# Validation — 2026-10-16

## Release executable

Built Windows x64 C++17 release with installed MinGW/TDM GCC 9.2.0, O3, static application C++ runtime, official upstream Scintilla 5.6.7 and Lexilla 5.5.4 runtime DLLs. Test-only entry points are excluded from the release binary.

Passed external-process end-to-end checks against the actual release app: hidden window creation; opening a command-line file through the background loader; undo collection after loading; Find selection positions; typing, dirty state, undo/redo; asynchronous Save; exact saved-byte verification; split editor visibility; new/close tabs; UI message responsiveness; normal close and exit code 0. The release executable remained present after these checks.

Passed read-only large-file check against the release app: loaded a synthetic 22,020,096-byte (21 MiB) file in 234 ms including process startup, with UI response, undo collection enabled, and clean exit. This was a warm-cache local fixture, not a general performance guarantee. See `large-file-results.txt`; source harness supports `smoke-tests.exe <app.exe> <existing-large-fixture>`.

## Encoding and save helpers

The standalone I/O tests passed: UTF-8, UTF-8 BOM, UTF-16 LE/BE roundtrips across 256 KiB read/write boundaries; astral Unicode; CRLF detection; new and existing atomic saves; stale metadata rejection; malformed Unicode; binary rejection; cancellation; empty files; and rejection of lossy ANSI conversion. They operate only on generated temporary fixtures.

## Extensive UI suite limitation

The in-process bulk UI harness exercised syntax styling, tabs, split document pointers, grouped replace-all/undo, regex, bookmarks, EOL conversion, Unicode save formats, and background reload. It exposed a loader integration issue: undo collection must be explicitly re-enabled after ILoader attachment. That was fixed and independently verified in the final release workflow, including unsaved-state tracking and undo after loading.

The full bulk suite could not finish reliably: test processes exited with code 5, and compiled test executables disappeared from disk. Some debugger runs also ended with access violations during process termination. Endpoint security logs were inaccessible, so the source of removal/termination is unconfirmed. No security exclusions or protection changes were made. This suite is provided as optional `-ExtendedTests`; it is not claimed as fully passing.

The stable release workflow is the default build check. It does not substitute for complete certification or testing on a normal desktop session.

## Packaging checks and remaining verification

PowerShell parsing, well-formed MSIX template XML, generated icons/assets, release dependency inspection, and final archive contents are checked separately during delivery.

Not verified: visual screenshots in this restricted desktop, file-dialog interactions and save/discard/cancel prompts through automation, system clipboard and IME behavior, different Windows ANSI codepages, accessibility, monitor DPI changes, long/network paths, clean Windows 10/11 installs, CMake/MSVC builds, MSIX generation/signing/installation, or Store certification. No feature-parity claim with Notepad++ is made.

The requested profile-root and F: output locations were unavailable for writes in the session. The deliverables are in a temporary NoteUltra directory, with a normal-user copy script defaulting to F:\NoteUltra.
