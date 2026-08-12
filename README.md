# PuTTY 0.84k*

This is an unofficial PuTTY build based on the standard PuTTY `0.84` release.

## Changes

### Close+Restart and quick restart

The Windows version adds session restart controls to the window menu.

- During an active session, a `Close+Restart` item appears in both the system menu and the window context menu.
- `Close+Restart` closes the current backend, switches the window to the inactive state, and immediately starts the backend again with the same `Conf`.
- After the session is closed, the menu item changes to `Restart Session`.
- If the session is already closed, pressing `Enter` without `Alt`, `Ctrl`, or `Shift` triggers `Restart Session`.

In standard PuTTY 0.84, a closed session cannot be restarted quickly from the same window: normally, you need to open a new window or select the saved session again.

### Start WinSCP

For SSH sessions, the Windows system and context menus contain a `Start WinSCP` item. It opens an SFTP connection using the current host, port, username, and private key. `WinSCP.exe` is discovered next to `PuTTY.exe` or through the standard Windows application registration. If it cannot be found, PuTTY opens a file picker. The executable can also be selected in `Window -> Behaviour -> WinSCP integration`; its path is stored globally as the `WinSCPPath` key in `putty.ini`.

### KiTTY-style portable file storage

An alternative Windows backend for storing settings in files has been added. The standard registry backend remains available and is used by default.

File storage can be enabled in any of the following ways:

- create a `putty.ini` file next to `putty.exe`;
- start the process with `PUTTY_STORAGE=file`;
- start the process with `PUTTY_STORAGE=ini`;
- start the process with `PUTTY_STORAGE=files`.

File storage can be forcibly disabled with:

```text
PUTTY_STORAGE=registry
```

File storage root:

- if `putty.ini` is found next to the executable, the executable directory becomes the root;
- if file storage is enabled through an environment variable without `putty.ini`, `%APPDATA%\PuTTY` becomes the root;
- if `%APPDATA%` is unavailable, the executable directory is used as a fallback.

File structure:

```text
putty.ini
sessions\<escaped>.ini
sshhostkeys.ini
hostcas\<escaped>.ini
randomseed
```

Stored in files:

- saved sessions;
- SSH host keys;
- SSH host CAs;
- random seed;
- session settings, including fonts and filenames.

Session file format:

