# NoteUltra — notes for Claude

NoteUltra is a native Windows text/code editor in the spirit of Notepad++. It is written in Win32 C++17 with Scintilla 5.6.7 and Lexilla 5.5.4, which are loaded as DLLs. The UI targets the look and feel of Windows 11 (Fluent), with full light and dark themes. README.md is the user-facing documentation; this file covers what you need in order to change the code safely.

## Build and test

Build and run the release smoke test (the default):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

Also run the I/O tests and the in-process UI self-tests:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -ExtendedTests
```

`-SkipTests` builds only.

- **Compiler:** TDM-GCC 9.2 at `F:\Software\TDM-GCC-64\bin\g++.exe`.
  - The language level is C++17, so C++20 features are not available.
  - The build uses `-Wall -Wextra`, and it should stay warning-free.
- **Output:** `dist\NoteUltra.exe`, plus `Scintilla.dll` and `Lexilla.dll` copied from `vendor\wscite\wscite`.
- **Link libraries:**
  - Both g++ lines in `build.ps1` (release and `-DNOTEULTRA_TESTING`) need the same library list.
  - `CMakeLists.txt` has its own copy of the list.
  - The current list is comctl32, comdlg32, ole32, uuid, shell32, gdi32, uxtheme, gdiplus and dwmapi.
- **Locked exe:** if the build fails with a copy/IOException error, a NoteUltra.exe is still running. Close it first.
- **Python:** `python` on PATH is Python 2; use `py -3`.
- **Line endings:** source files use LF line endings. Keep them that way.

### Tests and the contracts they depend on

- **`smoke-tests.cpp`** is an external process that drives the release exe. It relies on:
  - window class `NoteUltra.Main`
  - visible `Scintilla` child windows
  - command IDs 1001, 1003, 1006, 1201 and 1305
  - `GetDlgItem(app,5001)` (the tab strip) answering `TCM_GETITEMCOUNT`
  - 5101 being the Find field
  - 5103 (Next) and 5110 (HideFind)
- **`self_test.hpp`** is compiled into `main.cpp` with `NOTEULTRA_TESTING` and run with `--self-test <dir>`. It calls internal functions directly:
  - `command`, `control`, `selectDocument`, `newDocument`, `removeDocument`, `findNext`, `beginReplaceAll`, `saveTo`, `openPath` and `tick`
  - the globals `split`, `secondary`, `dark` and `replacement`
  - the toggle buttons through `BM_SETCHECK`/`BM_GETCHECK`
- **`io-tests.cpp`** covers `file_io.hpp`.
- **Before renaming:** keep these IDs, names and behaviours stable, or update the tests in the same change.

### Visual check

Run `tools\Screenshot.ps1 -File <file> -Out <png> -Theme 1|2 [-Commands 1202,1305]`, then look at the PNG.

- It uses `PrintWindow`, so it works on a locked or remote desktop.
- `SendKeys` and screen capture do not work there; drive the UI with `-Commands` (WM_COMMAND IDs) instead.
- A capture taken right after launch can come out blank or stale. If an area looks empty, recapture after sending a command before you assume there's a bug.
- The script backs up the user's `%LOCALAPPDATA%\NoteUltra\settings.ini` and restores it afterwards.

## Code layout

The code uses a dense style with many statements per line and few comments. Match it.

- **`main.cpp`** — the whole application, in roughly this order:
  1. The `Command` enum: menu, command and control IDs.
  2. `Document`, `Job` and `ReplaceTask`.
  3. The background IO worker thread (`startIO`, `submit`), with jobs completed on the UI thread in `completeJobs()`.
  4. Open and save (async, atomic save), find/replace (Replace All runs in time slices from `tick()`), and bookmarks.
  5. **Window chrome:** all of it is custom-painted child windows, not common controls:
     - `NoteUltra.Tabs` (id 5001; `paintTabs`, `layoutTabs`, `tabsProc`)
     - `NoteUltra.Toolbar` (id 5003; the `tools[]` table)
     - `NoteUltra.Status` (id 5002; clickable `segments`)
     - the find bar: borderless EDIT controls inside boxes painted by `paintMain`, plus `BS_OWNERDRAW` buttons subclassed by `buttonProc` for hover and check state
  6. `refresh()` runs from the 40 ms timer when `refreshNeeded` is set. It uses string "signatures" to repaint the chrome only when something changed.
  7. `layout()`, `applyAppearance()` (theme, fonts, brushes, editor colours), `command()`, the menu, settings, `windowProc`, `shortcut()` and `wWinMain`.
- **`theme.hpp`** — `Palette`/`makePalette` (Windows 11 colours; the accent comes from the registry), plus:
  - dark-mode plumbing:
    - uxtheme ordinals 133, 135 and 136
    - `DarkMode_Explorer` for scrollbars
    - DWM attributes for the dark title bar
  - the dark menu bar, drawn via undocumented `WM_UAHDRAWMENU`/`WM_UAHDRAWMENUITEM` plus a cover for the 1px line under it
  - drawing helpers: `Buffer` (double buffering), `Canvas` (GDI+ anti-aliased rounded shapes), and `drawText`/`drawGlyph` (GDI ClearType)
- **`languages.hpp`** — the language table and keywords, plus:
  - `colorLanguage()`, with VS Code Dark+/Light+ syntax colours
  - `paintEditor()`, which sets Scintilla element colours, DPI-scaled margins and markers
  - `fitLineNumbers()`
- **`file_io.hpp`** — encoding detection and conversion, streaming load, atomic save, and external-change stamps.
- **`platform.hpp`** — basic helpers: `sci()`, `widen`, `utf8`, paths.
- **Other folders:**
  - `packaging\` — icon generation (`Make-Assets.ps1`, called by the build) and MSIX packaging (see STORE.md)
  - `vendor\` — Scintilla, Lexilla and the prebuilt DLLs

## Conventions and gotchas

- **DPI:** everything is in DIPs through `px(n)`. The app is per-monitor v2 DPI aware (app.manifest), and `WM_DPICHANGED` recreates fonts and calls `applyAppearance()`.
- **Colours:** never hard-code colours in chrome code. Use `pal.*` and the `pal.hover()`/`pal.pressed()` helpers, so both themes work.
- **Fonts:**
  - UI text uses Segoe UI Variable Text, falling back to Segoe UI.
  - Icons use Segoe Fluent Icons, falling back to Segoe MDL2 Assets; glyphs are referenced by code point, e.g. `0xE8BB` for close.
  - The editor font is Cascadia Mono, falling back to Consolas.
- **New command:**
  1. Add an ID to the `Command` enum without renumbering existing ones, because the tests use the numbers.
  2. Handle it in `command()`.
  3. Add the menu item in `menu()`.
  4. Optionally add a toolbar entry in `tools[]` (`toolEnabled`/`toolChecked` control its state), and a shortcut in `shortcut()`.
- **New editor setting:**
  1. Apply it in `paintEditor()` or `configure()`. Both run again for every editor in `applyAppearance()`.
  2. Persist it in `loadSettings()`/`saveSettings()`.
- **Settings:** stored in `%LOCALAPPDATA%\NoteUltra\settings.ini` under `[View]`, with the keys Theme (0 system, 1 light, 2 dark), Wrap, Toolbar, StatusBar and FontSize. The legacy `Dark=` key is still read when `Theme` is missing.
- **Chrome state rule:** state computed during layout, such as hit rectangles, must not be rebuilt in `refresh()` unless that same call lays it out again. A status bar bug came from exactly this; see the `segments` handling.
- **Keyboard routing:** the message loop skips `IsDialogMessage` while an editor has focus, so Scintilla receives Tab and Enter. Global shortcuts live in `shortcut()`.
- **Dialogs:** they stay standard Windows dialogs, and the save prompt is a TaskDialog loaded dynamically. On Windows builds before 17763 (1809), popup menus stay light.

## Current state

- **Windows 11 redesign:** it covers every part of the UI listed under Code layout, plus dark mode end to end (title bar, menus, scrollbars, tooltips). The toolbar sun/moon button switches themes, and so does View > Theme. All three test suites pass.
- **Not built yet** (the README "Current scope" section lists them): find in files, macros, session restore and autosave, printing, plugins, and tab drag-to-reorder.
- **Not verified:** CMake builds; per-monitor DPI changes; and hover, click and menu behaviour on a live desktop, since the redesign was only checked through screenshots.
