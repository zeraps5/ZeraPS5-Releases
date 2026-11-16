[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AppRoot,
    [Parameter(Mandatory=$true)][string]$ExpectedVersion,
    [string]$ExpectedCertificateSubject,
    [string]$OutputJson,
    [switch]$RequireProductionSignature
)

$ErrorActionPreference = 'Stop'
$appDirectory = (Resolve-Path -LiteralPath $AppRoot).Path
$checks = foreach ($fileName in @('ZeraPS5.exe','ps5c.exe')) {
    $filePath = Join-Path $appDirectory $fileName
    $file = Get-Item -LiteralPath $filePath
    $info = $file.VersionInfo
    $signature = Get-AuthenticodeSignature -LiteralPath $filePath
    $subject = if ($signature.SignerCertificate) {$signature.SignerCertificate.Subject} else {$null}
    $publisherMatches = -not [string]::IsNullOrWhiteSpace($ExpectedCertificateSubject) -and $subject -ceq $ExpectedCertificateSubject
    $metadataReady = $info.ProductName -ceq 'ZeraPS5' -and $info.ProductVersion -ceq $ExpectedVersion -and $info.FileVersion -ceq $ExpectedVersion -and $info.CompanyName -ceq 'metallrockdev' -and $info.OriginalFilename -ceq $fileName
    [pscustomobject]@{
        file = $fileName
        sha256 = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
        product_name = $info.ProductName
        product_version = $info.ProductVersion
        file_version = $info.FileVersion
        company_name = $info.CompanyName
        original_filename = $info.OriginalFilename
        metadata_ready = [bool]$metadataReady
        signature_status = $signature.Status.ToString()
        certificate_subject = $subject
        expected_publisher_matches = [bool]$publisherMatches
        timestamp_present = [bool]$signature.TimeStamperCertificate
        production_ready = [bool]($metadataReady -and $signature.Status -eq 'Valid' -and $publisherMatches -and $signature.TimeStamperCertificate)
    }
}
$report = [pscustomobject]@{
    checked_utc = [DateTime]::UtcNow.ToString('o')
    mode = 'Read-only inspection; no binaries signed or changed'
    metadata_ready = @($checks | Where-Object {-not $_.metadata_ready}).Count -eq 0
    production_ready = @($checks | Where-Object {-not $_.production_ready}).Count -eq 0
    certificate_subject_approval_required = [string]::IsNullOrWhiteSpace($ExpectedCertificateSubject)
    files = @($checks)
}
$json = $report | ConvertTo-Json -Depth 6
if ($OutputJson) {
    [IO.File]::WriteAllText([IO.Path]::GetFullPath($OutputJson), $json, (New-Object Text.UTF8Encoding($false)))
}
$json
if ($RequireProductionSignature -and -not $report.production_ready) {exit 2}
