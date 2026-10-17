// Download Folder Butler
// A tray app that sorts your Downloads folder into subfolders by file type.
//
// Build (MSVC, from a Developer Command Prompt):
//   rc /nologo DownloadButler.rc
//   cl /EHsc /std:c++17 /O2 DownloadButler.cpp DownloadButler.res /link /SUBSYSTEM:WINDOWS
//
// Build (MinGW-w64):
//   windres DownloadButler.rc -O coff -o DownloadButler.res.o
//   g++ -std=c++17 -O2 -municode -mwindows DownloadButler.cpp DownloadButler.res.o -o DownloadButler.exe
//       -lcomctl32 -lcomdlg32 -lshell32 -lole32 -ladvapi32 -luuid -static   (all on one line)
//
// Config lives in %APPDATA%\DownloadButler\config.ini

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <commdlg.h>
#include "resource.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------- constants
static const wchar_t* APP_NAME   = L"Download Folder Butler";
static const wchar_t* WND_CLASS  = L"DownloadButlerWnd";
static const wchar_t* RUN_KEY    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

enum {
    WM_TRAYICON = WM_APP + 1,
    WM_JOB_STATUS,   // worker -> UI: progress text (lParam = new std::wstring*)
    WM_JOB_DONE,     // worker -> UI: finished   (lParam = new JobResult*)

    IDC_PATH = 100, IDC_BROWSE, IDC_LIST, IDC_NAME, IDC_EXTS, IDC_ADD,
    IDC_REMOVE, IDC_AUTO, IDC_STARTUP, IDC_SORTNOW, IDC_STATUS,
    IDC_TAB_RULES, IDC_TAB_PROCS, IDC_HINT,
    IDC_PLIST, IDC_PEXTS, IDC_PEXE, IDC_PARGS, IDC_PBROWSE, IDC_PADD, IDC_PUPDATE, IDC_PREMOVE,

    ID_TIMER_SORT = 1,

    IDM_OPEN = 200, IDM_SORTNOW, IDM_AUTO, IDM_EXIT
};

// ---------------------------------------------------------------- data
struct Rule {
    std::wstring folder;  // destination folder (relative to download folder, or absolute)
    std::wstring exts;    // comma separated, lowercase, no dots
};

// A processor hands matching files to an external program before they are sorted.
struct Processor {
    std::wstring exts;    // comma separated, lowercase, no dots
    std::wstring exe;     // program to run (.exe), full path or something on PATH
    std::wstring args;    // argument template; {file} {dir} {name} {ext}. Empty = "{file}"
    bool enabled = true;
};

struct Config {
    std::wstring downloadDir;
    bool autoSort = true;
    int processorTimeoutSec = 120;
    std::vector<Rule> rules;
    std::vector<Processor> processors;
};

static Config g_cfg;
static HWND g_hwnd, g_path, g_list, g_name, g_exts, g_auto, g_startup, g_status;
static HWND g_tabRules, g_tabProcs, g_hint, g_plist, g_pexts, g_pexe, g_pargs;
static std::vector<HWND> g_pageRules, g_pageProcs;   // controls belonging to each page
static bool g_refreshing = false;                    // suppress list notifications while rebuilding
static HANDLE g_worker = nullptr;                    // background sort/processor thread
static std::atomic<bool> g_busy{ false };
static std::atomic<bool> g_quit{ false };
static NOTIFYICONDATAW g_nid{};
static UINT g_taskbarCreated;
static std::wstring g_iniPath;

// ---------------------------------------------------------------- helpers
static std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// "  .JPG; png  gif" -> "jpg,png,gif"
static std::wstring NormalizeExts(const std::wstring& in) {
    std::wstring out, cur;
    auto flush = [&] {
        if (!cur.empty()) { if (!out.empty()) out += L","; out += cur; cur.clear(); }
    };
    for (wchar_t c : in) {
        if (c == L',' || c == L';' || c == L' ' || c == L'\t') flush();
        else if (c != L'.' && c != L'*') cur += (wchar_t)towlower(c);
    }
    flush();
    return out;
}

static bool ExtInList(const std::wstring& ext, const std::wstring& list) {
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(L',', pos);
        if (comma == std::wstring::npos) comma = list.size();
        if (list.compare(pos, comma - pos, ext) == 0 && comma - pos == ext.size()) return true;
        pos = comma + 1;
    }
    return false;
}

