# NoteUltra

A native Windows x64 text and code editor built in C++17 with Win32 controls, Scintilla 5.6.7, and Lexilla 5.5.4. This is an independent Notepad++-style editor with its own interface and branding.

## Run and copy to F:

Open `dist/NoteUltra.exe` or `Run.cmd`. Keep **NoteUltra.exe, Scintilla.dll, Lexilla.dll, and both license notices together**. No web runtime, .NET runtime, account, background service, or network connection is required by the app.

The session could write only in the temporary directory. To copy the whole project to **F:\NoteUltra**, double-click **Install.cmd** from your normal Windows account. The script refuses to overwrite a nonempty destination. To choose another location:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-ToDrive.ps1 -Destination 'F:\New folder\NoteUltra'
```

The portable ZIP can also be extracted directly into any folder. Copy the files out of Temp before Windows cleans temporary files.

## Editing features

- Multiple document tabs, multi-file Open, command-line file paths, and drag-and-drop opening.
- Syntax highlighting for C/C++, JavaScript/TypeScript, Python, JSON, HTML, XML, CSS, SQL, shell, Markdown, YAML, Rust, Java/C#, PowerShell, and INI/properties. Language selection is automatic by extension or manual from the menu. Some grouped languages use a shared lexer and are not full language-service implementations.
- Line numbers, code folding, bookmarks, indentation guides, brace matching, multi-caret editing, and keyword completion.
- Split view editing the same document in two panes.
- Document Map (View > Document map): a zoomed-out view of the whole document beside the editor, with the visible lines highlighted. Click or drag on it to scroll; the mouse wheel scrolls the editor.
- Document List (View > Document list): a side panel listing every open document with its folder and unsaved-change dot. Click to switch, middle-click or the hover close button to close, right-click for the tab menu.
- Undo/redo, duplicate line/selection, delete line, indent/unindent, uppercase/lowercase selection.
- Find next/previous with wrap, case and whole-word options, Scintilla regular expressions, Replace, and cooperative Replace All grouped into one undo action.
- Go to line, word wrap, whitespace display, and zoom.
- Windows 11 look and feel: Fluent icon command bar, rounded tabs with hover close buttons and unsaved-change dots, an inline find/replace bar, and a clickable status bar for line/column, language, line endings, and encoding.
- Light and dark themes for the whole window, including the title bar, menu bar, tabs, scrollbars, and editor. The theme follows Windows by default (View > Theme) and picks up the system accent colour. The toolbar sun/moon button switches between light and dark.
- Per-monitor DPI awareness, so the UI stays sharp on high-DPI and mixed-DPI displays.
- UTF-8, UTF-8 BOM, UTF-16 LE/BE, and Windows ANSI; CRLF, LF, and CR line endings. Encoding selection changes the next save format. Line-ending conversion is undoable.
- Background loading and saving, save progress, modified-tab indicators, Save All, and save/discard/cancel prompts when closing unsaved documents.

## Keyboard shortcuts

| Action | Shortcut |
|---|---|
| New / Open / Save | Ctrl+N / Ctrl+O / Ctrl+S |
| Save As / Close tab | Ctrl+Shift+S / Ctrl+W |
| Next / previous tab | Ctrl+Tab / Ctrl+Shift+Tab |
| Find / Replace / Go to line | Ctrl+F / Ctrl+H / Ctrl+G |
| Next / previous match | F3 / Shift+F3 |
| Toggle / next bookmark | Ctrl+F2 / F2 |
| Keyword completion | Ctrl+Space |
| Duplicate line or selection | Ctrl+D |
| Uppercase / lowercase selection | Ctrl+Shift+U / Ctrl+U |
| Split view / Word wrap | Ctrl+Alt+S / Alt+Z |
| Zoom in / out / reset | Ctrl++ / Ctrl+- / Ctrl+0 |
| Stop Replace All or opening / hide find panel | Escape |

Scintilla also supplies its native editing, rectangular-selection, and multi-caret shortcuts. The split-view panes share text and undo history, with independent scroll positions.

## Files and responsiveness

The UI uses native controls. A persistent background I/O worker reads files in 256 KiB chunks, converts encoding, and builds a Scintilla document through its background loader interface. Switching tabs does not reload or copy their text. Files above 16 MiB open with syntax highlighting disabled; this version accepts input files up to 128 MiB. Memory use depends strongly on line count and undo history, not just file size.

During saving, the document is temporarily read-only so the background writer can safely use its UTF-8 buffer. Other tabs remain available. The writer encodes into a newly created temporary file beside the destination, flushes it, and commits with ReplaceFileW or MoveFileExW. Failures retain the unsaved document. Saves reject changed destination size/timestamps; they do not silently overwrite that detected external edit. This is a metadata check rather than a content hash or guaranteed lock against all concurrent editors. Use Save As to keep both versions after a conflict. UTF-8 is the safest choice for text not representable in Windows ANSI; lossy ANSI saves are rejected.

Replace All yields between batches and supports Escape. Literal find and individual regex searches run synchronously in Scintilla. Regex is limited to documents up to 16 MiB, uses **Scintilla's basic POSIX-style syntax**, and does not promise PCRE/Notepad++ compatibility. See [Scintilla search documentation](https://www.scintilla.org/ScintillaDoc.html#searchFlags). For replacement groups use `\1`, `\2`, etc. Replacement fields contain literal text, not a general escape-sequence mode.

UTF-8 without a BOM is validated before loading. If invalid and the system ANSI codepage can decode it, ANSI is used. BOM-less UTF-16 and files containing NUL bytes without a UTF-16 BOM are rejected as likely binary. Invalid BOM-marked Unicode is rejected. The first encountered newline determines the default for newly typed lines; mixed existing endings are preserved until explicitly converted.

## Current scope

This is a functional first release of the core editor, not full Notepad++ feature parity. It does not include a plugin manager, macros, folder workspace tree, find-in-files, session restoration, crash recovery/autosave, compare tools, printing, LSP, or a terminal. Unsaved documents require a normal save before closing. Back up important documents independently, as with any new editor.

The menu bar is dark-themed through undocumented Windows menu-drawing messages and the `uxtheme` preferred-app-mode API, as used by Windows Terminal and Notepad++. On Windows 10 builds before 1809, popup menus and common dialogs keep the light system style. The open/save dialogs and message boxes are always standard Windows dialogs. Theme, toolbar, status bar, Document Map, Document List, wrap, and font-size preferences are stored in `%LOCALAPPDATA%\NoteUltra\settings.ini` when available. There are no built-in analytics, telemetry, update requests, or cloud services.

## Build and checks

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

The default uses the installed `F:\Software\TDM-GCC-64\bin\g++.exe`; pass `-Compiler` for another MinGW compiler. It regenerates the icon/logos, builds the release executable, and runs a separate end-to-end release workflow test. `-SkipTests` builds only. `-ExtendedTests` additionally compiles/runs the encoding and bulk UI suites. Test-only commands and test code are excluded from the release binary.

Core I/O tests passed. The release workflow passed against the actual release app, including opening, find, dirty-state tracking, undo/redo, save-byte verification, split view, tabs, UI responsiveness, and exit. The extensive in-process UI suite could not complete reliably in this environment: test processes exited with code 5 and test executables disappeared from disk. The source of that removal could not be confirmed because security logs were inaccessible. No security settings were changed. See [VALIDATION.md](VALIDATION.md) for the exact coverage and limits.

CMake files are supplied for MinGW or Visual Studio, but that route has not been tested here. Generate assets with `packaging/Make-Assets.ps1` before configuring a CMake build. The bundled upstream DLLs avoid rebuilding third-party dependencies; their source distributions and hashes are in `vendor`.

## Microsoft Store

MSIX manifest, assets, and packaging script are included. See [STORE.md](STORE.md). The app is unsigned and has not passed Store certification. Partner Center identity values and Windows SDK packaging tools are still required.
