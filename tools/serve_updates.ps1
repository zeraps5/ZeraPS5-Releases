# SPDX-License-Identifier: GPL-2.0-or-later
# Serves the published builds over HTTP on the local network (and receives
# the launchers' problem reports and build check-ins: tools/update_server.py), for the
# launchers' [atualizacao] fonte=http://<this PC's IP>:<port>. Runs until
# closed (Ctrl+C). Windows asks once to allow Python through the firewall:
# allow it on private networks only.
#
#   powershell -ExecutionPolicy Bypass -File tools\serve_updates.ps1 [-Dir <folder>] [-Port 8750]
param(
    [string]$Dir = "",
    [int]$Port = 8750
)
$root = Split-Path -Parent $PSScriptRoot
if (-not $Dir) { $Dir = Join-Path $root "releases\servidor" }
if (-not (Test-Path (Join-Path $Dir "latest.json"))) {
    Write-Error "Nothing published in $Dir yet (tools\publish.ps1 -Dest `"$Dir`")"
}
$ips = Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
    ForEach-Object { "http://$($_.IPAddress):$Port" }
Write-Host "Serving $Dir"
Write-Host "Launchers: Configurações > Atualizações = one of: $($ips -join '  ')"
python (Join-Path $PSScriptRoot "update_server.py") --dir $Dir --port $Port
