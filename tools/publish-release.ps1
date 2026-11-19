# SPDX-License-Identifier: GPL-2.0-or-later
# FUTURE manual publication only. Never called by build/package scripts.
[CmdletBinding(SupportsShouldProcess=$true,ConfirmImpact='High')]
param(
    [Parameter(Mandatory=$true)][string]$PackageDir,
    [switch]$AuthorizePublication
)
$ErrorActionPreference='Stop'
if(-not $AuthorizePublication){throw 'Publication is not authorized. Review the package and explicitly pass -AuthorizePublication when the user approves publication.'}
$repo='zeraps5/ZeraPS5-Releases'
$dir=[IO.Path]::GetFullPath($PackageDir)
$m=Get-Content -LiteralPath (Join-Path $dir 'latest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if($m.schema -ne 1 -or $m.channel -ne 'stable' -or $m.platform -ne 'windows-x86_64' -or $m.full_version -match 'dirty'){throw 'Not a clean stable Windows x64 package'}
$zip=Join-Path $dir $m.zip
if((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $m.zip_sha256){throw 'Package hash mismatch'}
$source=Join-Path $dir ($m.zip -replace '\.zip$','-source.zip')
if(-not (Test-Path -LiteralPath $source)){throw 'Real corresponding source archive is required'}
# Do not create repositories or alter visibility. Publisher authenticates gh
# separately; no token is stored in the client, sources, manifest, or package.
$visibility=& gh repo view $repo --json isPrivate --jq .isPrivate
if($LASTEXITCODE -or $visibility.Trim() -ne 'false'){throw 'The separately approved PUBLIC distribution repository must already exist'}
$tag="v$($m.version)-b$($m.build)"
$assets=@($zip,$source,"$zip.sha256",(Join-Path $dir 'latest.json'),(Join-Path $dir 'SHA256SUMS'))
foreach($file in $assets){if(-not(Test-Path -LiteralPath $file)){throw 'Missing release asset'}}
if($PSCmdlet.ShouldProcess("$repo / $tag",'Upload the reviewed binary, real corresponding sources, checksums and stable update manifest')){
    # Publishing a draft is a separate review step. Explicitly publish it in
    # GitHub only after downloading and reviewing its assets and sources.
    & gh release create $tag @assets --repo $repo --draft --title "ZeraPS5 $tag" --notes 'Experimental Windows x64 build. Real corresponding sources and FFmpeg accompany the binary ZIP. See included credits, licenses and known limitations.'
    if($LASTEXITCODE){throw 'Draft upload failed'}
}
