param([string]$Compiler = 'F:\Software\TDM-GCC-64\bin\g++.exe', [switch]$SkipTests, [switch]$ExtendedTests)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (!(Test-Path -LiteralPath $Compiler)) { $Compiler = (Get-Command g++ -ErrorAction Stop).Source }
    New-Item -ItemType Directory -Path dist -Force | Out-Null
    & (Join-Path $PSScriptRoot 'packaging\Make-Assets.ps1')
    & (Join-Path (Split-Path -Parent $Compiler) 'windres.exe') app.rc -O coff -o dist/app-res.o
    if ($LASTEXITCODE) { throw 'Resource compilation failed' }
    Copy-Item -LiteralPath .\vendor\wscite\wscite\Scintilla.dll,.\vendor\wscite\wscite\Lexilla.dll -Destination .\dist -Force
    Copy-Item -LiteralPath .\vendor\scintilla\License.txt -Destination .\dist\Scintilla-LICENSE.txt -Force
    Copy-Item -LiteralPath .\vendor\lexilla\License.txt -Destination .\dist\Lexilla-LICENSE.txt -Force
    & $Compiler '-std=c++17' -O3 -DNDEBUG -Wall -Wextra -Wno-misleading-indentation -Wno-cast-function-type -municode -mwindows -static main.cpp dist/app-res.o -o dist/NoteUltra.exe '-lcomctl32' '-lcomdlg32' '-lole32' '-luuid' '-lshell32' '-lgdi32' '-luxtheme' '-lgdiplus' '-ldwmapi'
    if ($LASTEXITCODE) { throw 'Application compilation failed' }
    if ($SkipTests) { Write-Host "Built: $PSScriptRoot\dist\NoteUltra.exe"; return }
    & $Compiler '-std=c++17' -O2 -municode -static smoke-tests.cpp -o dist/smoke-tests.exe '-lcomctl32'
    if ($LASTEXITCODE) { throw 'Release workflow test compilation failed' }
    & .\dist\smoke-tests.exe (Join-Path $PSScriptRoot 'dist\NoteUltra.exe')
    if ($LASTEXITCODE) { throw 'Release workflow tests failed' }
    if (!$ExtendedTests) { Write-Host "Built and release workflow verified: $PSScriptRoot\dist\NoteUltra.exe"; return }
    & $Compiler '-std=c++17' -O2 -Wall -Wextra -Wno-misleading-indentation -static io-tests.cpp -o dist/io-tests.exe
    if ($LASTEXITCODE) { throw 'I/O test compilation failed' }
    & .\dist\io-tests.exe
    if ($LASTEXITCODE) { throw 'I/O tests failed' }
    & $Compiler '-std=c++17' -O2 -DNOTEULTRA_TESTING -municode -mwindows -static main.cpp dist/app-res.o -o dist/NoteUltra-tests.exe '-lcomctl32' '-lcomdlg32' '-lole32' '-luuid' '-lshell32' '-lgdi32' '-luxtheme' '-lgdiplus' '-ldwmapi'
    if ($LASTEXITCODE) { throw 'UI test application compilation failed' }
    $testFolder = Join-Path $env:TEMP ('NoteUltra-ui-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $testFolder | Out-Null
    $process = Start-Process -FilePath (Join-Path $PSScriptRoot 'dist\NoteUltra-tests.exe') -ArgumentList @('--self-test',('"' + $testFolder + '"')) -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(60000)) { $process.Kill(); throw "UI tests timed out. Fixtures: $testFolder" }
    $process.Refresh()
    $report = Join-Path $testFolder 'ui-test-results.txt'
    if (Test-Path -LiteralPath $report) { Get-Content -LiteralPath $report; Copy-Item -LiteralPath $report -Destination .\ui-test-results.txt -Force }
    if ($process.ExitCode -ne 0) { throw "UI tests failed with code $($process.ExitCode). Fixtures: $testFolder" }
    Write-Host "Built: $PSScriptRoot\dist\NoteUltra.exe"
} finally { Pop-Location }
