param(
    [Parameter(Mandatory=$true)][string]$IdentityName,
    [Parameter(Mandatory=$true)][string]$Publisher,
    [Parameter(Mandatory=$true)][string]$PublisherDisplayName,
    [string]$Version = '1.0.0.0',
    [string]$MakeAppx
)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
if (!$MakeAppx) {
    $sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $tool = Get-ChildItem -LiteralPath $sdk -Filter MakeAppx.exe -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.Directory.Name -eq 'x64' } | Sort-Object FullName -Descending | Select-Object -First 1
    if (!$tool) { throw 'Install Windows SDK MSIX packaging tools, or pass -MakeAppx with the tool path.' }
    $MakeAppx = $tool.FullName
}
$stage = Join-Path $project ('dist\msix-stage-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($name in @('NoteUltra.exe','Scintilla.dll','Lexilla.dll','Scintilla-LICENSE.txt','Lexilla-LICENSE.txt')) {
    $source = Join-Path $project ('dist\' + $name)
    if (!(Test-Path -LiteralPath $source)) { throw "Missing release file: $name. Build the app first." }
    Copy-Item -LiteralPath $source -Destination (Join-Path $stage $name)
}
& (Join-Path $PSScriptRoot 'Make-Assets.ps1') -Destination (Join-Path $stage 'Assets')
function XmlEscape([string]$Value) { [Security.SecurityElement]::Escape($Value) }
$xml = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'AppxManifest.template.xml'))
$xml = $xml.Replace('__IDENTITY__',(XmlEscape $IdentityName)).Replace('__PUBLISHER__',(XmlEscape $Publisher)).Replace('__DISPLAYPUBLISHER__',(XmlEscape $PublisherDisplayName)).Replace('__VERSION__',(XmlEscape $Version))
[IO.File]::WriteAllText((Join-Path $stage 'AppxManifest.xml'),$xml,[Text.UTF8Encoding]::new($false))
$output = Join-Path $project "dist\NoteUltra-$Version-x64.msix"
if (Test-Path -LiteralPath $output) { throw "Output exists: $output. Choose a new version or move that file." }
& $MakeAppx pack /d $stage /p $output
if ($LASTEXITCODE) { throw 'MSIX packaging failed' }
Write-Host "Unsigned MSIX created: $output"
Write-Host 'Validate on a clean machine and run Windows App Certification Kit before Store submission.'