static std::wstring StripQuotes(std::wstring s) {
    while (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = Trim(s.substr(1, s.size() - 2));
    return s;
}

// GetPrivateProfileString silently strips quotes around a value, which would mangle
// argument templates like "{file}". Wrap such values in |...| on disk to keep them intact.
static std::wstring IniWrap(const std::wstring& s) { return L"|" + s + L"|"; }
static std::wstring IniUnwrap(const std::wstring& s) {
    if (s.size() >= 2 && s.front() == L'|' && s.back() == L'|') return s.substr(1, s.size() - 2);
    return s;
}

static void SetStatus(const std::wstring& s) { SetWindowTextW(g_status, s.c_str()); }

// ---------------------------------------------------------------- config
static std::wstring DefaultDownloads() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

static void SetDefaultRules() {
    g_cfg.rules = {
        { L"Images",        L"jpg,jpeg,png,gif,bmp,webp,svg,heic,tiff,ico" },
        { L"Documents",     L"pdf,doc,docx,txt,rtf,odt,md" },
        { L"Spreadsheets",  L"xls,xlsx,csv,ods" },
        { L"Presentations", L"ppt,pptx,odp" },
        { L"Videos",        L"mp4,mkv,avi,mov,wmv,webm,flv" },
        { L"Music",         L"mp3,wav,flac,aac,ogg,m4a,wma" },
        { L"Archives",      L"zip,rar,7z,tar,gz,bz2,xz" },
        { L"Installers",    L"exe,msi,msix,dmg,iso" },
    };
}

static void InitIniPath() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    dir += L"\\DownloadButler";
    CreateDirectoryW(dir.c_str(), nullptr);
    g_iniPath = dir + L"\\config.ini";
}

static std::wstring IniGet(const wchar_t* sec, const std::wstring& key, const wchar_t* def = L"") {
    wchar_t buf[4096];
    GetPrivateProfileStringW(sec, key.c_str(), def, buf, 4096, g_iniPath.c_str());
    return buf;
}

static void LoadConfig() {
    g_cfg.downloadDir = IniGet(L"General", L"DownloadFolder");
    if (g_cfg.downloadDir.empty()) g_cfg.downloadDir = DefaultDownloads();
    g_cfg.autoSort = GetPrivateProfileIntW(L"General", L"AutoSort", 1, g_iniPath.c_str()) != 0;
    g_cfg.processorTimeoutSec = (int)GetPrivateProfileIntW(L"General", L"ProcessorTimeoutSec", 120, g_iniPath.c_str());
    if (g_cfg.processorTimeoutSec < 1 || g_cfg.processorTimeoutSec > 86400) g_cfg.processorTimeoutSec = 120;

    g_cfg.processors.clear();
    int pn = GetPrivateProfileIntW(L"Processors", L"Count", 0, g_iniPath.c_str());
    for (int i = 0; i < pn; ++i) {
        std::wstring k = std::to_wstring(i);
        Processor pr;
        pr.exts    = NormalizeExts(IniGet(L"Processors", L"Exts" + k));
        pr.exe     = StripQuotes(Trim(IniUnwrap(IniGet(L"Processors", L"Exe" + k))));
        pr.args    = IniUnwrap(IniGet(L"Processors", L"Args" + k));
        pr.enabled = GetPrivateProfileIntW(L"Processors", (L"Enabled" + k).c_str(), 1, g_iniPath.c_str()) != 0;
        if (!pr.exts.empty() && !pr.exe.empty()) g_cfg.processors.push_back(pr);
    }

    int n = GetPrivateProfileIntW(L"Rules", L"Count", -1, g_iniPath.c_str());
    g_cfg.rules.clear();
    if (n < 0) { SetDefaultRules(); return; }
    for (int i = 0; i < n; ++i) {
        Rule r;
        r.folder = IniGet(L"Rules", L"Folder" + std::to_wstring(i));
        r.exts   = IniGet(L"Rules", L"Exts" + std::to_wstring(i));
        if (!r.folder.empty()) g_cfg.rules.push_back(r);
    }
}

static void SaveConfig() {
    WritePrivateProfileStringW(L"General", L"DownloadFolder", g_cfg.downloadDir.c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"AutoSort", g_cfg.autoSort ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"General", L"ProcessorTimeoutSec", std::to_wstring(g_cfg.processorTimeoutSec).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"Processors", nullptr, nullptr, g_iniPath.c_str());  // wipe section
    WritePrivateProfileStringW(L"Processors", L"Count", std::to_wstring(g_cfg.processors.size()).c_str(), g_iniPath.c_str());
    for (size_t i = 0; i < g_cfg.processors.size(); ++i) {
        std::wstring k = std::to_wstring(i);
        const Processor& pr = g_cfg.processors[i];
        WritePrivateProfileStringW(L"Processors", (L"Exts" + k).c_str(), pr.exts.c_str(), g_iniPath.c_str());
        WritePrivateProfileStringW(L"Processors", (L"Exe" + k).c_str(), IniWrap(pr.exe).c_str(), g_iniPath.c_str());
        WritePrivateProfileStringW(L"Processors", (L"Args" + k).c_str(), IniWrap(pr.args).c_str(), g_iniPath.c_str());
        WritePrivateProfileStringW(L"Processors", (L"Enabled" + k).c_str(), pr.enabled ? L"1" : L"0", g_iniPath.c_str());
    }
    WritePrivateProfileStringW(L"Rules", nullptr, nullptr, g_iniPath.c_str());  // wipe section
    WritePrivateProfileStringW(L"Rules", L"Count", std::to_wstring(g_cfg.rules.size()).c_str(), g_iniPath.c_str());
    for (size_t i = 0; i < g_cfg.rules.size(); ++i) {
        WritePrivateProfileStringW(L"Rules", (L"Folder" + std::to_wstring(i)).c_str(), g_cfg.rules[i].folder.c_str(), g_iniPath.c_str());
        WritePrivateProfileStringW(L"Rules", (L"Exts" + std::to_wstring(i)).c_str(), g_cfg.rules[i].exts.c_str(), g_iniPath.c_str());
    }
}

