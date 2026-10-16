# Vendored editor components

Downloaded on 2026-10-16 from the official Scintilla project over certificate-verified HTTPS:

- Scintilla 5.6.7 source: https://www.scintilla.org/scintilla567.zip
- Lexilla 5.5.4 source: https://www.scintilla.org/lexilla554.zip
- Windows x64 runtime DLLs from SciTE 5.6.7: https://www.scintilla.org/wscite567.zip

`SHA256SUMS.txt` records the received archives and extracted DLL hashes. It is a local reproducibility record, not an upstream signature verification.

The app uses Scintilla.dll and Lexilla.dll and the corresponding API headers. It does not use the SciTE application or any Notepad++ source, assets, name, or icon. Full upstream distributions are retained here for source/license reference; the portable release ships only the two runtime DLLs and their license notices.

Scintilla and Lexilla have permissive licenses allowing commercial redistribution with notices. Their complete upstream License.txt files are copied next to NoteUltra.exe on every build. Other source files in the upstream distributions retain their original license headers.
