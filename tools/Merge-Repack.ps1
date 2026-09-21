[CmdletBinding()]
param(
    [string]$InputDirectory = '',
    [string]$OutputPath = ''
)
$ErrorActionPreference = 'Stop'
$archiveName = 'WER_REPACK_VER.1.1.0.zip'
if (-not $InputDirectory) { $InputDirectory = $PSScriptRoot }
$inputRoot = (Resolve-Path -LiteralPath $InputDirectory).Path
if (-not $OutputPath) { $OutputPath = Join-Path $inputRoot $archiveName }
$outputFull = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $outputFull) { throw 'Output already exists. It will not be overwritten.' }
$checksumPath = Join-Path $inputRoot 'SHA256SUMS.txt'
if (-not (Test-Path -LiteralPath $checksumPath -PathType Leaf)) { throw 'Download SHA256SUMS.txt first.' }
$hashes = @{}
foreach ($line in (Get-Content -LiteralPath $checksumPath)) {
    if ($line -match '^([0-9a-fA-F]{64})  (.+)$') {
        if ($hashes.ContainsKey($Matches[2])) { throw 'Duplicate checksum entry.' }
        $hashes[$Matches[2]] = $Matches[1].ToLowerInvariant()
    }
}
$partNames = @(($archiveName + '.001'), ($archiveName + '.002'))
foreach ($name in @($partNames) + @($archiveName)) {
    if (-not $hashes.ContainsKey($name)) { throw "Missing checksum: $name" }
}
foreach ($name in $partNames) {
    $part = Join-Path $inputRoot $name
    if (-not (Test-Path -LiteralPath $part -PathType Leaf)) { throw "Missing part: $name" }
    Write-Host "Checking $name ..."
    if ((Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hashes[$name]) {
        throw "Checksum mismatch: $name. Download the file again."
    }
}
$outputStream = $null
try {
    $outputStream = [IO.File]::Open($outputFull, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    foreach ($name in $partNames) {
        $inputStream = [IO.File]::OpenRead((Join-Path $inputRoot $name))
        try { $inputStream.CopyTo($outputStream, 1048576) }
        finally { $inputStream.Dispose() }
    }
}
catch {
    Write-Warning 'Merge failed. An incomplete output may remain; inspect it before retrying.'
    throw
}
finally { if ($null -ne $outputStream) { $outputStream.Dispose() } }
$actual = (Get-FileHash -LiteralPath $outputFull -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $hashes[$archiveName]) { throw 'Merged ZIP checksum mismatch. Do not use this output.' }
Write-Host "Verified ZIP: $outputFull"
Write-Host "SHA256: $actual"
Write-Host 'Extract the ZIP to a new folder. Keep all download parts until extraction succeeds.'