// ---------------------------------------------------------------- run at startup
static bool GetRunAtStartup() {
    HKEY k;
    bool r = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        r = RegQueryValueExW(k, L"DownloadButler", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return r;
}

static void SetRunAtStartup(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, L"DownloadButler", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"DownloadButler");
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- sorting
static bool IsPartialDownload(const std::wstring& ext) {
    static const wchar_t* partial[] = { L"crdownload", L"part", L"tmp", L"download", L"opdownload", L"partial" };
    for (auto p : partial) if (ext == p) return true;
    return false;
}

// True if the file was last modified more than `seconds` ago (so it's not still being written).
static bool IsSettled(const fs::path& p, int seconds) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) return false;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = now.dwLowDateTime;               a.HighPart = now.dwHighDateTime;
    b.LowPart = d.ftLastWriteTime.dwLowDateTime; b.HighPart = d.ftLastWriteTime.dwHighDateTime;
    if (a.QuadPart < b.QuadPart) return false;
    return (a.QuadPart - b.QuadPart) > (ULONGLONG)seconds * 10000000ULL;
}

static fs::path UniquePath(const fs::path& target) {
    std::error_code ec;
    if (!fs::exists(target, ec)) return target;
    fs::path dir = target.parent_path(), stem = target.stem(), ext = target.extension();
    for (int i = 1; i < 10000; ++i) {
        fs::path c = dir / (stem.wstring() + L" (" + std::to_wstring(i) + L")" + ext.wstring());
        if (!fs::exists(c, ec)) return c;
    }
    return target;
}

// ---------------------------------------------------------------- processors
static bool IsPathLike(const std::wstring& s) { return s.find_first_of(L"\\/:") != std::wstring::npos; }

static bool HasExeExtension(const std::wstring& exe) {
    return ToLower(fs::path(exe).extension().wstring()) == L".exe";
}

// Expands {file} {dir} {name} {ext} in a single pass, so text that happens to appear
// inside a file name (e.g. a file called "{dir}.webp") is never expanded twice.
static std::wstring ExpandArgs(const std::wstring& tmplIn, const fs::path& file) {
    std::wstring tmpl = Trim(tmplIn).empty() ? std::wstring(L"\"{file}\"") : tmplIn;
    std::wstring ext = file.extension().wstring();
    if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
    std::wstring dir = file.parent_path().wstring();
    if (!dir.empty() && dir.back() == L'\\') dir += L'\\';   // "D:\" before a closing quote would escape it

    struct Tok { const wchar_t* name; std::wstring value; };
    const Tok toks[] = {
        { L"{file}", file.wstring() }, { L"{dir}", dir }, { L"{name}", file.stem().wstring() }, { L"{ext}", ext },
    };
    std::wstring out;
    for (size_t i = 0; i < tmpl.size();) {
        bool hit = false;
        if (tmpl[i] == L'{') {
            for (const Tok& t : toks) {
                size_t n = wcslen(t.name);
                if (tmpl.compare(i, n, t.name) == 0) { out += t.value; i += n; hit = true; break; }
            }
        }
        if (!hit) out += tmpl[i++];
    }
    return out;
}

enum class RunResult { Ok, Failed, Aborted };

// Runs one processor on one file and waits for it (with a timeout). The program is started
// directly (no shell), so characters in file names can't be interpreted as shell syntax.
static RunResult RunProcessor(const Processor& p, const fs::path& file, int timeoutSec, std::wstring& err) {
    std::wstring prog = fs::path(p.exe).filename().wstring();
    std::wstring cmd = L"\"" + p.exe + L"\" " + ExpandArgs(p.args, file);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cwd = file.parent_path().wstring();
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) {
        err = L"could not start " + prog + L" (error " + std::to_wstring(GetLastError()) + L")";
        return RunResult::Failed;
    }
    CloseHandle(pi.hThread);

    RunResult rr = RunResult::Ok;
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeoutSec * 1000ULL;
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) break;
        if (g_quit) { rr = RunResult::Aborted; break; }   // app is exiting; leave the program running
        if (GetTickCount64() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            err = prog + L" timed out after " + std::to_wstring(timeoutSec) + L"s";
            rr = RunResult::Failed;
            break;
        }
    }
    if (rr == RunResult::Ok) {
        DWORD code = 0;
        if (GetExitCodeProcess(pi.hProcess, &code) && code != 0) {
            err = prog + L" exited with code " + std::to_wstring(code);
            rr = RunResult::Failed;
        }
    }
    CloseHandle(pi.hProcess);
    return rr;
}

