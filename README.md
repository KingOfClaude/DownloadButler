# Download Folder Butler

Download Folder Butler is a lightweight Windows system-tray application written in C++ that automatically organises files in your Downloads folder into subfolders based on their file extensions.

It runs quietly in the background, supports configurable sorting rules, and can launch automatically when Windows starts.

## Features

- **Automatic sorting** — checks the selected folder every five seconds and sorts eligible files in the background.
- **Manual sorting** — sort files immediately using the application window or system-tray menu.
- **Custom sorting rules** — add, edit, and remove rules to control which file types go into which folders.
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

Save the source code as `DownloadButler.cpp`, navigate to its directory, and run:

```bat
cl /EHsc /std:c++17 /O2 DownloadButler.cpp /link /SUBSYSTEM:WINDOWS
```

The resulting executable is `DownloadButler.exe`.

### Option 2: MinGW-w64

Open a terminal where `g++` is available and run:

```bash
g++ -std=c++17 -O2 -municode -mwindows DownloadButler.cpp -o DownloadButler.exe -lcomctl32 -lshell32 -lole32 -ladvapi32 -static
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

The configuration file contains the selected folder, automatic-sorting preference, and custom rules.

Example:

```ini
[General]
DownloadFolder=C:\Users\Example\Downloads
AutoSort=1

[Rules]
Count=2
Folder0=Images
Exts0=jpg,jpeg,png,gif
Folder1=Documents
Exts1=pdf,doc,docx,txt
```

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

**A file has a numbered suffix**

A file with the same name already exists in the destination folder. Butler uses a numbered filename to avoid overwriting it.

**The configuration does not appear to save**

Check that the application can write to your roaming application-data directory and that the configuration file is not being modified by another process.

## Safety Notes and Limitations

- The application moves files; it does not categorise them by their actual contents.
- File extensions determine the destination folder, so files with misleading extensions may be sorted incorrectly.
- The three-second settling check reduces the chance of moving an active download but cannot guarantee that every download has finished.
- Moving or copying files can fail if permissions, file locks, disk space, or filesystem boundaries prevent the operation.
- The application does not currently provide an undo function or a log of individual file movements. Consider backing up important files before using it on a large folder.
- Custom absolute destination paths should be checked carefully before sorting.
- Files in subdirectories are not processed automatically.

## License

Creative Commons Zero v1.0 Universal 

## Project Structure

```text
DownloadFolderButler/
├── DownloadButler.cpp
├── DownloadButler.exe       # Generated when compiled
├── README.md
└── LICENSE                  # Optional; add before distribution
```

---

**Download Folder Butler** — keep your Downloads folder organised automatically, with simple rules and minimal interruption.
