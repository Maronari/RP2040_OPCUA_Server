param(
    [Parameter(Mandatory = $true)] [string]$PayloadBin,
    [Parameter(Mandatory = $true)] [string]$OutJson,
    [Parameter(Mandatory = $true)] [string]$Board,
    [Parameter(Mandatory = $true)] [string]$Platform,
    [Parameter(Mandatory = $true)] [string]$AppOrigin,
    [Parameter(Mandatory = $true)] [string]$BankBOffset,
    [Parameter(Mandatory = $true)] [string]$FirmwareVersion,
    [Parameter(Mandatory = $true)] [UInt32]$VersionNumber,
    [Parameter(Mandatory = $true)] [string]$GitHash
)

$payloadInfo = Get-Item $PayloadBin
$sha256 = (Get-FileHash -Path $PayloadBin -Algorithm SHA256).Hash.ToLowerInvariant()
$crc32 = 0xFFFFFFFF
$payloadBytes = [System.IO.File]::ReadAllBytes($PayloadBin)
foreach ($byte in $payloadBytes) {
    $crc32 = $crc32 -bxor [uint32]$byte
    for ($bit = 0; $bit -lt 8; $bit++) {
        if (($crc32 -band 1) -ne 0) {
            $crc32 = (($crc32 -shr 1) -bxor 0xEDB88320)
        }
        else {
            $crc32 = ($crc32 -shr 1)
        }
    }
}
$crc32 = (-bnot $crc32) -band 0xFFFFFFFF
$json = @{
    format_version = 1
    board = $Board
    platform = $Platform
    payload_file = [System.IO.Path]::GetFileName($PayloadBin)
    payload_size_bytes = [int64]$payloadInfo.Length
    payload_sha256 = $sha256
    payload_crc32 = ('0x{0:x8}' -f [uint32]$crc32)
    execution_origin = $AppOrigin
    staging_offset = $BankBOffset
    firmware_version = $FirmwareVersion
    version_number = [UInt32]$VersionNumber
    git_hash = $GitHash
} | ConvertTo-Json
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($OutJson)) | Out-Null
[System.IO.File]::WriteAllText($OutJson, $json + [Environment]::NewLine)
