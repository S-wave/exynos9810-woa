<#
.SYNOPSIS
  Build the release assets that are not firmware or drivers: the installer zips and the
  prebuilt WinRE-look recovery.

.DESCRIPTION
  S9WoaInstaller-<version>-x64.zip, S9WoaInstaller-<version>-arm64.zip
      dotnet publish of the installer (self-contained, no debug symbols), with the project's
      LICENSE and NOTICE, a README, and the license terms of the bundled .NET and Windows App
      SDK runtimes under licenses\.
  star2lte-winre-recovery.img
      the WinRE-look recovery built from the official twrp-3.7.0_9-0-star2lte.img with the open
      fonts in tools/twrp-winre/fonts and the built-in gears: redistributable, unlike the
      recovery the installer builds on a user's PC with Segoe UI (and maybe their UpdateOS
      gears). Its build stamp records both, and make-payload.ps1 refuses any other recovery.

  Then pass the files to make-payload.ps1 -Assets. Needs PowerShell 7 (pwsh) and the .NET 8 SDK.

.PARAMETER Twrp     The official twrp-3.7.0_9-0-star2lte.img (from twrp.me). Optional: without it
                  only the installer zips are built, for a release that republishes an
                  existing star2lte-winre-recovery.img.
.PARAMETER Version  Installer version, e.g. 0.1.0.
.PARAMETER OutDir   Output folder (created; must be empty or absent).
#>
[CmdletBinding()]
param(
  [string]$Twrp,
  [Parameter(Mandatory)] [string]$Version,
  [Parameter(Mandatory)] [string]$OutDir
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$app = Join-Path $repo "installer\src\S9Woa.Installer.App\S9Woa.Installer.App.csproj"
$out = [IO.Path]::GetFullPath($OutDir)
if ((Test-Path -LiteralPath $out) -and (Get-ChildItem -LiteralPath $out -Force)) { throw "$out is not empty." }
New-Item -ItemType Directory -Force -Path $out | Out-Null
$work = Join-Path ([IO.Path]::GetTempPath()) ("s9woa-extras-" + [guid]::NewGuid().ToString("N"))
$nuget = if ($env:NUGET_PACKAGES) { $env:NUGET_PACKAGES } else { Join-Path $env:USERPROFILE ".nuget\packages" }

$readme = @"
Galaxy S9+ Windows Installer $Version
==================================

Installs Windows 11 (ARM64) on a Samsung Galaxy S9+ SM-G965F (Exynos 9810), from
stock Android to the Windows desktop. 64, 128 and 256 GB phones are supported.

Run S9WoaInstaller.exe (it asks for administrator rights: DISM and the disk tools
need them). Needs Windows 10 2004 or newer on an x64 or ARM64 PC, about 80 GB of
free space, a USB data cable, and your own Windows 11 ARM64 media.

The first run opens Set up: "Set up automatically" installs adb (winget) and
downloads the UEFI images and phone drivers from this project's GitHub release,
verified against the release's SHA256SUMS. You add Samsung's USB driver and the
official twrp-3.7.0_9-0-star2lte.img from twrp.me (or the prebuilt
star2lte-winre-recovery.img from the release).

Portable mode: create an empty folder named "data" next to S9WoaInstaller.exe
and the installer keeps its settings, logs and phone backups there instead of
%LOCALAPPDATA%\S9WoaInstaller.

Installing erases Android. Keep the backup the installer makes: it is how you
return to stock. No warranty; see LICENSE.txt.

This package includes the .NET runtime and the Windows App SDK runtime, which
are Microsoft's and are redistributed under their own terms (licenses\). By
using this package you agree to those terms, including the Windows App SDK
license terms in licenses\WindowsAppSDK-license.txt.

Source: https://github.com/S-wave/exynos9810-woa
"@

try {
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  foreach ($arch in "x64", "arm64") {
    $platform = if ($arch -eq "x64") { "x64" } else { "ARM64" }
    $stage = Join-Path $work "$arch\S9WoaInstaller"
    & dotnet publish $app -c Release "-p:Platform=$platform" -r "win-$arch" "-p:Version=$Version" `
      -p:DebugType=none -p:DebugSymbols=false -p:ContinuousIntegrationBuild=true -o $stage --nologo -v q
    if ($LASTEXITCODE -ne 0) { throw "dotnet publish ($arch) failed." }
    # An unpackaged WinUI app loads its XAML from its resource index; without it the window cannot open.
    if (-not (Test-Path (Join-Path $stage "S9WoaInstaller.pri"))) { throw "The $arch publish has no S9WoaInstaller.pri." }

    $runtime = (Get-Content (Join-Path $stage "S9WoaInstaller.runtimeconfig.json") -Raw | ConvertFrom-Json).runtimeOptions.includedFrameworks |
      Where-Object name -eq "Microsoft.NETCore.App"
    $dotnetPack = Join-Path $nuget "microsoft.netcore.app.runtime.win-$arch\$($runtime.version)"
    $appSdk = [xml](Get-Content (Join-Path $repo "installer\src\S9Woa.Installer.App\S9Woa.Installer.App.csproj") -Raw)
    $appSdkVersion = $appSdk.SelectSingleNode("//PackageReference[@Include='Microsoft.WindowsAppSDK']").GetAttribute("Version")
    $appSdkPack = Join-Path $nuget "microsoft.windowsappsdk.runtime\$appSdkVersion"
    $licenses = New-Item -ItemType Directory -Force -Path (Join-Path $stage "licenses")
    Copy-Item (Join-Path $dotnetPack "LICENSE.TXT") (Join-Path $licenses "dotnet-LICENSE.txt")
    Copy-Item (Join-Path $dotnetPack "THIRD-PARTY-NOTICES.TXT") (Join-Path $licenses "dotnet-THIRD-PARTY-NOTICES.txt")
    Copy-Item (Join-Path $appSdkPack "license.txt") (Join-Path $licenses "WindowsAppSDK-license.txt")
    Copy-Item (Join-Path $appSdkPack "NOTICE.txt") (Join-Path $licenses "WindowsAppSDK-NOTICE.txt")
    Copy-Item (Join-Path $repo "LICENSE") (Join-Path $stage "LICENSE.txt")
    Copy-Item (Join-Path $repo "NOTICE") (Join-Path $stage "NOTICE.txt")
    Set-Content -LiteralPath (Join-Path $stage "README.txt") -Value $readme -Encoding utf8

    $zip = Join-Path $out "S9WoaInstaller-$Version-$arch.zip"
    [IO.Compression.ZipFile]::CreateFromDirectory((Split-Path $stage), $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
    Write-Output "$zip"
  }

  if (-not $Twrp) { return }

  # The recovery, built by the installer's own engine from the x64 publish. A child process loads
  # it, so the engine is unloaded again and the temporary folder can be removed.
  $fonts = Join-Path $repo "tools\twrp-winre\fonts"
  $recovery = Join-Path $out "star2lte-winre-recovery.img"
  & pwsh -NoProfile -NonInteractive -Command {
    param($core, $twrpImage, $fontsDir, $dest)
    $ErrorActionPreference = "Stop"
    Add-Type -Path $core
    $result = [S9Woa.Installer.Core.Twrp.WinReTwrpBuilder]::new().Build([IO.File]::ReadAllBytes($twrpImage),
      $fontsDir, [NullString]::Value, $null, $null)
    $stamp = [S9Woa.Installer.Core.Twrp.WinReTwrpBuilder]::ReadStamp($result.Image)
    if ($stamp.Fonts -ne "open" -or $stamp.Gears -ne "builtin") { throw "Unexpected recovery stamp: $stamp" }
    [IO.File]::WriteAllBytes($dest, $result.Image)
    "$dest ($($result.Sha256))"
  } -args (Join-Path $work "x64\S9WoaInstaller\S9Woa.Installer.Core.dll"), (Resolve-Path $Twrp).Path, $fonts, $recovery
  if ($LASTEXITCODE -ne 0) { throw "Building the recovery failed." }
} finally {
  if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
