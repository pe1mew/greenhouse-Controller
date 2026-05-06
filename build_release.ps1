<#
.SYNOPSIS
    Build a versioned release package for the Greenhouse Controller.

.DESCRIPTION
    1. Reads FIRMWARE_VERSION from firmware/platformio.ini.
    2. Builds the firmware binary   (pio run -e lolin_s3).
    3. Builds the LittleFS image    (pio run -e lolin_s3 -t buildfs).
    4. Creates a STORE-only ZIP of firmware/data/  (method=0, no deflate).
    5. Copies both artefacts to bin\<version>\ with the version in the filename.

.OUTPUTS
    bin\<version>\greenhouse-controller-<version>.bin
    bin\<version>\web-assets-<version>.zip

.NOTES
    Run from the project root:
        powershell -ExecutionPolicy Bypass -File .\build_release.ps1

    The web-assets ZIP uses ZIP STORE (no compression) because the on-device
    OTA extractor only handles method=0.  DEFLATE entries are rejected at
    flash time with a diagnostic error message.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
$ROOT_DIR     = $PSScriptRoot
$FIRMWARE_DIR = Join-Path $ROOT_DIR "firmware"
$PIO          = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"

if (-not (Test-Path $PIO)) {
    Write-Error "PlatformIO not found at: $PIO  -- install PlatformIO Core first."
    exit 1
}

# ---------------------------------------------------------------------------
# Read FIRMWARE_VERSION from platformio.ini
# ---------------------------------------------------------------------------
$INI_PATH = Join-Path $FIRMWARE_DIR "platformio.ini"
$ini_text = Get-Content $INI_PATH -Raw

if ($ini_text -match 'FIRMWARE_VERSION=\\"([0-9]+\.[0-9]+\.[0-9]+)\\"') {
    $VERSION = $Matches[1]
} else {
    Write-Error "Could not find FIRMWARE_VERSION in $INI_PATH"
    exit 1
}

# ---------------------------------------------------------------------------
# Output directory
# ---------------------------------------------------------------------------
$OUT_DIR = Join-Path $ROOT_DIR "bin\$VERSION"

Write-Host ""
Write-Host "=== Greenhouse Controller - Release Builder ===" -ForegroundColor Cyan
Write-Host "    Version  : $VERSION"
Write-Host "    Firmware : $FIRMWARE_DIR"

if (Test-Path $OUT_DIR) {
    Write-Host "    Output   : $OUT_DIR  (exists - files will be overwritten)"
} else {
    New-Item -ItemType Directory -Force -Path $OUT_DIR | Out-Null
    Write-Host "    Output   : $OUT_DIR  (created)"
}
Write-Host ""