- the primary format is compatible with KiTTY portable files: `Item\Value\`;
- values are percent-encoded;
- the legacy `key=value` format is also supported for smooth migration.

Session names containing `/` are converted into subfolders under `sessions`. For example:

```text
Network/Routers/Core01
```

is stored as a session file inside `sessions\Network\Routers\`.

In file storage mode, the `-load` option can open:

- a regular saved session name;
- an `.ini` filename;
- a relative or absolute path to an `.ini` file.

Examples:

```text
putty.exe -load "Network/Routers/Core01"
putty.exe -load "Core01.ini"
putty.exe -load "D:\PuTTY\sessions\Core01.ini"
```

### Saved Sessions hierarchy

A folder-like mode has been added:

- folders are shown before sessions;
- inside a subfolder, only the session name is displayed;
- load/save/delete operations use the full session name;
- entering the name of an existing session switches the list to the corresponding folder.

This is especially useful together with file storage, where `/` in a session name is reflected in the actual directory structure.

### putty-launcher.exe

A separate singleton tray launcher application, `putty-launcher.exe`, has been added to the Windows build.

Left-clicking the tray icon opens the main launcher window near the cursor, with:

- a search field;
- a session list;
- a `Load` button;
- a `New` button.

Main window behavior:

- without search text, explorer mode is used with saved-session folders;
- when text is entered, a flat search across all sessions is used;
- `Enter` and double-click launch the selected session;
- `Load` starts `putty.exe -load "<session>" -edit`;
- `New` starts `putty.exe` without a session;
- `Backspace` in the list moves up one folder;
- `Up` / `Down` switch focus between the search field and the list;
- `Esc` hides the launcher window.

The launcher builds the session list from two sources:

- Windows Registry: `HKCU\Software\SimonTatham\PuTTY\Sessions`;
- file storage: the `sessions` directory.

If the same session exists in both the registry and file storage, the launcher takes both sources into account when building the list.

### GitHub Actions build/release workflow

A `.github/workflows/build.yml` workflow has been added.

It runs on tag pushes, builds Windows x64 using `dockcross/windows-static-x64`, takes executables from `build/shipped.txt`, and creates a GitHub Release through `softprops/action-gh-release`.

### Ctrl/Shift/Alt arrow key fix

The behavior of `ShiftedArrowKeys` has been changed.

In standard PuTTY 0.84, the default mode was `Ctrl toggles app mode`. In this branch, the default has been changed to `xterm-style bitmap`.

Ctrl/Shift/Alt combined with arrow keys now generate xterm-style CSI sequences:

```text
ESC [ 1 ; <mod> A
ESC [ 1 ; <mod> B
ESC [ 1 ; <mod> C
ESC [ 1 ; <mod> D
```

Here, `A/B/C/D` correspond to Up/Down/Right/Left, and `<mod>` encodes the modifier combination.

This fixes a case where Ctrl/Shift modifiers were lost in application cursor mode. The practical goal is compatibility with Midnight Commander and similar terminal applications that expect xterm-style modified arrow keys.

### Window title templates

KiTTY-style placeholders have been added to `Window/Behaviour -> Window title`. The syntax uses a double `%`.

| Placeholder | Meaning |
| --- | --- |
| `%%f` | saved-session folder name, meaning the part before the final `/` or `\` |
| `%%h` | host name |
| `%%p` | port number |
| `%%P` | protocol name |
| `%%s` | saved session name |
| `%%u` | username |
| `%%l` | list of local forwarded ports |
| `%%d` | list of dynamic forwarded ports |
| `%%` | literal `%` |

Example:

```text
%%s [%%u@%%h:%%p]
```

For the session `Network/Core01`, user `admin`, host `core01.example.net`, and port `22`, this produces a title like:

```text
Network/Core01 [admin@core01.example.net:22]
```

If the custom title is empty, PuTTY's standard logic is used: hostname + application name.

### Saving window position and size

The following setting has been added:

```text
SaveWindowPos
```

In the UI, it is shown as:

```text
Window/Behaviour -> Save position and size on exit
```

Default: enabled.

When the PuTTY window closes, it saves the following values to the loaded session:

- `TermWidth`;
- `TermHeight`;
- `TermXPos`;
- `TermYPos`.

The next time the session is started, the window opens at the saved position if `TermXPos` and `TermYPos` are not `-1`.
If no session was loaded, or if it is `Default Settings`, the values are saved to the default settings.

### Extended mouse selection

You can quickly extend the selection to the row under the current mouse cursor by pressing `Enter`.

### Ctrl+MouseWheel changes font size

The Windows terminal window now supports:

```text
Ctrl + MouseWheel Up -> increase font size by 1
Ctrl + MouseWheel Down -> decrease font size by 1
```

### Minimize to Tray via putty-launcher

The following item has been added to the system and context menus:

```text
Minimize to Tray
```

Right-clicking the launcher tray icon opens a window called:

```text
Minimized PuTTY Sessions
```

It allows you to:

- search by session name;
- view a list of minimized sessions;
- minimize all open session windows (`Minimize all to tray`);
- restore all session windows (`Restore all`);
- close all sessions (`Close all...`).

Operations for minimized sessions:

- left-click / double-click / `Enter` restores the selected session;
- right-click opens a context menu;
- the context menu provides `Restore`, `Close session`, `Copy title`, and `Copy session name`.

### Direct connect from launcher search

The launcher search shows a direct-connect entry when the entered text looks like a host and does not match an existing saved session.

Supported formats:

- IPv4;
- IPv6;
- FQDN;
- `user@host`.

Examples:

```text
192.0.2.10
2001:db8::10
server.example.net
admin@server.example.net
```

For `host`, the launcher starts:

```text
putty.exe "<host>"
```

For `user@host`, the launcher starts:

```text
putty.exe -l "<user>" "<host>"
```

### Open URLs with Ctrl+Shift+Left Click

- if a URL is found, it is opened with the system URL handler;
- if no URL is found, the action is silently ignored.

The Windows frontend opens URLs through:

```text
ShellExecuteA(..., "open", url, ...)
```

The GTK/Unix frontend opens URLs through:

- `gtk_show_uri_on_window`, if GTK 3.22+ is available;
- `gtk_show_uri`, if GTK 2.14+ is available;
- fallback to `xdg-open`;
- fallback to `open` for macOS GTK.

URLs are recognized by the following prefixes:

```text
https://
http://
www.
```

For `www.`, `https://` is added automatically.
Example:

```text
www.example.com/path
```

is opened as:

```text
https://www.example.com/path
```

```text
visit https://example.com/path), now
```

opens:

```text
https://example.com/path
```

## Comparison with standard PuTTY 0.84

| Area | Standard PuTTY 0.84 | 0.84k-r2 |
| --- | --- | --- |
| Saved sessions | Windows Registry, flat list | Registry by default, optional file storage, folder-like UI |
| Portable mode | No built-in KiTTY-style file storage | `putty.ini` next to the executable or `PUTTY_STORAGE=file` |
| Host keys / host CAs | Registry | Registry or files, depending on the storage backend |
| Random seed | Standard Windows paths | Upstream behavior in registry mode; `randomseed` in file mode |
| Launcher | No separate tray launcher | `putty-launcher.exe` with session search |
| Quick connect | Main PuTTY dialog / command line | Launcher search supports direct host connection |
| Restart session | No `Close+Restart` / Enter restart | `Close+Restart`, `Restart Session`, Enter after close |
| Minimize to tray | No | Through `putty-launcher`, with a list of minimized sessions |
| Ctrl+MouseWheel font size | No | Available in the Windows terminal window |
| URL click | No built-in Ctrl+Shift URL opening | `Ctrl+Shift+Left Click`, hover underline, hand cursor |
| Modified arrow keys | Default `Ctrl toggles app mode` | Default `xterm-style bitmap`, better for MC |
| Window title | Static title or remote title | KiTTY-style placeholders in the initial title |
| Window position | No session-level `SaveWindowPos` | Saves position and terminal size on exit |
| Mouse selection + Enter | No | Enter extends the active drag selection downward |
| Build automation | No such repository workflow | Tag-based Windows x64 release workflow |

## Limitations and important notes

- File storage is implemented for the Windows storage backend.
- In normal mode, PuTTY continues to use the Registry until `putty.ini` is found or `PUTTY_STORAGE=file|ini|files` is set.
- File storage does not automatically import registry sessions. The launcher can see both sources, but the main PuTTY storage backend is selected for the entire process.
- `putty-launcher.exe` starts `putty.exe` from its own directory.
- `Minimize to Tray` requires `putty-launcher.exe` to be running; otherwise, PuTTY performs a normal minimize.
- URL opening has no separate enable/disable setting.
- `Ctrl+Shift+Left Click` is reserved for URL opening and is not sent as a normal mouse click or selection gesture.
- `SaveWindowPos` is enabled by default, so closing a session updates its saved settings.

## Main implementation files

| File | Purpose |
| --- | --- |
| `windows/filestore.c`, `windows/filestore.h` | File storage backend for Windows |
| `windows/storage.c` | Runtime switch between Registry and file storage |
| `config.c`, `conf.h`, `settings.c` | New settings, foldered Saved Sessions, SaveWindowPos |
| `terminal/terminal.c`, `terminal/terminal.h` | Title placeholders, Enter selection, modified arrows, URL scanner/hover/open |
| `windows/window.c`, `windows/win-gui-seat.h`, `windows/platform.h` | Windows UI: restart, minimize-to-tray, Ctrl+Wheel, URL click/hover |
| `unix/window.c` | GTK/Unix URL click/hover/open |
| `windows/putty-launcher.c`, `windows/putty-launcher.rc`, `windows/launcher_stubs.c` | Tray launcher |
| `windows/CMakeLists.txt` | `putty-launcher` build target and file storage linkage |
| `.github/workflows/build.yml` | Tag-based Windows x64 release workflow |
| `test/test_terminal.c`, `test/test_conf.c` | Regression tests for URL handling and configuration defaults |