// ---------------------------------------------------------------- sorting
struct JobResult {
    int moved = 0;
    int processed = 0;     // successful processor runs
    int failed = 0;        // files whose processor failed
    bool badFolder = false;
    bool manual = false;
    std::wstring error;    // first failure, for display
};

// What we last did to a file that is still in the folder, so a processor doesn't run on
// it again every five seconds. Only touched by the worker thread.
struct ProcState { ULONGLONG size = 0, mtime = 0; bool failed = false; };
static std::map<std::wstring, ProcState> g_processed;

static bool GetFileState(const fs::path& p, ProcState& s) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return false;
    s.size  = ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    s.mtime = ((ULONGLONG)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    return true;
}

static void PostStatus(const std::wstring& text) {
    if (!g_hwnd) return;
    auto* msg = new std::wstring(text);
    if (!PostMessageW(g_hwnd, WM_JOB_STATUS, 0, (LPARAM)msg)) delete msg;
}

// Runs processors, then sorts. Works on its own copy of the config (it runs on a worker thread).
static void SortDownloads(const Config& cfg, bool settleCheck, JobResult& res) {
    std::error_code ec;
    fs::path root = cfg.downloadDir;
    if (root.empty() || !fs::is_directory(root, ec)) { res.badFolder = true; return; }

    std::vector<fs::path> files;
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) files.push_back(it->path());
    }

    // Forget files that have left the folder.
    {
        std::set<std::wstring> present;
        for (const auto& f : files) present.insert(f.wstring());
        for (auto it = g_processed.begin(); it != g_processed.end();)
            it = present.count(it->first) ? std::next(it) : g_processed.erase(it);
    }

    for (const auto& f : files) {
        if (g_quit) return;
        std::wstring name = ToLower(f.filename().wstring());
        if (name == L"desktop.ini") continue;
        std::wstring ext = ToLower(f.extension().wstring());
        if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
        if (ext.empty() || IsPartialDownload(ext)) continue;
        if (settleCheck && !IsSettled(f, 3)) continue;

        const Rule* match = nullptr;
        for (const auto& r : cfg.rules)
            if (ExtInList(ext, r.exts)) { match = &r; break; }

        std::vector<const Processor*> procs;
        for (const auto& pr : cfg.processors)
            if (pr.enabled && !pr.exe.empty() && ExtInList(ext, pr.exts)) procs.push_back(&pr);

        if (!match && procs.empty()) continue;

        // ---- processors: every enabled match runs, in list order, before sorting
        if (!procs.empty()) {
            ProcState cur;
            if (!GetFileState(f, cur)) continue;
            auto prev = g_processed.find(f.wstring());
            bool seen = prev != g_processed.end() && prev->second.size == cur.size && prev->second.mtime == cur.mtime;
            if (seen && prev->second.failed) continue;   // failed on this exact file; wait until it changes

            if (!seen) {
                bool ok = true;
                for (const Processor* pr : procs) {
                    PostStatus(L"Running " + fs::path(pr->exe).filename().wstring() + L" on " + f.filename().wstring() + L"...");
                    std::wstring err;
                    RunResult rr = RunProcessor(*pr, f, cfg.processorTimeoutSec, err);
                    if (rr == RunResult::Aborted) return;
                    if (rr == RunResult::Failed) {
                        ++res.failed;
                        if (res.error.empty()) res.error = f.filename().wstring() + L": " + err;
                        ok = false;
                        break;
                    }
                    ++res.processed;
                }
                std::error_code e;
                if (!fs::exists(f, e)) { g_processed.erase(f.wstring()); continue; }   // the program consumed/renamed it
                ProcState after;
                after.failed = !ok;
                if (!GetFileState(f, after)) continue;   // record the file as the program left it
                g_processed[f.wstring()] = after;
                if (!ok) continue;                       // leave a failed file where it is
            }
        }

        // ---- sorting
        if (!match) continue;
        fs::path destDir = root / match->folder;  // an absolute rule folder replaces root
        std::error_code e;
        fs::create_directories(destDir, e);
        if (e) continue;
        fs::path dest = UniquePath(destDir / f.filename());

        fs::rename(f, dest, e);
        if (e) {  // e.g. different drive
            fs::copy_file(f, dest, fs::copy_options::none, e);
            if (!e) fs::remove(f, e);
            else continue;
        }
        ++res.moved;
    }
}

struct JobCtx { Config cfg; bool manual; };

static DWORD WINAPI JobThread(LPVOID param) {
    std::unique_ptr<JobCtx> ctx(static_cast<JobCtx*>(param));
    auto* res = new JobResult();
    res->manual = ctx->manual;
    try {
        SortDownloads(ctx->cfg, !ctx->manual, *res);
    } catch (...) {
        res->error = L"unexpected error while sorting";
        ++res->failed;
    }
    if (!PostMessageW(g_hwnd, WM_JOB_DONE, 0, (LPARAM)res)) delete res;
    return 0;
}

