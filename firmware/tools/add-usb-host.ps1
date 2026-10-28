<#
.SYNOPSIS
  Add the USB host hand-off to a released star2lte UEFI boot image.

.DESCRIPTION
  The released UEFI images are built from a newer tree than firmware/ (their boot
  shim, BDS and recovery-watchdog chain are not all in this repository), so a
  from-source image would lose behaviour the installer relies on. This script
  instead edits the released image in place and changes only two things:

    1. Adds the Star2LteUsbHostDxe driver (built from this tree) to the DXE volume.
    2. Appends the USB0 SSDT (UsbHost.asl) to the ACPI tables file
       11111111-2222-3333-4444-555555555501. Every shipped table is kept byte for
       byte; Star2LteAcpiPlatformDxe installs each raw section of that file.

  The boot shim, every other firmware module and the boot image layout are taken
  from the released image unchanged (tools/repack-with-ref.py).

  Needs: EDK2 BaseTools built in firmware\edk2 (GenSec, GenFfs, FMMT), the driver
  built (.\tools\build-uefi.ps1 -Module ...Star2LteUsbHostDxe.inf), iasl on PATH.

.PARAMETER Released  A released star2lte-uefi-<build>.img.
.PARAMETER Output    The new boot image.
.PARAMETER Target    DEBUG (default) or RELEASE build of the driver.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)] [string]$Released,
  [Parameter(Mandatory)] [string]$Output,
  [ValidateSet("DEBUG", "RELEASE")] [string]$Target = "DEBUG"
)
$ErrorActionPreference = "Stop"

$ws    = Split-Path -Parent $PSScriptRoot
$edk2  = Join-Path $ws "edk2"
$bin   = Join-Path $edk2 "BaseTools\Bin\Win32"
$drv   = Join-Path $ws "Platform\Star2LtePkg\Drivers\Star2LteUsbHostDxe"
$built = Join-Path $edk2 "Build\Star2LtePkg\${Target}_CLANGPDB\AARCH64\Platform\Star2LtePkg\Drivers\Star2LteUsbHostDxe\Star2LteUsbHostDxe\OUTPUT"
$acpiGuid   = "11111111-2222-3333-4444-555555555501"
$driverGuid = "11111111-2222-3333-4444-555555555550"

foreach ($f in @((Join-Path $built "Star2LteUsbHostDxe.efi"), (Join-Path $built "Star2LteUsbHostDxe.depex"))) {
  if (-not (Test-Path $f)) { throw "Driver not built: $f. Run .\tools\build-uefi.ps1 -Module Platform\Star2LtePkg\Drivers\Star2LteUsbHostDxe\Star2LteUsbHostDxe.inf" }
}
if (-not (Get-Command iasl -ErrorAction SilentlyContinue)) { throw "iasl not on PATH." }

$work = Join-Path ([IO.Path]::GetTempPath()) ("s9-usbhost-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $work | Out-Null

$env:EDK_TOOLS_PATH  = Join-Path $edk2 "BaseTools"
$env:BASE_TOOLS_PATH = $env:EDK_TOOLS_PATH
$env:PYTHONPATH      = Join-Path $edk2 "BaseTools\Source\Python"
$env:PYTHON_COMMAND  = "python"
$env:PATH            = "$bin;" + (Join-Path $edk2 "BaseTools\BinWrappers\WindowsLike") + ";" + $env:PATH

function Invoke-Tool([string]$Exe, [string[]]$Arguments) {
  & $Exe @Arguments | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "$Exe failed ($LASTEXITCODE): $($Arguments -join ' ')" }
}

