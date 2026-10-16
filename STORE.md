# Microsoft Store release preparation

NoteUltra is a normal-user Win32 desktop app. The template packages the executable, both editor DLLs, licenses, and logo assets as an x64 desktop MSIX with `runFullTrust`. It requests no elevation and performs no installation or publication itself.

1. Reserve the final app name in Partner Center and obtain the exact package identity name, publisher distinguished name, and publisher display name. Availability of the NoteUltra name has not been checked.
2. Install Windows SDK MSIX packaging tools, build the release executable, and run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\packaging\Build-MSIX.ps1 `
  -IdentityName 'YOUR-PACKAGE-IDENTITY' `
  -Publisher 'CN=YOUR-PUBLISHER-ID' `
  -PublisherDisplayName 'Your publisher name'
```

3. For local sideloading, sign the package with a certificate matching Publisher and trust that certificate on the test machine. Keep private signing keys outside the project. Use the correct Store identity for submission; Microsoft handles signing for Store distribution.
4. Test clean installation, file Open/Save/Save As, encoding conversions, external-change conflict handling, unsaved-close prompts, drag-and-drop, large files, DPI settings, upgrade, and uninstall on Windows 10/11 x64. Run Windows App Certification Kit against the final package.
5. Add real screenshots, support and privacy URLs, accurate feature descriptions, age rating, pricing, and a full-trust capability explanation to Partner Center. Submit only after release checks pass.

The Windows SDK packaging tools and certification kit were not available here. The template is well-formed XML, but MSIX generation, installation, signing, and certification remain untested. Runtime dependency licenses are included. The app's branding and UI are independent and do not use Notepad++ source or assets.

Privacy statement draft: NoteUltra edits files locally. It has no account system, analytics, telemetry, or built-in network requests. It reads files explicitly opened by the user, writes files when the user saves, creates temporary files in destination folders during saves, and stores view preferences under local app data. Unsaved documents are not uploaded or automatically backed up. Publish a reviewed version at a URL you control.

[Microsoft: Manual MSIX packaging](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-manual-conversion)