// Starts a sort (and any processors) in the background. Only one job runs at a time.
static void StartJob(bool manual) {
    if (g_busy) {
        if (manual) SetStatus(L"Butler is still busy with the previous run - try again in a moment.");
        return;
    }
    if (g_worker) { WaitForSingleObject(g_worker, INFINITE); CloseHandle(g_worker); g_worker = nullptr; }
    auto* ctx = new JobCtx{ g_cfg, manual };
    g_busy = true;
    g_worker = CreateThread(nullptr, 0, JobThread, ctx, 0, nullptr);
    if (!g_worker) {
        delete ctx;
        g_busy = false;
        if (manual) SetStatus(L"Could not start the sorting job.");
    }
}

static void ShowBalloon(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    n.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void ReportJob(const JobResult& r) {
    if (r.badFolder) {
        if (r.manual) SetStatus(L"Download folder doesn't exist - please choose a valid one.");
        return;
    }
    std::wstring s;
    if (r.manual || r.moved > 0)
        s = std::wstring(r.manual ? L"Sorted " : L"Auto-sorted ") + std::to_wstring(r.moved) + L" file" + (r.moved == 1 ? L"." : L"s.");
    if (r.processed > 0) {
        if (!s.empty()) s += L" ";
        s += L"Ran " + std::to_wstring(r.processed) + L" processor task" + (r.processed == 1 ? L"." : L"s.");
    }
    if (r.failed > 0) {
        if (!s.empty()) s += L" ";
        s += L"Processor failed - " + r.error + (r.failed > 1 ? L" (+" + std::to_wstring(r.failed - 1) + L" more)" : L"");
        ShowBalloon(APP_NAME, L"Processor failed: " + r.error);
    }
    if (!s.empty()) SetStatus(s);
}

// ---------------------------------------------------------------- UI <-> config
static void RefreshList() {
    ListView_DeleteAllItems(g_list);
    for (size_t i = 0; i < g_cfg.rules.size(); ++i) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = const_cast<wchar_t*>(g_cfg.rules[i].folder.c_str());
        ListView_InsertItem(g_list, &it);
        ListView_SetItemText(g_list, (int)i, 1, const_cast<wchar_t*>(g_cfg.rules[i].exts.c_str()));
    }
}

static void ApplyPathFromUi() {
    g_cfg.downloadDir = Trim(GetText(g_path));
    SaveConfig();
}

static void UpdateTrayTooltip() {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_TIP;
    wcscpy_s(n.szTip, g_cfg.autoSort ? L"Download Folder Butler (auto-sort on)" : L"Download Folder Butler (auto-sort off)");
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void SetAutoSort(bool on) {
    g_cfg.autoSort = on;
    SendMessageW(g_auto, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    SaveConfig();
    UpdateTrayTooltip();
}

static void ShowMainWindow() {
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

static void HideMainWindow() {
    ApplyPathFromUi();
    ShowWindow(g_hwnd, SW_HIDE);
}

static void BrowseForFolder() {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = L"Choose the folder to keep tidy";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            SetWindowTextW(g_path, path);
            ApplyPathFromUi();
            SetStatus(L"Download folder set.");
        }
        CoTaskMemFree(pidl);
    }
}

static void AddOrUpdateRule() {
    std::wstring name = Trim(GetText(g_name));
    std::wstring exts = NormalizeExts(GetText(g_exts));
    if (name.empty() || exts.empty()) {
        SetStatus(L"Enter a folder name and at least one extension.");
        return;
    }
    bool found = false;
    for (auto& r : g_cfg.rules)
        if (ToLower(r.folder) == ToLower(name)) { r.exts = exts; found = true; break; }
    if (!found) g_cfg.rules.push_back({ name, exts });
    SaveConfig();
    RefreshList();
    SetStatus(found ? L"Rule updated." : L"Rule added.");
}

static void RemoveSelectedRule() {
    int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)g_cfg.rules.size()) { SetStatus(L"Select a rule to remove."); return; }
    g_cfg.rules.erase(g_cfg.rules.begin() + sel);
    SaveConfig();
    RefreshList();
    SetWindowTextW(g_name, L"");
    SetWindowTextW(g_exts, L"");
    SetStatus(L"Rule removed.");
}

// ---------------------------------------------------------------- icons
// Loads the embedded app icon at an exact size (falls back to the stock icon).
static HICON LoadAppIcon(int cx, int cy) {
    HICON h = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR | LR_SHARED);
    return h ? h : LoadIconW(nullptr, IDI_APPLICATION);
}

