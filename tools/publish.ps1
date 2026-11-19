# SPDX-License-Identifier: GPL-2.0-or-later
# Publishes the newest package in releases\ to an update source: the folder the
# launchers read ([atualizacao] fonte, e.g. a LAN share \\PC\ZeraPS5). Copies the
# zip and writes latest.json (its manifest) last, so launchers never see a
# manifest whose zip is not there yet. Older zips stay (a way back).
#
#   powershell -ExecutionPolicy Bypass -File tools\publish.ps1 -Dest C:\ZeraPS5-atualizacoes
param([Parameter(Mandatory = $true)][string]$Dest)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$releases = Join-Path $root "releases"
$manifest = Get-ChildItem "$releases\ZeraPS5-*.manifest.json" | Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $manifest) { Write-Error "No package in releases\ (run tools\package.ps1 first)" }
$m = Get-Content $manifest.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
if ($m.full_version -match "-dirty") { Write-Error "The package was built from uncommitted sources: not published" }
$zip = Join-Path $releases $m.zip
if ((Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower() -ne $m.zip_sha256) { Write-Error "$($m.zip) does not match its manifest" }

New-Item -ItemType Directory -Force $Dest | Out-Null
$current = Join-Path $Dest "latest.json"
if (Test-Path $current) {
    $old = Get-Content $current -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([int]$old.build -gt [int]$m.build) { Write-Error "$Dest already has build $($old.build), newer than $($m.build)" }
}
Copy-Item $zip $Dest -Force
Copy-Item $manifest.FullName (Join-Path $Dest "latest.json.tmp") -Force
Move-Item (Join-Path $Dest "latest.json.tmp") $current -Force
Write-Host "Published $($m.full_version) (build $($m.build)) to $Dest"
