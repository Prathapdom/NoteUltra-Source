param([string]$Destination = 'F:\NoteUltra')
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
$target = [IO.Path]::GetFullPath($Destination).TrimEnd('\')
if ($source -eq $target) { Write-Host "Already in $target"; exit 0 }
if ($target.StartsWith($source + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Choose a destination outside the source folder.' }
if (Test-Path -LiteralPath $target) {
    if (Get-ChildItem -LiteralPath $target -Force | Select-Object -First 1) { throw "Destination is not empty; choose an empty folder: $target" }
} else { New-Item -ItemType Directory -Path $target -Force | Out-Null }
Get-ChildItem -LiteralPath $source -Force | Where-Object { $_.Extension -ne '.zip' } | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $target -Recurse }
Write-Host "Copied app and source to $target"
Write-Host "Run $target\dist\NoteUltra.exe"