// ---------------------------------------------------------------- processors UI
static void RefreshProcList() {
    g_refreshing = true;
    ListView_DeleteAllItems(g_plist);
    for (size_t i = 0; i < g_cfg.processors.size(); ++i) {
        const Processor& pr = g_cfg.processors[i];
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = const_cast<wchar_t*>(pr.exts.c_str());
        ListView_InsertItem(g_plist, &it);
        ListView_SetItemText(g_plist, (int)i, 1, const_cast<wchar_t*>(pr.exe.c_str()));
        ListView_SetItemText(g_plist, (int)i, 2, const_cast<wchar_t*>(pr.args.c_str()));
        ListView_SetCheckState(g_plist, (int)i, pr.enabled);
    }
    g_refreshing = false;
}

// Reads and validates the processor fields. Returns false (with a status message) if invalid.
static bool ReadProcessorFields(Processor& pr) {
    pr.exts = NormalizeExts(GetText(g_pexts));
    pr.exe  = StripQuotes(Trim(GetText(g_pexe)));
    pr.args = Trim(GetText(g_pargs));
    if (pr.exts.empty() || pr.exe.empty()) {
        SetStatus(L"Enter at least one extension and the program to run.");
        return false;
    }
    if (!HasExeExtension(pr.exe)) {
        SetStatus(L"The program must be an .exe file (use powershell.exe or cmd.exe for scripts).");
        return false;
    }
    if (IsPathLike(pr.exe)) {
        std::error_code ec;
        if (!fs::is_regular_file(pr.exe, ec)) { SetStatus(L"Program not found: " + pr.exe); return false; }
    }
    return true;
}

static void AddProcessor() {
    Processor pr;
    if (!ReadProcessorFields(pr)) return;
    g_cfg.processors.push_back(pr);
    SaveConfig();
    RefreshProcList();
    SetStatus(L"Processor added.");
}

static void UpdateSelectedProcessor() {
    int sel = ListView_GetNextItem(g_plist, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)g_cfg.processors.size()) { SetStatus(L"Select a processor to update (or use Add)."); return; }
    Processor pr;
    if (!ReadProcessorFields(pr)) return;
    pr.enabled = g_cfg.processors[sel].enabled;
    g_cfg.processors[sel] = pr;
    SaveConfig();
    RefreshProcList();
    ListView_SetItemState(g_plist, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SetStatus(L"Processor updated.");
}

static void RemoveSelectedProcessor() {
    int sel = ListView_GetNextItem(g_plist, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= (int)g_cfg.processors.size()) { SetStatus(L"Select a processor to remove."); return; }
    g_cfg.processors.erase(g_cfg.processors.begin() + sel);
    SaveConfig();
    RefreshProcList();
    SetWindowTextW(g_pexts, L"");
    SetWindowTextW(g_pexe, L"");
    SetWindowTextW(g_pargs, L"");
    SetStatus(L"Processor removed.");
}

static void BrowseForExe() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose the program to run";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_pexe, file);
}

