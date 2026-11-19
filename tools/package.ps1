# SPDX-License-Identifier: GPL-2.0-or-later
# Build and stage a local candidate; never uploads or publishes.
param(
    [string]$BuildDir,
    [string]$OutputDir,
    [string]$CMake = 'cmake',
    [string]$Toolchain = 'C:\msys64\ucrt64\bin',
    [switch]$SkipBuild
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
Set-Location $root
if(-not $BuildDir){$BuildDir=Join-Path $root 'build\distribution'}
if(-not $OutputDir){$OutputDir=Join-Path $root 'releases'}
$BuildDir=[IO.Path]::GetFullPath($BuildDir);$OutputDir=[IO.Path]::GetFullPath($OutputDir)
$env:PATH="$Toolchain;$env:PATH"
$dirty=git status --porcelain --untracked-files=all
if($dirty){throw 'Commit the candidate source before packaging'}
$commit=(git rev-parse HEAD).Trim()
if(-not $SkipBuild){
    & $CMake -S $root -B $BuildDir -G 'MinGW Makefiles' "-DCMAKE_MAKE_PROGRAM=$Toolchain/mingw32-make.exe" "-DCMAKE_C_COMPILER=$Toolchain/gcc.exe" -DCMAKE_BUILD_TYPE=Release -DPS5C_BUILD_TESTS=OFF
    if($LASTEXITCODE){throw 'Configure failed'}
    & $CMake --build $BuildDir --target ps5c zeraps5 --parallel 2
    if($LASTEXITCODE){throw 'Build failed'}
}
$exe=Join-Path $BuildDir 'ps5c.exe';$full=(& $exe --version).Trim()
if($full -match 'dirty' -or -not $full.Contains($commit.Substring(0,7))){throw 'Executable does not match the clean candidate commit'}
# A stale CMake cache can label an unoptimized build as Release. Check actual
# target options before producing distributable emulator executables.
foreach($target in @('ps5c','zeraps5','ps5c_base','ps5c_pal','ps5c_core')) {
    $options=Get-Content -LiteralPath "$BuildDir/CMakeFiles/$target.dir/flags.make" -Raw
    $flags=[regex]::Match($options,'(?m)^C_FLAGS\s*=\s*(.+)$').Groups[1].Value
    $optimization=[regex]::Matches($flags,'(?:^|\s)(-O(?:[0-3sg]|fast))(?=\s|$)')
    if(-not $optimization.Count -or $optimization[$optimization.Count-1].Groups[1].Value -notin @('-O2','-O3','-Os','-Ofast') -or $flags -notmatch '(?:^|\s)-DNDEBUG(?:\s|$)') {
        throw "Target $target is not an optimized distribution build; configure a fresh Release build directory"
    }
}
$version=($full -split '\+')[0];$build=[int]([regex]::Match($full,'build (\d+)').Groups[1].Value)
$name="ZeraPS5-v$version-b$build-win64"
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$stage=Join-Path $OutputDir $name
if(Test-Path -LiteralPath $stage){throw 'Package directory exists; select a fresh output folder'}
New-Item -ItemType Directory -Path $stage | Out-Null
foreach($dir in @('data','tools','docs','licenses','sources','games','system/modules')){New-Item -ItemType Directory -Path (Join-Path $stage $dir) -Force | Out-Null}
foreach($bin in @('ps5c.exe','ZeraPS5.exe')){
    $from=Join-Path $BuildDir $bin;$to=Join-Path $stage $bin
    $dlls=& "$Toolchain/objdump.exe" -p $from | Select-String 'DLL Name: (.+)' | ForEach-Object {$_.Matches[0].Groups[1].Value.Trim()}
    $bad=$dlls | Where-Object {$_ -notmatch '^api-ms-win-' -and -not (Test-Path -LiteralPath "$env:SystemRoot/System32/$_")}
    if($bad){throw "Non-system DLL dependency: $bad"}
    Copy-Item -LiteralPath $from -Destination $to
    & "$Toolchain/strip.exe" --strip-all $to
    if($LASTEXITCODE){throw 'Strip failed'}
}
Copy-Item -LiteralPath "$root/data/nid_names.txt","$root/data/entitlements.txt" -Destination "$stage/data"
Copy-Item -LiteralPath "$root/LICENSE","$root/CHANGELOG.md","$root/dist/CREDITS.txt" -Destination $stage
Copy-Item -LiteralPath "$root/tools/apply-update.ps1" -Destination "$stage/tools"
Copy-Item -Path "$root/dist/licenses/*" -Destination "$stage/licenses"
Copy-Item -Path "$root/dist/docs/*" -Destination "$stage/docs"
Copy-Item -LiteralPath "$root/COMPATIBILIDADE.md" -Destination "$stage/docs"
Get-ChildItem -LiteralPath "$stage/docs" -Filter 'HELP-*.txt' -File | ForEach-Object {
    (Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8).Replace('{VERSION}',$full) | Set-Content -LiteralPath $_.FullName -Encoding UTF8
}
(Get-Content -LiteralPath "$root/dist/LEIA-ME.txt" -Raw -Encoding UTF8).Replace('{VERSION}',$full) | Set-Content -LiteralPath "$stage/LEIA-ME.txt" -Encoding UTF8
Copy-Item -LiteralPath "$root/dist/LEIA-ME-system.txt" -Destination "$stage/system/LEIA-ME.txt"
Copy-Item -LiteralPath "$root/dist/LEIA-ME-games.txt" -Destination "$stage/games/LEIA-ME.txt"
# Real corresponding sources, without Git administration, historical reports,
# local builds, dumps, private maps, saves or machine configurations.
$source=Join-Path $stage 'sources/ZeraPS5-source.zip'
$allowed=@('CMakeLists.txt','.gitattributes','.clang-format','LICENSE','README.md','CHANGELOG.md','COMPATIBILIDADE.md','cmake','data','dist','frontends','include','src','tests','tools','third_party/vulkan','third_party/stb')
& git archive --format=zip "--output=$source" HEAD -- @allowed
if($LASTEXITCODE){throw 'Source archive failed'}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive=[IO.Compression.ZipFile]::Open($source,[IO.Compression.ZipArchiveMode]::Update)
try {
    $entry=$archive.CreateEntry('SOURCE-VERSION.txt')
    $writer=New-Object IO.StreamWriter($entry.Open())
    try {$writer.Write("commit=$commit`nbuild=$build`n")} finally {$writer.Dispose()}
} finally {$archive.Dispose()}
$ff=Join-Path $root 'third_party/ffmpeg/ffmpeg-7.1.2.tar.xz'
if((Get-FileHash -LiteralPath $ff -Algorithm SHA256).Hash.ToLower() -ne '089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304'){throw 'FFmpeg source checksum mismatch'}
Copy-Item -LiteralPath $ff -Destination "$stage/sources"
# Provide relinkable emulator objects as well as source for the static LGPL library.
$kit=Join-Path $OutputDir 'relink-kit'
if(Test-Path -LiteralPath $kit){throw 'Relink staging directory already exists'}
New-Item -ItemType Directory -Path $kit | Out-Null
$objects=Get-ChildItem -LiteralPath $BuildDir -Filter '*.obj' -Recurse -File | Where-Object {$_.FullName -match 'CMakeFiles[\\/](ps5c|zeraps5|ps5c_core|ps5c_pal|ps5c_base)\.dir[\\/]'}
foreach($obj in $objects){$rel=$obj.FullName.Substring($BuildDir.Length+1);$to=Join-Path $kit $rel;New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null;Copy-Item -LiteralPath $obj.FullName -Destination $to}
foreach($lib in @('libps5c_base.a','libps5c_pal.a','libps5c_core.a')){Copy-Item -LiteralPath (Join-Path $BuildDir $lib) -Destination $kit}
foreach($target in @('ps5c','zeraps5')){
    $to=Join-Path $kit "CMakeFiles/$target.dir";New-Item -ItemType Directory -Force $to | Out-Null
    (Get-Content -LiteralPath "$BuildDir/CMakeFiles/$target.dir/link.txt" -Raw).Replace($root.Replace('\','/'),'<SOURCE>').Replace($BuildDir.Replace('\','/'),'<BUILD>') | Set-Content -LiteralPath "$to/link.txt" -Encoding UTF8
}
Copy-Item -LiteralPath "$root/dist/docs/RECOMPILAR.txt" -Destination $kit
Compress-Archive -Path "$kit/*" -DestinationPath "$stage/sources/ZeraPS5-relink-objects.zip"
# The kit is private staging, resolved and verified inside OutputDir before deletion.
if(-not ([IO.Path]::GetFullPath($kit)).StartsWith($OutputDir+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe staging path'}
Remove-Item -LiteralPath $kit -Recurse -Force
$files=@(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {$_.FullName -notmatch '[\\/](games|system)[\\/]'} | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($stage.Length+1).Replace('\','/');size=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower()}
})
[ordered]@{schema=1;channel='stable';platform='windows-x86_64';build=$build;files=$files} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$stage/update-files.json" -Encoding UTF8
$zip=Join-Path $OutputDir "$name.zip"
Compress-Archive -LiteralPath $stage -DestinationPath $zip
$sha=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower()
"$sha  $name.zip" | Set-Content -LiteralPath "$zip.sha256" -Encoding ASCII
$languageMessages=Get-Content -LiteralPath "$root/frontends/launcher/translations_extra.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$notesLocalized=$languageMessages.PSObject.Properties['Versão experimental. Consulte LEIA-ME e limitações conhecidas.'].Value
$manifest=[ordered]@{schema=1;channel='stable';platform='windows-x86_64';name='ZeraPS5';version=$version;build=$build;full_version=$full;commit=$commit;zip="$name.zip";zip_sha256=$sha;notes='Versão experimental. Consulte LEIA-ME e limitações conhecidas.';notes_localized=$notesLocalized;source_zip="$name-source.zip";sources_included=$true}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$OutputDir/latest.json" -Encoding UTF8
Copy-Item -LiteralPath "$OutputDir/latest.json" -Destination "$OutputDir/$name.manifest.json"
Copy-Item -LiteralPath $source -Destination "$OutputDir/$name-source.zip"
$assets=Get-ChildItem -LiteralPath $OutputDir -File | Where-Object {$_.Name -ne 'SHA256SUMS'}
$assets | ForEach-Object {"$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.Name)"} | Set-Content -LiteralPath "$OutputDir/SHA256SUMS" -Encoding ASCII
"Package: $zip";"Version: $full";"SHA256: $sha"
