# Download Folder Butler

Download Folder Butler is a lightweight Windows system-tray application written in C++ that automatically organises files in your Downloads folder into subfolders based on their file extensions.

It runs quietly in the background, supports configurable sorting rules, and can launch automatically when Windows starts.

## Features

- **Automatic sorting** — checks the selected folder every five seconds and sorts eligible files in the background.
- **Manual sorting** — sort files immediately using the application window or system-tray menu.
- **Custom sorting rules** — add, edit, and remove rules to control which file types go into which folders.
- **Processors** — hand matching files to any external program (for example a converter or optimiser) before they are sorted. See [Processors](#processors).
- **Duplicate filename protection** — avoids overwriting files by adding numbered suffixes, such as `report (1).pdf`.
- **Partial-download protection** — skips common temporary download extensions, including `.crdownload`, `.part`, and `.tmp`.
- **File-write protection** — automatic sorting skips files modified within the last three seconds.
- **System-tray integration** — close or minimise the window to keep the application running in the background.
- **Start with Windows** — optionally launch automatically when you sign in.
- **Persistent configuration** — saves your selected folder, sorting rules, and automatic-sorting preference.
- **No external runtime dependencies required** — uses the Windows API and C++ standard library.

## Default Sorting Rules

The application includes the following rules by default:

| Destination folder | File extensions |
|---|---|
| `Images` | jpg, jpeg, png, gif, bmp, webp, svg, heic, tiff, ico |
| `Documents` | pdf, doc, docx, txt, rtf, odt, md |
| `Spreadsheets` | xls, xlsx, csv, ods |
| `Presentations` | ppt, pptx, odp |
| `Videos` | mp4, mkv, avi, mov, wmv, webm, flv |
| `Music` | mp3, wav, flac, aac, ogg, m4a, wma |
| `Archives` | zip, rar, 7z, tar, gz, bz2, xz |
| `Installers` | exe, msi, msix, dmg, iso |

Files that do not match a configured rule are left in the selected folder.

## Requirements

- Windows 10 or Windows 11
- A C++17-compatible compiler
- Windows SDK headers and libraries when compiling with MSVC

Supported compiler options:

- Microsoft Visual C++ (MSVC)
- MinGW-w64 with GCC

## Building

### Option 1: MSVC

Open an **x64 Native Tools Command Prompt for Visual Studio** or another suitable Visual Studio Developer Command Prompt.

Navigate to the repository directory and run:

```bat
rc /nologo DownloadButler.rc
cl /EHsc /std:c++17 /O2 DownloadButler.cpp DownloadButler.res /link /SUBSYSTEM:WINDOWS
```

The first command compiles the application icon (`assets/DownloadButler.ico`) into a resource file that is linked into the executable.

The resulting executable is `DownloadButler.exe`.

### Option 2: MinGW-w64

Open a terminal where `g++` is available and run:

```bash
windres DownloadButler.rc -O coff -o DownloadButler.res.o
g++ -std=c++17 -O2 -municode -mwindows DownloadButler.cpp DownloadButler.res.o -o DownloadButler.exe -lcomctl32 -lcomdlg32 -lshell32 -lole32 -ladvapi32 -luuid -static
```

The resulting executable is `DownloadButler.exe`.

## Getting Started

1. Compile the source code using one of the methods above.
2. Run `DownloadButler.exe`.
3. The application starts hidden in the system tray.
4. Find the Download Folder Butler icon in the Windows notification area. If it is hidden, open the tray's overflow menu.
5. Click the icon to open the configuration window.
6. Confirm the folder you want to organise.
7. Review the default sorting rules and customise them if necessary.
8. Enable **Auto-sort new downloads in the background** if it is not already enabled.
9. Use **Sort now** to organise eligible files immediately.

The application continues running in the system tray when you close or minimise its window. To stop it, right-click the tray icon and select **Exit**.

## Configuration

The application stores its configuration in:

```text
%APPDATA%\DownloadButler\config.ini
```

This normally resolves to a path similar to:

```text
C:\Users\<YourUserName>\AppData\Roaming\DownloadButler\config.ini
```

The configuration file contains the selected folder, automatic-sorting preference, custom rules, and processors.

Example:

```ini
[General]
DownloadFolder=C:\Users\Example\Downloads
AutoSort=1
ProcessorTimeoutSec=120

[Rules]
Count=2
Folder0=Images
Exts0=jpg,jpeg,png,gif
Folder1=Documents
Exts1=pdf,doc,docx,txt

[Processors]
Count=1
Exts0=webp
Exe0=|C:\Program Files\ImageMagick\magick.exe|
Args0=|"{file}" "{dir}\{name}.png"|
Enabled0=1
```

The `|` characters around a processor's program and arguments are written by the application (Windows would otherwise strip quotes from the value); keep them if you edit those lines by hand.

The application manages this file automatically. It is generally best to edit rules through the application window rather than modifying the configuration manually while the application is running.

## Creating Custom Rules

To add or change a rule:

1. Enter a destination folder name or full path.
2. Enter the file extensions to match.
3. Click **Add / Update rule**.

Extensions can be separated by commas, semicolons, spaces, or tabs. Leading dots and asterisks are removed automatically.

For example:

```text
Folder: Images
Extensions: .jpg; .jpeg; .png; *.webp
```

The normalised extension list becomes:

```text
jpg,jpeg,png,webp
```

Rules are processed in order, and **the first matching rule wins**. If two rules contain the same extension, the earlier rule takes precedence.

A relative destination such as `Images` creates a subfolder inside the selected directory. An absolute destination path can be used to send files elsewhere.

## Processors

A **processor** hands a file to an external program. For example: whenever a `.webp` appears in your Downloads folder, run a converter on it.

Open the **Processors** page in the main window and fill in:

| Field | Meaning |
| ----- | ------- |
| **Extensions** | Which files the processor applies to (`webp`, or `png,jpg`). Same format as sorting rules. |
| **Program to run** | The `.exe` to launch: a full path, or just a name if it is on your `PATH` (such as `magick.exe`). |
| **Arguments** | What to pass to the program. Leave blank to pass just the file path. |

Arguments can use these placeholders:

| Placeholder | Replaced with | Example |
| ----------- | ------------- | ------- |
| `{file}` | Full path of the file | `C:\Users\You\Downloads\photo.webp` |
| `{dir}` | Folder containing the file | `C:\Users\You\Downloads` |
| `{name}` | File name without extension | `photo` |
| `{ext}` | Extension without the dot | `webp` |

Put quotes around placeholders that might contain spaces. For example, to convert WebP images to PNG with ImageMagick:

```text
Extensions: webp
Program:    C:\Program Files\ImageMagick\magick.exe
Arguments:  "{file}" "{dir}\{name}.png"
```

The resulting `photo.png` appears in the Downloads folder and is then sorted by your normal rules, like any other download. Use the checkbox beside a processor to switch it off without deleting it.

### How processors run

- **Before sorting.** The program receives the file where it was downloaded; afterwards the file is sorted as usual.
- **All matches run.** Unlike sorting rules (first match wins), every enabled processor that matches a file runs, one after another, in list order.
- **In the background.** Programs run on a separate thread, so a slow converter never freezes the tray app. Console windows are not shown.
- **Once per file.** A file that stays in the folder (because no sorting rule matches it) is not reprocessed every five seconds; it is processed again only if it changes. Restarting Butler forgets this, so such files are processed once more after a restart.
- **Success means exit code 0.** If a program exits with any other code, can't be started, or runs longer than the timeout (120 seconds), the file is **left where it is** (not sorted), a tray notification explains why, and Butler does not retry until the file changes.
- **Programs that delete or rename the file** are fine: Butler notices the file is gone and moves on.
- **Scripts.** Only `.exe` programs are accepted directly, because batch files interpret special characters in file names. To run a script, point the processor at its interpreter, e.g. Program `powershell.exe`, Arguments `-NoProfile -File "C:\Scripts\handle.ps1" "{file}"`.

The timeout can be changed with `ProcessorTimeoutSec` in the config file.

## Automatic Sorting Behaviour

When automatic sorting is enabled, the application checks the selected folder every five seconds.

During each check, it:

- Examines files directly inside the selected folder.
- Ignores subdirectories and does not recursively sort their contents.
- Skips hidden and system files when checking whether a file is ready for sorting.
- Skips files that have been modified within the last three seconds.
- Skips recognised partial-download extensions.
- Moves files whose extensions match a configured rule.
- Leaves unmatched files in place.

If a destination filename already exists, the application tries a numbered filename to prevent overwriting the existing file.

When a file cannot be moved directly, the application attempts to copy it to the destination and then remove the original.

## Start with Windows

Enable **Start with Windows** in the main window to register the application under the current user's Windows startup settings.

The application stores this setting in:

```text
HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run
```

Disabling the option removes the corresponding startup entry. This setting applies to the current Windows user and does not require administrator privileges under normal circumstances.

## Troubleshooting

**The application does not open a visible window**

The application starts hidden in the system tray. Look for its icon near the clock, including in the hidden-icons menu.

**Files are not being sorted**

Check that the selected folder exists, automatic sorting is enabled, and the file extensions match a configured rule. Automatic sorting also skips files modified within the last three seconds.

**Some files remain in Downloads**

Files without a matching rule, files with unsupported extensions, and certain temporary downloads are intentionally left in place.

**A file isn't being sorted and a notification said a processor failed**

A processor exited with an error, couldn't be started, or timed out, so the file was deliberately left in place. Check the program path and arguments on the Processors page. Butler tries again when the file changes, or after you restart it.

**A file has a numbered suffix**

A file with the same name already exists in the destination folder. Butler uses a numbered filename to avoid overwriting it.

**The configuration does not appear to save**

Check that the application can write to your roaming application-data directory and that the configuration file is not being modified by another process.

## Safety Notes and Limitations

- The application moves files; it does not categorise them by their actual contents.
- File extensions determine the destination folder, so files with misleading extensions may be sorted incorrectly.
- The three-second settling check reduces the chance of moving an active download but cannot guarantee that every download has finished.
- Moving or copying files can fail if permissions, file locks, disk space, or filesystem boundaries prevent the operation.
- Processors run arbitrary programs on files you downloaded, which may be untrusted. Only point them at tools you trust, and remember that a processor can modify or delete the files it is given.
- The application does not currently provide an undo function or a log of individual file movements. Consider backing up important files before using it on a large folder.
- Custom absolute destination paths should be checked carefully before sorting.
- Files in subdirectories are not processed automatically.

## License

Creative Commons Zero v1.0 Universal 

## Project Structure

```text
DownloadFolderButler/
├── DownloadButler.cpp
├── DownloadButler.rc        # Embeds the app icon
├── resource.h
├── assets/
│   └── DownloadButler.ico
├── DownloadButler.exe       # Generated when compiled
├── README.md
└── LICENSE                  # Optional; add before distribution
```

---

**Download Folder Butler** — keep your Downloads folder organised automatically, with simple rules and minimal interruption.