static void ShowPage(int page) {   // 0 = sorting rules, 1 = processors
    for (HWND h : g_pageRules) ShowWindow(h, page == 0 ? SW_SHOW : SW_HIDE);
    for (HWND h : g_pageProcs) ShowWindow(h, page == 1 ? SW_SHOW : SW_HIDE);
    SendMessageW(g_tabRules, BM_SETCHECK, page == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_tabProcs, BM_SETCHECK, page == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(g_hint, page == 0 ? L"First matching rule wins."
                                     : L"Matching processors run before the file is sorted.");
}

// ---------------------------------------------------------------- tray
static void AddTrayIcon() {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    wcscpy_s(g_nid.szTip, L"Download Folder Butler");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"Open Butler");
    AppendMenuW(m, MF_STRING, IDM_SORTNOW, L"Sort now");
    AppendMenuW(m, MF_STRING | (g_cfg.autoSort ? MF_CHECKED : 0), IDM_AUTO, L"Auto-sort");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ---------------------------------------------------------------- window
static HWND MakeCtl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

static BOOL CALLBACK SetFontProc(HWND h, LPARAM f) { SendMessageW(h, WM_SETFONT, f, TRUE); return TRUE; }

static void BuildUi() {
    auto onRules = [](HWND h) { g_pageRules.push_back(h); return h; };
    auto onProcs = [](HWND h) { g_pageProcs.push_back(h); return h; };

    MakeCtl(L"STATIC", L"Folder to keep tidy:", 0, 12, 12, 200, 18, 0);
    g_path = MakeCtl(L"EDIT", g_cfg.downloadDir.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 12, 32, 436, 24, IDC_PATH, WS_EX_CLIENTEDGE);
    MakeCtl(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 456, 31, 80, 26, IDC_BROWSE);

    // Page switcher
    g_tabRules = MakeCtl(L"BUTTON", L"Sorting rules", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP | WS_TABSTOP, 12, 66, 110, 26, IDC_TAB_RULES);
    g_tabProcs = MakeCtl(L"BUTTON", L"Processors", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_TABSTOP, 126, 66, 110, 26, IDC_TAB_PROCS);
    g_hint = MakeCtl(L"STATIC", L"", SS_RIGHT, 244, 72, 292, 18, IDC_HINT);

    // ---- Page 0: sorting rules
    g_list = onRules(MakeCtl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, 12, 100, 524, 150, IDC_LIST, WS_EX_CLIENTEDGE));
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.cx = 140; c.pszText = const_cast<wchar_t*>(L"Folder");     ListView_InsertColumn(g_list, 0, &c);
    c.cx = 360; c.pszText = const_cast<wchar_t*>(L"Extensions"); ListView_InsertColumn(g_list, 1, &c);

    onRules(MakeCtl(L"STATIC", L"Folder name (or full path):", 0, 12, 258, 170, 18, 0));
    onRules(MakeCtl(L"STATIC", L"Extensions (comma separated):", 0, 190, 258, 250, 18, 0));
    g_name = onRules(MakeCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 12, 278, 166, 24, IDC_NAME, WS_EX_CLIENTEDGE));
    g_exts = onRules(MakeCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 190, 278, 346, 24, IDC_EXTS, WS_EX_CLIENTEDGE));
    onRules(MakeCtl(L"BUTTON", L"Add / Update rule", BS_PUSHBUTTON | WS_TABSTOP, 12, 310, 130, 28, IDC_ADD));
    onRules(MakeCtl(L"BUTTON", L"Remove selected", BS_PUSHBUTTON | WS_TABSTOP, 150, 310, 130, 28, IDC_REMOVE));

    // ---- Page 1: processors
    g_plist = onProcs(MakeCtl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, 12, 100, 524, 118, IDC_PLIST, WS_EX_CLIENTEDGE));
    ListView_SetExtendedListViewStyle(g_plist, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES);
    c.cx = 125; c.pszText = const_cast<wchar_t*>(L"Extensions"); ListView_InsertColumn(g_plist, 0, &c);
    c.cx = 190; c.pszText = const_cast<wchar_t*>(L"Program");    ListView_InsertColumn(g_plist, 1, &c);
    c.cx = 180; c.pszText = const_cast<wchar_t*>(L"Arguments");  ListView_InsertColumn(g_plist, 2, &c);

    onProcs(MakeCtl(L"STATIC", L"Extensions (comma separated):", 0, 12, 226, 170, 18, 0));
    onProcs(MakeCtl(L"STATIC", L"Program to run (.exe):", 0, 190, 226, 250, 18, 0));
    g_pexts = onProcs(MakeCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 12, 244, 166, 24, IDC_PEXTS, WS_EX_CLIENTEDGE));
    g_pexe  = onProcs(MakeCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 190, 244, 256, 24, IDC_PEXE, WS_EX_CLIENTEDGE));
    onProcs(MakeCtl(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 452, 243, 84, 26, IDC_PBROWSE));
    onProcs(MakeCtl(L"STATIC", L"Arguments - {file} = full path; also {dir} {name} {ext} (blank = \"{file}\"):", 0, 12, 274, 524, 18, 0));
    g_pargs = onProcs(MakeCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 12, 292, 524, 24, IDC_PARGS, WS_EX_CLIENTEDGE));
    onProcs(MakeCtl(L"BUTTON", L"Add", BS_PUSHBUTTON | WS_TABSTOP, 12, 324, 90, 28, IDC_PADD));
    onProcs(MakeCtl(L"BUTTON", L"Update selected", BS_PUSHBUTTON | WS_TABSTOP, 110, 324, 120, 28, IDC_PUPDATE));
    onProcs(MakeCtl(L"BUTTON", L"Remove selected", BS_PUSHBUTTON | WS_TABSTOP, 238, 324, 120, 28, IDC_PREMOVE));

    // ---- Always visible
    g_auto = MakeCtl(L"BUTTON", L"Auto-sort new downloads in the background", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 366, 340, 20, IDC_AUTO);
    g_startup = MakeCtl(L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 390, 200, 20, IDC_STARTUP);
    MakeCtl(L"BUTTON", L"Sort now", BS_DEFPUSHBUTTON | WS_TABSTOP, 436, 370, 100, 36, IDC_SORTNOW);

    g_status = MakeCtl(L"STATIC", L"Ready. Closing this window keeps Butler running in the tray.", SS_LEFT, 12, 420, 524, 18, IDC_STATUS);

    SendMessageW(g_auto, BM_SETCHECK, g_cfg.autoSort ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_startup, BM_SETCHECK, GetRunAtStartup() ? BST_CHECKED : BST_UNCHECKED, 0);
    EnumChildWindows(g_hwnd, SetFontProc, (LPARAM)GetStockObject(DEFAULT_GUI_FONT));
    RefreshList();
    RefreshProcList();
    ShowPage(0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated && msg == g_taskbarCreated) { AddTrayIcon(); UpdateTrayTooltip(); return 0; }

    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        BuildUi();
        AddTrayIcon();
        UpdateTrayTooltip();
        SetTimer(hwnd, ID_TIMER_SORT, 5000, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == ID_TIMER_SORT && g_cfg.autoSort) StartJob(false);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE:  BrowseForFolder(); break;
        case IDC_ADD:     AddOrUpdateRule(); break;
        case IDC_REMOVE:  RemoveSelectedRule(); break;
        case IDC_SORTNOW: ApplyPathFromUi(); StartJob(true); break;
        case IDC_TAB_RULES: ShowPage(0); break;
        case IDC_TAB_PROCS: ShowPage(1); break;
        case IDC_PADD:    AddProcessor(); break;
        case IDC_PUPDATE: UpdateSelectedProcessor(); break;
        case IDC_PREMOVE: RemoveSelectedProcessor(); break;
        case IDC_PBROWSE: BrowseForExe(); break;
        case IDC_AUTO:    SetAutoSort(SendMessageW(g_auto, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case IDC_STARTUP: SetRunAtStartup(SendMessageW(g_startup, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case IDC_PATH:    if (HIWORD(wp) == EN_KILLFOCUS) ApplyPathFromUi(); break;
        case IDM_OPEN:    ShowMainWindow(); break;
        case IDM_SORTNOW: StartJob(true); break;
        case IDM_AUTO:    SetAutoSort(!g_cfg.autoSort); break;
        case IDM_EXIT:    DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_NOTIFY: {
        auto* nm = (NMLISTVIEW*)lp;
        if (nm->hdr.code == LVN_ITEMCHANGED && (nm->uChanged & LVIF_STATE) && !g_refreshing) {
            if (nm->hdr.idFrom == IDC_LIST) {
                if ((nm->uNewState & LVIS_SELECTED) && nm->iItem >= 0 && nm->iItem < (int)g_cfg.rules.size()) {
                    SetWindowTextW(g_name, g_cfg.rules[nm->iItem].folder.c_str());
                    SetWindowTextW(g_exts, g_cfg.rules[nm->iItem].exts.c_str());
                }
            } else if (nm->hdr.idFrom == IDC_PLIST && nm->iItem >= 0 && nm->iItem < (int)g_cfg.processors.size()) {
                Processor& pr = g_cfg.processors[nm->iItem];
                if ((nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED)) {
                    SetWindowTextW(g_pexts, pr.exts.c_str());
                    SetWindowTextW(g_pexe, pr.exe.c_str());
                    SetWindowTextW(g_pargs, pr.args.c_str());
                }
                if ((nm->uOldState ^ nm->uNewState) & LVIS_STATEIMAGEMASK) {   // checkbox toggled
                    bool on = ((nm->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                    if (pr.enabled != on) { pr.enabled = on; SaveConfig(); }
                }
            }
        }
        return 0;
    }

    case WM_JOB_STATUS: {
        std::unique_ptr<std::wstring> text((std::wstring*)lp);
        SetStatus(*text);
        return 0;
    }

    case WM_JOB_DONE: {
        std::unique_ptr<JobResult> res((JobResult*)lp);
        if (g_worker) { WaitForSingleObject(g_worker, INFINITE); CloseHandle(g_worker); g_worker = nullptr; }
        g_busy = false;
        ReportJob(*res);
        return 0;
    }

    case WM_TRAYICON:
        if (LOWORD(lp) == WM_LBUTTONUP) {
            if (IsWindowVisible(hwnd)) HideMainWindow(); else ShowMainWindow();
        } else if (LOWORD(lp) == WM_RBUTTONUP) {
            ShowTrayMenu();
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) { HideMainWindow(); return 0; }
        break;

    case WM_CLOSE:
        HideMainWindow();   // keep running in the tray
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER_SORT);
        g_quit = true;   // tell the worker to stop; give it a moment to wind down
        if (g_worker) { WaitForSingleObject(g_worker, 5000); CloseHandle(g_worker); g_worker = nullptr; }
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Single instance
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"DownloadFolderButler_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Download Folder Butler is already running - look in the system tray.", APP_NAME, MB_ICONINFORMATION);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    InitIniPath();
    LoadConfig();
    SaveConfig();
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    wc.lpszClassName = WND_CLASS;
    RegisterClassW(&wc);

    // Fixed-size window with a ~548x446 client area
    RECT r{ 0, 0, 548, 446 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND hwndMain = CreateWindowExW(0, WND_CLASS, APP_NAME, style, x, y, w, h, nullptr, nullptr, hInst, nullptr);
    if (hwndMain) {
        // Small icon = title bar (top-left); big icon = taskbar button / Alt+Tab
        SendMessageW(hwndMain, WM_SETICON, ICON_SMALL, (LPARAM)LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON)));
        SendMessageW(hwndMain, WM_SETICON, ICON_BIG, (LPARAM)LoadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON)));
    }
    // Starts hidden in the tray; click the tray icon to open.

    // Let the user know it's alive
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wcscpy_s(n.szInfoTitle, APP_NAME);
    wcscpy_s(n.szInfo, L"Running in the tray. Click the icon to configure.");
    n.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &n);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return (int)msg.wParam;
}
