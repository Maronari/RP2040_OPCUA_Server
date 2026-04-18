param(
    [Parameter(Mandatory = $true)] [string]$BootloaderBin,
    [Parameter(Mandatory = $true)] [string]$AppBin,
    [Parameter(Mandatory = $true)] [int]$AppOffsetBytes,
    [Parameter(Mandatory = $true)] [string]$OutBin
)

$bootloader = [System.IO.File]::ReadAllBytes($BootloaderBin)
$app = [System.IO.File]::ReadAllBytes($AppBin)
if ($bootloader.Length -gt $AppOffsetBytes) {
    throw "Bootloader binary exceeds app offset: $($bootloader.Length) > $AppOffsetBytes"
}

$factory = New-Object byte[] ($AppOffsetBytes + $app.Length)
[System.Array]::Copy($bootloader, 0, $factory, 0, $bootloader.Length)
for ($index = $bootloader.Length; $index -lt $AppOffsetBytes; $index++) {
    $factory[$index] = 0xFF
}
[System.Array]::Copy($app, 0, $factory, $AppOffsetBytes, $app.Length)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($OutBin)) | Out-Null
[System.IO.File]::WriteAllBytes($OutBin, $factory)