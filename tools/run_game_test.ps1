# SPDX-License-Identifier: GPL-2.0-or-later
# Uses the emulator's controller script, never the Windows keyboard/mouse.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$GameExe,
    [Parameter(Mandatory=$true)][string]$Modules,
    [Parameter(Mandatory=$true)][string]$NidDb,
    [Parameter(Mandatory=$true)][string]$ProfileTemplate,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$TestName='game',
    [string]$EntitlementsFile=(Join-Path $PSScriptRoot '../data/entitlements.txt'),
    [ValidateSet(128,256,512)][int]$ArenaMiB=128,
    [ValidateSet(0,1)][int]$HoldReleases=1,
    [ValidateSet(0,1)][int]$MemoryReaders=0,
    [switch]$VertexRanges,
    [switch]$ReadOnlyCache,
    [switch]$ScriptOnly,
    [switch]$NoTraffic,
    [switch]$LowSharedArea,
    [switch]$CpWaits,
    [switch]$ShaderCorpus,
    [ValidateRange(0,3600)][int]$SampleSeconds=0,
    [ValidateRange(0,3600)][int]$GpuSampleSeconds=0,
    [ValidateRange(0,1000000)][int]$CaptureEvery=0,
    [string]$PadScript=''
)
$ErrorActionPreference='Stop'
foreach ($taskPath in @($Executable,$GameExe,$Modules,$NidDb,$ProfileTemplate,$EntitlementsFile)) {
    if (-not (Test-Path -LiteralPath $taskPath)) { throw "Missing test input: $taskPath" }
    if ($taskPath.Contains('"')) { throw 'Input paths must not contain quotation marks' }
}
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$GameExe=(Resolve-Path -LiteralPath $GameExe).Path
$Modules=(Resolve-Path -LiteralPath $Modules).Path
$NidDb=(Resolve-Path -LiteralPath $NidDb).Path
$ProfileTemplate=(Resolve-Path -LiteralPath $ProfileTemplate).Path
$EntitlementsFile=(Resolve-Path -LiteralPath $EntitlementsFile).Path
if ($ScriptOnly -and -not $PadScript) { throw 'ScriptOnly requires an explicit PadScript' }
$OutputDirectory=[System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a fresh output directory; existing runs are preserved' }
if ($OutputDirectory.StartsWith($ProfileTemplate.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Output directory must be outside the profile template'
}
if (-not (Get-Item -LiteralPath $ProfileTemplate).PSIsContainer) { throw 'ProfileTemplate must be a directory' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
Copy-Item -LiteralPath $ProfileTemplate -Destination (Join-Path $OutputDirectory 'user') -Recurse
New-Item -ItemType Directory -Path (Join-Path $OutputDirectory 'data') | Out-Null
Copy-Item -LiteralPath $EntitlementsFile -Destination (Join-Path $OutputDirectory 'data\entitlements.txt')
if ($CaptureEvery) { New-Item -ItemType Directory -Path (Join-Path $OutputDirectory 'user\frames') -Force | Out-Null }
$taskExe=Join-Path $OutputDirectory "ps5c-$TestName-test.exe"
Copy-Item -LiteralPath $Executable -Destination $taskExe
$taskFlags=@('PS5C_VERTEX_RANGES','PS5C_READONLY_CACHE','PS5C_GPU_PROFILE','PS5C_DEVICE_ARENA',
    'PS5C_DEVICE_UNIFORMS','PS5C_SAVE_FRAMES','PS5C_SAVE_TARGETS','PS5C_LOG_SUBMITS',
    'PS5C_WB_LOG','PS5C_FAULT_DUMP_AT','PS5C_SAMPLE','PS5C_DUMP','PS5C_LOG_MAPS',
    'PS5C_LOG_BUFFERS','PS5C_PAD_SCRIPT','PS5C_PAD_SCRIPT_ONLY','PS5C_ARENA_MIB','PS5C_GPU_STATS',
    'PS5C_BUFFER_TRAFFIC','PS5C_FAULT_MAPS','PS5C_FAULT_CODE','PS5C_HOLD_RELEASES',
    'PS5C_MEMORY_READERS','PS5C_GPU_SAMPLE','PS5C_STORE_WINDOWS','PS5C_NO_SPAN_CACHE','PS5C_NO_EVAL_CACHE',
    'PS5C_LOW_SHARED_AREA','PS5C_NO_SHARED_AREA','PS5C_SHADER_CORPUS','PS5C_CP_WAITS')
$taskOldEnv=@{}
foreach ($taskFlag in $taskFlags) {
    $taskOldEnv[$taskFlag]=[Environment]::GetEnvironmentVariable($taskFlag,'Process')
}
$taskHash=[System.Security.Cryptography.SHA256]::Create()
$taskStream=[System.IO.File]::OpenRead($taskExe)
try { $taskSha256=[BitConverter]::ToString($taskHash.ComputeHash($taskStream)).Replace('-','') }
finally { $taskStream.Dispose(); $taskHash.Dispose() }
$taskState=[ordered]@{
    test_name=$TestName
    executable_sha256=$taskSha256
    pad_script=$PadScript; script_only=[bool]$ScriptOnly; arena_mib=$ArenaMiB; vertex_ranges=[bool]$VertexRanges
    readonly_cache=[bool]$ReadOnlyCache
    sample_seconds=$SampleSeconds; gpu_sample_seconds=$GpuSampleSeconds; captures_every=$CaptureEvery
    traffic_enabled=(-not [bool]$NoTraffic)
    hold_releases=$HoldReleases
    memory_readers=$MemoryReaders
    low_shared_area=[bool]$LowSharedArea
    cp_waits=[bool]$CpWaits
    shader_corpus=[bool]$ShaderCorpus
    inicio=(Get-Date).ToString('o'); pid=$null; fim=$null; codigo_saida=$null; erro=$null
}
function Write-TestState {
    $taskManifest=Join-Path $OutputDirectory 'processo.json'
    $taskPending="$taskManifest.$PID.new"
    $taskState | ConvertTo-Json | Set-Content -LiteralPath $taskPending -Encoding utf8
    for ($taskAttempt=0; $taskAttempt -lt 10; $taskAttempt++) {
        try {
            Move-Item -LiteralPath $taskPending -Destination $taskManifest -Force -ErrorAction Stop
            return
        } catch [System.IO.IOException] {
            if ($taskAttempt -eq 9) { throw }
            Start-Sleep -Milliseconds 50
        }
    }
}
try {
    foreach ($taskFlag in $taskFlags) { [Environment]::SetEnvironmentVariable($taskFlag,$null,'Process') }
    $env:PS5C_PAD_SCRIPT=$PadScript
    if ($ScriptOnly) { $env:PS5C_PAD_SCRIPT_ONLY='1' }
    $env:PS5C_ARENA_MIB=[string]$ArenaMiB
    $env:PS5C_HOLD_RELEASES=[string]$HoldReleases
    $env:PS5C_MEMORY_READERS=[string]$MemoryReaders
    if ($LowSharedArea) { $env:PS5C_LOW_SHARED_AREA='1' }
    if ($CpWaits) { $env:PS5C_CP_WAITS='1' }
    if ($ShaderCorpus) { $env:PS5C_SHADER_CORPUS='1' }
    $env:PS5C_VERTEX_RANGES=if ($VertexRanges) { '1' } else { '0' }
    if ($ReadOnlyCache) { $env:PS5C_READONLY_CACHE='1' }
    if ($SampleSeconds) { $env:PS5C_SAMPLE=[string]$SampleSeconds }
    if ($GpuSampleSeconds) { $env:PS5C_GPU_SAMPLE=[string]$GpuSampleSeconds }
    if ($CaptureEvery) { $env:PS5C_SAVE_FRAMES=[string]$CaptureEvery }
    $env:PS5C_GPU_STATS='1'
    if (-not $NoTraffic) { $env:PS5C_BUFFER_TRAFFIC='1' }
    $env:PS5C_FAULT_MAPS='1'
    $env:PS5C_FAULT_CODE='1'
    $taskArgs=@('run',('"'+$GameExe+'"'),'--modules',('"'+$Modules+'"'),'--nid-db',('"'+$NidDb+'"'))
    $taskProcess=Start-Process -FilePath $taskExe -ArgumentList $taskArgs -WorkingDirectory $OutputDirectory -WindowStyle Normal -RedirectStandardOutput (Join-Path $OutputDirectory 'stdout.log') -RedirectStandardError (Join-Path $OutputDirectory "$TestName.log") -PassThru
    $taskHandle=$taskProcess.Handle
    $taskState.pid=$taskProcess.Id
    Write-TestState
    $taskProcess.WaitForExit()
    $taskProcess.Refresh()
    $taskState.codigo_saida=$taskProcess.ExitCode
} catch {
    $taskState.erro=$_.Exception.Message
    throw
} finally {
    $taskState.fim=(Get-Date).ToString('o')
    try { Write-TestState }
    finally {
        foreach ($taskFlag in $taskFlags) { [Environment]::SetEnvironmentVariable($taskFlag,$taskOldEnv[$taskFlag],'Process') }
    }
}
