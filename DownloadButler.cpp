// Download Folder Butler
// A tray app that sorts your Downloads folder into subfolders by file type.
//
// Build (MSVC, from a Developer Command Prompt):
//   cl /EHsc /std:c++17 /O2 DownloadButler.cpp /link /SUBSYSTEM:WINDOWS
//
// Build (MinGW-w64):
//   g++ -std=c++17 -O2 -municode -mwindows DownloadButler.cpp -o DownloadButler.exe \
//       -lcomctl32 -lshell32 -lole32 -ladvapi32 -static
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

#include <algorithm>
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
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------- constants
static const wchar_t* APP_NAME   = L"Download Folder Butler";
static const wchar_t* WND_CLASS  = L"DownloadButlerWnd";
static const wchar_t* RUN_KEY    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

enum {
    WM_TRAYICON = WM_APP + 1,

    IDC_PATH = 100, IDC_BROWSE, IDC_LIST, IDC_NAME, IDC_EXTS, IDC_ADD,
    IDC_REMOVE, IDC_AUTO, IDC_STARTUP, IDC_SORTNOW, IDC_STATUS,

    ID_TIMER_SORT = 1,

    IDM_OPEN = 200, IDM_SORTNOW, IDM_AUTO, IDM_EXIT
};

// ---------------------------------------------------------------- data
struct Rule {
    std::wstring folder;  // destination folder (relative to download folder, or absolute)
    std::wstring exts;    // comma separated, lowercase, no dots
};

struct Config {
    std::wstring downloadDir;
    bool autoSort = true;
    std::vector<Rule> rules;
};

static Config g_cfg;
static HWND g_hwnd, g_path, g_list, g_name, g_exts, g_auto, g_startup, g_status;
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

// Returns number of files moved; -1 if folder is invalid.
static int SortDownloads(bool settleCheck) {
    std::error_code ec;
    fs::path root = g_cfg.downloadDir;
    if (root.empty() || !fs::is_directory(root, ec)) return -1;

    std::vector<fs::path> files;
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) files.push_back(it->path());
    }

    int moved = 0;
    for (const auto& f : files) {
        std::wstring name = ToLower(f.filename().wstring());
        if (name == L"desktop.ini") continue;
        std::wstring ext = ToLower(f.extension().wstring());
        if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
        if (ext.empty() || IsPartialDownload(ext)) continue;
        if (settleCheck && !IsSettled(f, 3)) continue;

        const Rule* match = nullptr;
        for (const auto& r : g_cfg.rules)
            if (ExtInList(ext, r.exts)) { match = &r; break; }
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
        ++moved;
    }
    return moved;
}

static void DoSort(bool manual) {
    int n = SortDownloads(!manual);
    if (n < 0) {
        if (manual) SetStatus(L"Download folder doesn't exist - please choose a valid one.");
    } else if (manual) {
        SetStatus(L"Sorted " + std::to_wstring(n) + L" file" + (n == 1 ? L"" : L"s") + L".");
    } else if (n > 0) {
        SetStatus(L"Auto-sorted " + std::to_wstring(n) + L" file" + (n == 1 ? L"" : L"s") + L".");
    }
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

// ---------------------------------------------------------------- tray
static void AddTrayIcon() {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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
    MakeCtl(L"STATIC", L"Folder to keep tidy:", 0, 12, 12, 200, 18, 0);
    g_path = MakeCtl(L"EDIT", g_cfg.downloadDir.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 12, 32, 436, 24, IDC_PATH, WS_EX_CLIENTEDGE);
    MakeCtl(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 456, 31, 80, 26, IDC_BROWSE);

    MakeCtl(L"STATIC", L"Sorting rules (first match wins):", 0, 12, 68, 300, 18, 0);
    g_list = MakeCtl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, 12, 88, 524, 200, IDC_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.cx = 140; c.pszText = const_cast<wchar_t*>(L"Folder");     ListView_InsertColumn(g_list, 0, &c);
    c.cx = 360; c.pszText = const_cast<wchar_t*>(L"Extensions"); ListView_InsertColumn(g_list, 1, &c);

    MakeCtl(L"STATIC", L"Folder name (or full path):", 0, 12, 298, 170, 18, 0);
    MakeCtl(L"STATIC", L"Extensions (comma separated):", 0, 190, 298, 250, 18, 0);
    g_name = MakeCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 12, 318, 166, 24, IDC_NAME, WS_EX_CLIENTEDGE);
    g_exts = MakeCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 190, 318, 346, 24, IDC_EXTS, WS_EX_CLIENTEDGE);
    MakeCtl(L"BUTTON", L"Add / Update rule", BS_PUSHBUTTON | WS_TABSTOP, 12, 350, 130, 28, IDC_ADD);
    MakeCtl(L"BUTTON", L"Remove selected", BS_PUSHBUTTON | WS_TABSTOP, 150, 350, 130, 28, IDC_REMOVE);

    g_auto = MakeCtl(L"BUTTON", L"Auto-sort new downloads in the background", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 392, 340, 20, IDC_AUTO);
    g_startup = MakeCtl(L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 416, 200, 20, IDC_STARTUP);
    MakeCtl(L"BUTTON", L"Sort now", BS_DEFPUSHBUTTON | WS_TABSTOP, 436, 396, 100, 36, IDC_SORTNOW);

    g_status = MakeCtl(L"STATIC", L"Ready. Closing this window keeps Butler running in the tray.", SS_LEFT, 12, 446, 524, 18, IDC_STATUS);

    SendMessageW(g_auto, BM_SETCHECK, g_cfg.autoSort ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_startup, BM_SETCHECK, GetRunAtStartup() ? BST_CHECKED : BST_UNCHECKED, 0);
    EnumChildWindows(g_hwnd, SetFontProc, (LPARAM)GetStockObject(DEFAULT_GUI_FONT));
    RefreshList();
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
        if (wp == ID_TIMER_SORT && g_cfg.autoSort) DoSort(false);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE:  BrowseForFolder(); break;
        case IDC_ADD:     AddOrUpdateRule(); break;
        case IDC_REMOVE:  RemoveSelectedRule(); break;
        case IDC_SORTNOW: ApplyPathFromUi(); DoSort(true); break;
        case IDC_AUTO:    SetAutoSort(SendMessageW(g_auto, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case IDC_STARTUP: SetRunAtStartup(SendMessageW(g_startup, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case IDC_PATH:    if (HIWORD(wp) == EN_KILLFOCUS) ApplyPathFromUi(); break;
        case IDM_OPEN:    ShowMainWindow(); break;
        case IDM_SORTNOW: DoSort(true); break;
        case IDM_AUTO:    SetAutoSort(!g_cfg.autoSort); break;
        case IDM_EXIT:    DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_NOTIFY: {
        auto* nm = (NMLISTVIEW*)lp;
        if (nm->hdr.idFrom == IDC_LIST && nm->hdr.code == LVN_ITEMCHANGED && (nm->uChanged & LVIF_STATE) &&
            (nm->uNewState & LVIS_SELECTED) && nm->iItem >= 0 && nm->iItem < (int)g_cfg.rules.size()) {
            SetWindowTextW(g_name, g_cfg.rules[nm->iItem].folder.c_str());
            SetWindowTextW(g_exts, g_cfg.rules[nm->iItem].exts.c_str());
        }
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
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = WND_CLASS;
    RegisterClassW(&wc);

    // Fixed-size window with a ~548x478 client area
    RECT r{ 0, 0, 548, 478 };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    CreateWindowExW(0, WND_CLASS, APP_NAME, style, x, y, w, h, nullptr, nullptr, hInst, nullptr);
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