# ---------------------------------------------------------------------------
# Step 1 - Build firmware
# ---------------------------------------------------------------------------
Write-Host "--- Step 1/3: Build firmware ---" -ForegroundColor Yellow
Push-Location $FIRMWARE_DIR
try {
    & $PIO run -e lolin_s3
    if ($LASTEXITCODE -ne 0) { throw "pio run failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}

$BIN_SRC = Join-Path $FIRMWARE_DIR ".pio\build\lolin_s3\firmware.bin"
$BIN_DST = Join-Path $OUT_DIR "greenhouse-controller-$VERSION.bin"
Copy-Item -Force $BIN_SRC $BIN_DST

$bin_kb = [math]::Round((Get-Item $BIN_DST).Length / 1KB, 1)
Write-Host "    -> $BIN_DST  ($bin_kb KB)" -ForegroundColor Green
Write-Host ""

# ---------------------------------------------------------------------------
# Step 2 - Build LittleFS image (validates data/ is complete)
# ---------------------------------------------------------------------------
Write-Host "--- Step 2/3: Build LittleFS image ---" -ForegroundColor Yellow
Push-Location $FIRMWARE_DIR
try {
    & $PIO run -e lolin_s3 -t buildfs
    if ($LASTEXITCODE -ne 0) { throw "pio buildfs failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}
Write-Host ""

# ---------------------------------------------------------------------------
# Step 3 - Create STORE-only ZIP of firmware/data/
#
# .NET Framework (PowerShell 5.1) writes method=8 (Deflate level 0) even
# when CompressionLevel.NoCompression is requested -- fixed only in .NET 6+.
# The on-device ZIP extractor rejects method=8, so we build the ZIP manually
# by writing raw Local File Headers, Central Directory, and EOCD with
# compression method = 0 (STORE).
# ---------------------------------------------------------------------------
Write-Host "--- Step 3/3: Package web assets (STORE method=0) ---" -ForegroundColor Yellow

$DATA_DIR = Join-Path $FIRMWARE_DIR "data"
$ZIP_DST  = Join-Path $OUT_DIR "web-assets-$VERSION.zip"

if (Test-Path $ZIP_DST) { Remove-Item -Force $ZIP_DST }

# CRC-32 lookup table using Int64 arithmetic to avoid PowerShell 5.1 signed-
# int32 overflow: hex literals > 0x7FFFFFFF (e.g. 0xEDB88320) are parsed as
# Int32(-x) in PS 5.1, causing cast-to-uint32 failures. Use decimal [long].
$CRC_POLY = [long]3988292384   # 0xEDB88320  reflected polynomial
$CRC_INIT = [long]4294967295   # 0xFFFFFFFF
$CRC_MASK = [long]4294967295   # 0xFFFFFFFF  (32-bit mask)
$CRC_0xFF = [long]255

$crc_table = New-Object long[] 256
for ($i = 0; $i -lt 256; $i++) {
    $c = [long]$i
    for ($j = 0; $j -lt 8; $j++) {
        if ($c -band 1L) { $c = ($c -shr 1) -bxor $CRC_POLY }
        else             { $c = $c -shr 1 }
    }
    $crc_table[$i] = $c
}

function Get-Crc32([byte[]]$data) {
    $crc = $CRC_INIT
    foreach ($b in $data) {
        $idx = ($crc -bxor [long]$b) -band $CRC_0xFF
        $crc = $crc_table[$idx] -bxor (($crc -shr 8) -band $CRC_MASK)
    }
    return [uint32](($crc -bxor $CRC_INIT) -band $CRC_MASK)
}

function Write-U16([System.IO.MemoryStream]$s, [long]$v) {
    $s.WriteByte([byte]($v -band 255L))
    $s.WriteByte([byte](($v -shr 8) -band 255L))
}

function Write-U32([System.IO.MemoryStream]$s, [long]$v) {
    $s.WriteByte([byte]($v -band 255L))
    $s.WriteByte([byte](($v -shr 8)  -band 255L))
    $s.WriteByte([byte](($v -shr 16) -band 255L))
    $s.WriteByte([byte](($v -shr 24) -band 255L))
}

$ms      = New-Object System.IO.MemoryStream
$cd_list = [System.Collections.ArrayList]::new()
$enc     = [System.Text.Encoding]::UTF8

# Fixed DOS timestamp: 2000-01-01 00:00:00
$DOS_DATE = [uint32]0x2821   # (2000-1980)<<9 | 1<<5 | 1
$DOS_TIME = [uint32]0x0000

$files = Get-ChildItem -Path $DATA_DIR -File | Sort-Object Name
foreach ($f in $files) {
    $data       = [System.IO.File]::ReadAllBytes($f.FullName)
    $crc        = Get-Crc32 $data
    $name_bytes = $enc.GetBytes($f.Name)
    $name_len   = [uint32]$name_bytes.Length
    $data_len   = [uint32]$data.Length
    $lhdr_off   = [uint32]$ms.Position

    # Local File Header  (method=0 STORE)
    Write-U32 $ms 0x04034B50   # signature PK\x03\x04
    Write-U16 $ms 20            # version needed (2.0)
    Write-U16 $ms 0             # general purpose bit flag
    Write-U16 $ms 0             # compression method: STORE
    Write-U16 $ms $DOS_TIME     # last mod time
    Write-U16 $ms $DOS_DATE     # last mod date
    Write-U32 $ms $crc          # CRC-32
    Write-U32 $ms $data_len     # compressed size   (= uncompressed for STORE)
    Write-U32 $ms $data_len     # uncompressed size
    Write-U16 $ms $name_len     # file name length
    Write-U16 $ms 0             # extra field length
    $ms.Write($name_bytes, 0, $name_bytes.Length)
    $ms.Write($data, 0, $data.Length)

    $cd_list.Add([PSCustomObject]@{
        crc       = $crc
        size      = $data_len
        name_bytes = $name_bytes
        offset    = $lhdr_off
    }) | Out-Null

    $kb = [math]::Round($f.Length / 1KB, 1)
    Write-Host "    + $($f.Name)  ($kb KB)"
}

# Central Directory
$cd_offset = [uint32]$ms.Position
foreach ($e in $cd_list) {
    $nl = [uint32]$e.name_bytes.Length
    Write-U32 $ms 0x02014B50   # signature PK\x01\x02
    Write-U16 $ms 20            # version made by
    Write-U16 $ms 20            # version needed
    Write-U16 $ms 0             # general flag
    Write-U16 $ms 0             # compression: STORE
    Write-U16 $ms $DOS_TIME
    Write-U16 $ms $DOS_DATE
    Write-U32 $ms $e.crc
    Write-U32 $ms $e.size
    Write-U32 $ms $e.size
    Write-U16 $ms $nl           # file name length
    Write-U16 $ms 0             # extra field length
    Write-U16 $ms 0             # file comment length
    Write-U16 $ms 0             # disk number start
    Write-U16 $ms 0             # internal attributes
    Write-U32 $ms 0             # external attributes
    Write-U32 $ms $e.offset     # relative offset of local header
    $ms.Write($e.name_bytes, 0, $e.name_bytes.Length)
}

$cd_size   = [uint32]($ms.Position - $cd_offset)
$n_entries = [uint32]$cd_list.Count

# End of Central Directory
Write-U32 $ms 0x06054B50   # signature PK\x05\x06
Write-U16 $ms 0             # disk number
Write-U16 $ms 0             # start disk
Write-U16 $ms $n_entries    # entries on this disk
Write-U16 $ms $n_entries    # total entries
Write-U32 $ms $cd_size      # central directory size
Write-U32 $ms $cd_offset    # central directory offset
Write-U16 $ms 0             # comment length

[System.IO.File]::WriteAllBytes($ZIP_DST, $ms.ToArray())
$ms.Dispose()

# Verify: check the compression method byte in the first local file header (offset 8)
$zip_bytes = [System.IO.File]::ReadAllBytes($ZIP_DST)
$method    = [uint16]($zip_bytes[8] -bor ($zip_bytes[9] -shl 8))
if ($method -ne 0) {
    Write-Error "ZIP verification failed: first entry has method=$method (expected 0=STORE)"
    exit 1
}

$zip_kb = [math]::Round((Get-Item $ZIP_DST).Length / 1KB, 1)
Write-Host "    -> $ZIP_DST  ($zip_kb KB, method=STORE verified)" -ForegroundColor Green
Write-Host ""

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host "=== Done - release v$VERSION ===" -ForegroundColor Cyan
Write-Host "    $BIN_DST"
Write-Host "    $ZIP_DST"
Write-Host ""