try {
  # 1. Split the released kernel into [shim][FD]. The FD starts with a firmware
  #    volume whose "_FVH" signature sits 0x28 bytes in.
  $split = Join-Path $work "split.py"
  @'
import struct, sys
img = open(sys.argv[1], "rb").read()
ksize, page = struct.unpack_from("<I", img, 8)[0], struct.unpack_from("<I", img, 36)[0]
kernel = img[page:page + ksize]
fv = kernel.find(b"_FVH") - 0x28
if fv <= 0:
    sys.exit("no firmware volume in the released kernel")
open(sys.argv[2], "wb").write(kernel[:fv])
open(sys.argv[3], "wb").write(kernel[fv:])
print(fv, len(kernel) - fv)
'@ | Set-Content -LiteralPath $split -Encoding ascii
  $shim = Join-Path $work "shim.bin"; $fd = Join-Path $work "released.fd"
  $sizes = python -I $split $Released $shim $fd
  if ($LASTEXITCODE -ne 0) { throw "Could not split $Released" }
  Write-Host "==> released kernel: shim + FD = $sizes bytes"

  # 2. The driver as an FFS: PE32 + DXE dependency + name.
  $pe = Join-Path $work "drv.pe32"; $dep = Join-Path $work "drv.depex"; $ui = Join-Path $work "drv.ui"
  Invoke-Tool (Join-Path $bin "GenSec.exe") @("-s", "EFI_SECTION_PE32", "-o", $pe, (Join-Path $built "Star2LteUsbHostDxe.efi"))
  Invoke-Tool (Join-Path $bin "GenSec.exe") @("-s", "EFI_SECTION_DXE_DEPEX", "-o", $dep, (Join-Path $built "Star2LteUsbHostDxe.depex"))
  Invoke-Tool (Join-Path $bin "GenSec.exe") @("-s", "EFI_SECTION_USER_INTERFACE", "-n", "Star2LteUsbHostDxe", "-o", $ui)
  $driverFfs = Join-Path $work "Star2LteUsbHostDxe.ffs"
  Invoke-Tool (Join-Path $bin "GenFfs.exe") @("-t", "EFI_FV_FILETYPE_DRIVER", "-g", $driverGuid, "-o", $driverFfs, "-i", $dep, "-i", $pe, "-i", $ui)

  # 3. The ACPI tables file: the released sections, unchanged, plus the USB0 SSDT.
  Invoke-Tool "iasl" @("-p", (Join-Path $work "UsbHost"), (Join-Path $drv "UsbHost.asl"))
  $acpiOld = Join-Path $work "AcpiTables.released.ffs"
  & powershell -NoProfile -Command "& python '$(Join-Path $edk2 'BaseTools\Source\Python\FMMT\FMMT.py')' -e '$fd' FV1 $acpiGuid '$acpiOld'" | Out-Null
  if (-not (Test-Path $acpiOld)) { throw "Could not extract the ACPI tables file from the released image." }
  $sections = Join-Path $work "sections.py"
  @'
import os, sys
d = open(sys.argv[1], "rb").read()
size = int.from_bytes(d[20:23], "little")
off, n, sigs = 24, 0, []
while off + 4 <= size:
    slen = int.from_bytes(d[off:off + 3], "little")
    if slen < 4:
        break
    if d[off + 3] != 0x19:
        sys.exit("unexpected non-raw section in the ACPI tables file")
    open(os.path.join(sys.argv[2], "s%02d.sec" % n), "wb").write(d[off:off + slen])
    sigs.append(d[off + 4:off + 8].decode("ascii", "replace"))
    n += 1
    off = (off + slen + 3) & ~3
print(" ".join(sigs))
'@ | Set-Content -LiteralPath $sections -Encoding ascii
  $secDir = Join-Path $work "acpi"; New-Item -ItemType Directory -Force $secDir | Out-Null
  $sigs = python -I $sections $acpiOld $secDir
  if ($LASTEXITCODE -ne 0) { throw "Could not read the ACPI tables file." }
  if ($sigs -match "USBHOST") { throw "This image already carries the USB host SSDT." }
  Write-Host "==> released ACPI tables: $sigs"
  $usbSec = Join-Path $secDir "s99.sec"
  Invoke-Tool (Join-Path $bin "GenSec.exe") @("-s", "EFI_SECTION_RAW", "-o", $usbSec, (Join-Path $work "UsbHost.aml"))
  $inputs = @(); Get-ChildItem $secDir -Filter *.sec | Sort-Object Name | ForEach-Object { $inputs += @("-i", $_.FullName) }
  $acpiNew = Join-Path $work "AcpiTables.ffs"
  Invoke-Tool (Join-Path $bin "GenFfs.exe") (@("-t", "EFI_FV_FILETYPE_FREEFORM", "-g", $acpiGuid, "-o", $acpiNew) + $inputs)

  # 4. Edit the released FD: replace the ACPI file and add the driver inside the
  #    compressed DXE volume, re-compressed to fit FV0 (see edit-dxe-volume.py).
  $fd2 = Join-Path $work "edited.fd"
  python (Join-Path $PSScriptRoot "edit-dxe-volume.py") --fd $fd --output $fd2 `
    --replace $acpiGuid $acpiNew --add $driverFfs `
    --tools $bin --fmmt (Join-Path $edk2 "BaseTools\Source\Python\FMMT\FMMT.py")
  if (($LASTEXITCODE -ne 0) -or -not (Test-Path $fd2)) { throw "Editing the DXE volume failed." }
  if ((Get-Item $fd2).Length -ne (Get-Item $fd).Length) {
    throw "The edited FD is $((Get-Item $fd2).Length) bytes, not the released $((Get-Item $fd).Length)."
  }

  # 5. [released shim][edited FD] back into the released boot image.
  $kernel = Join-Path $work "kernel.bin"
  [IO.File]::WriteAllBytes($kernel, [IO.File]::ReadAllBytes($shim) + [IO.File]::ReadAllBytes($fd2))
  python (Join-Path $PSScriptRoot "repack-with-ref.py") --ref $Released --kernel $kernel --output $Output --verify-ref
  if ($LASTEXITCODE -ne 0) { throw "repack-with-ref.py failed." }
  Write-Host "==> USB host image: $Output" -ForegroundColor Green
} finally {
  if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}

