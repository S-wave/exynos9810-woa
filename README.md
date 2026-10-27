# Windows on the Galaxy S9+ (Exynos 9810)

Run Windows 11 (ARM64) on the Samsung Galaxy S9+ **SM-G965F** (Exynos 9810,
"star2lte"). This repository holds the firmware, drivers and a graphical
installer that takes a phone from a fully stock Android state to installed
Windows.

> **One validated device.** Everything here has only been proven on
> **SM-G965F** with **Windows 11 22H2/23H2 ARM64** (the builds listed in the
> firmware catalog: 22621/22631.2428 and 22621.7582/22631.7584). The installer
> knows which steps are automated and which are not: unfinished stages are shown
> as *coming soon* and the installer stops before them rather than guessing. Do
> not point it at another model or firmware.
>
> **Storage sizes.** The reference phone is the 64 GB model. The 128 GB and
> 256 GB models are supported but not yet validated on hardware: they share the
> 64 GB partition layout except for a larger `USERDATA`, so the installer writes
> the same Windows image to the start of it and Windows grows the volume to fill
> the whole partition on first boot. The UFS driver takes the end of the disk
> (backup GPT and write fence) from the size the phone reports, accepting only
> the 64 GB unit exactly or a size inside the 128 GB or 256 GB band.

## 🎬 Watch it install

[![Windows 11 on your Galaxy S9+. The installer is out.](https://img.youtube.com/vi/i3sSkU9Lck8/maxresdefault.jpg)](https://youtu.be/i3sSkU9Lck8)

The whole installation, from a stock Galaxy S9+ to the Windows desktop.

## What's in the box

| Path | Component | License |
|------|-----------|---------|
| `firmware/` | EDK2-based UEFI for star2lte (Platform + Silicon packages, boot shim, build/pack scripts), with code and designs from Mu-Silicium, edk2-msm / edk2-exynos and WOA-Project's Lumia950XLPkg / mu_andromeda_platforms (see [Credits](#credits)) | BSD-2-Clause-Patent; boot shim BSD-2-Clause (edk2-msm); CNTFRQ_EL0 patch MIT (Mu-Silicium) |
| `drivers/Exynos9810Ufs/` | UFS storage driver (`drivers/include/` holds its shared record definitions) | BSD-2-Clause-Patent |
| `drivers/Exynos9810Hsi2c/` | HSI2C bus controller (SpbCx) the touchscreen sits on | BSD-2-Clause-Patent AND MS-PL (framework from Microsoft's SkeletonI2C sample) |
| `drivers/S6SY761Touch/` | Touchscreen driver, derived from [TheMorc's S6SY761Touch](https://github.com/TheMorc/S6SY761Touch) (which ports the Linux `sec_ts`/`s6sy761` driver) | GPL-2.0-only |
| `installer/` | WinUI 3 installer (`S9Woa.Installer.App`) over a reusable C# engine (`S9Woa.Installer.Core`) | BSD-2-Clause-Patent |
| `tools/twrp-winre/` | The WinRE look the installer gives TWRP on your PC (theme, scripts, original artwork, GPL kernel modules; `fonts/` holds the open fonts of the published recovery) | BSD-2-Clause-Patent; modules GPL-2.0; fonts SIL OFL 1.1 |
| `tools/release/` | Release importer, leak scanner and release payload script | BSD-2-Clause-Patent |

The [releases](../../releases) carry everything else you need, all listed in
`SHA256SUMS`:

| Asset | What it is |
|-------|------------|
| `S9WoaInstaller-<version>-x64.zip` / `-arm64.zip` | The installer, ready to run (self-contained: no .NET or Windows App SDK install needed). Unzip and run `S9WoaInstaller.exe`. |
| `firmware.json` + `star2lte-uefi-<build>.img` | The UEFI images, one per supported Windows build. The installer downloads and verifies them for you. |
| `drivers.zip` | The UFS, HSI2C and touch driver packages (test-signed). Also downloaded by the installer. |
| `star2lte-winre-recovery.img` | The WinRE-look TWRP, prebuilt from the official TWRP with open fonts. Optional: choose it on the Set up page instead of the official TWRP download. |

Windows itself is never redistributed: bring your own ARM64 media.

## The installer

The installer is a Windows desktop app. It walks the end-to-end flow and, for
each stage, either performs the work or guides you through the manual part and
verifies the result.

| Stage | State |
|-------|-------|
| Check this PC (admin, disk, adb, USB driver) | ✅ Ready |
| Identify the phone | ✅ Ready |
| Get Windows media | ✅ Ready |
| Build the Windows image | ✅ Ready (VHDX: apply + drivers + slim + OOBE unattend + bcdboot, exported to a raw image) |
| Unlock the bootloader | 🧑 Guided (you do it; the installer verifies) |
| Install TWRP | ✅ Ready (flashes RECOVERY and BOOT by name over Samsung's Download-mode protocol, through the Samsung USB driver — no Zadig — and the phone starts TWRP by itself) |
| Back up EFS / modem / partition table | ✅ Ready (verified backup to your PC) |
| Prepare partitions | ✅ Ready (verifies the layout by name) |
| Copy Windows to the phone | ✅ Ready (only the used NTFS clusters → USERDATA, staged in TWRP's RAM; boot files → EFI; optional read-back verification) |
| Install UEFI | ✅ Ready (the UEFI build for your Windows build → BOOT) |
| First boot | ✅ Ready (clears the startup records, reboots into Windows; OOBE finishes to the desktop unattended) |

The flow is end to end: from plugging the phone in to landing on the Windows
desktop. All the slow PC-side work (choosing media and building the image) runs
**before** the phone is touched, so the on-device steps — unlock, TWRP, backup,
and the verified writes — run back to back. The **Build the Windows image** step
produces a bootable Windows volume (with your drivers, the optional slim profile
and an OOBE answer file that creates your local account, skips the setup
screens and sets the display scaling to 275 DPI for the phone's screen); **Copy Windows** writes it plus the boot files to the phone. After
**First boot**, Windows setup completes on its own and signs you in.

Every stage has been run end to end on the reference phone, from stock Android
to the Windows desktop. The installer still backs up the phone before it writes
anything that cannot be recovered, and keeps those backups on your PC: keep them.

Quality-of-life tools (e.g. "Restart to TWRP" from inside Windows) live on the
**Tools** page.

### First-run setup

The first time it runs, the installer opens **Set up**. The install pages stay
locked until everything below is ready, and the choices are remembered in
`%LOCALAPPDATA%\S9WoaInstaller\toolset.json` (or in the `data` folder next to
the app, when there is one: portable mode).

| Item | How Setup provides it |
|------|------------------------|
| Android platform tools (adb) | winget `Google.PlatformTools` |
| Heimdall (optional) | winget `BenjaminDobell.Heimdall`; only a fallback, used if the Download-mode interface has been switched to WinUSB |
| Zadig (optional) | winget `akeo.ie.Zadig`; only for the Heimdall fallback |
| Samsung USB driver | You download Samsung's installer; Setup runs it only if it is validly signed by Samsung Electronics |
| TWRP for star2lte | Setup opens the official TWRP page; you choose the downloaded `twrp-3.7.0_9-0-star2lte.img` (checked for the model name, the boot-image header and the RECOVERY size). The installer re-skins it on your PC into a WinRE-look recovery and flashes that, with Segoe UI from your Windows. Or choose the release's prebuilt `star2lte-winre-recovery.img` (the same recovery with open fonts), which is used as-is. If your build folder holds your own copy of Microsoft's `UpdateOS-GearAnimation.gif` (e.g. in `twrp\`), the recovery is built with those gears; otherwise it uses its own drawn gears |
| UEFI image | Latest project release (`firmware.json` and the UEFI image for each Windows build it lists), or your local build folder |
| Phone drivers | Latest project release (`drivers.zip`), or your local build folder |
| Download-mode USB driver for Heimdall (optional) | Not needed: the installer flashes TWRP through the Samsung USB driver. Only for the Heimdall fallback, via Zadig |

**Set up automatically** installs everything that needs no decision from you.
Release downloads are kept only if they match the release's `SHA256SUMS` (and,
for the UEFI images, the SHA-256 recorded in `firmware.json` as well); a
release without that file is refused. Any program can be replaced with your own
copy (*Use a different file…*), and a local build folder takes precedence over
releases.

The raw Windows image and boot files are produced automatically by the **Build
the Windows image** step (under the installer's work folder); you don't supply
them.

### Publishing a release

`tools\release\make-release-extras.ps1` builds the installer zips (self-contained
publish plus the bundled runtimes' license terms) and the prebuilt recovery (the
official TWRP re-skinned with the open fonts in `tools/twrp-winre/fonts` and the
built-in gears). `tools\release\make-payload.ps1` then assembles every asset:
`firmware.json` with its UEFI images (each checked against the hash the catalog
records), `drivers.zip`, those extras, and `SHA256SUMS`:

```powershell
.\tools\release\make-release-extras.ps1 -Twrp <official twrp-3.7.0_9-0-star2lte.img> -Version 0.1.0 -OutDir out\extras
.\tools\release\make-payload.ps1 -FirmwareCatalog <folder with firmware.json> `
    -Drivers <Exynos9810Ufs package>, <Exynos9810Hsi2c package>, <S6SY761Touch package> `
    -Assets (Get-ChildItem out\extras).FullName -OutDir out\release
gh release create v0.1.0 (Get-ChildItem out\release).FullName
```

Never attach Windows media or a built Windows image, and never a recovery the
installer built on a PC: that one carries Segoe UI from the builder's Windows (and
maybe Microsoft's UpdateOS animation). `make-payload.ps1` refuses a recovery whose
build stamp is not "open fonts, built-in gears".

### Build the installer

Requirements: Windows 11, the .NET 8 SDK, and the Windows App SDK workload
(Visual Studio 2022 "WinUI" workload or the standalone components).

```powershell
cd installer
dotnet build S9Woa.Installer.sln -c Release -p:Platform=x64
dotnet test tests\S9Woa.Installer.Core.Tests\S9Woa.Installer.Core.Tests.csproj -p:Platform=x64
```

`-p:Platform=ARM64` builds the ARM64 flavour. The app is unpackaged
(`WindowsPackageType=None`) but keeps the MSIX tooling on, which is what puts its
compiled XAML resources (`S9WoaInstaller.pri`) into a `dotnet publish`.

### Build the firmware

Requires EDK2 BaseTools, the CLANGPDB toolchain (LLVM), Python and GnuWin32
make. See `firmware/tools/build-uefi.ps1` and `firmware/tools/pack-uefi.ps1`.
The UEFI images in the releases are the builds validated on the phone, one per
Windows build (see `firmware.json`); they come from the research lineage of this
tree, and reproducing them byte for byte from `firmware/` is not scripted yet.

### Build the drivers

- **UFS storage**: `drivers/Exynos9810Ufs/build-ufs.ps1` (fixed cl/link command
  lines, reproducible). The released driver is
  `build-ufs.ps1 -SourceDir drivers\Exynos9810Ufs -OutDir <new folder> -WideUncached -DmaWindow`,
  which reproduces it byte for byte (unsigned SHA-256
  `1346ce2c2e1cd08624ecb4b09a8418c071e0a5ec9b2f976ebfa34bd3702dc029` for this fork's
  0.2.0 driver with 128/256 GB support; upstream's 64 GB-only 0.1.0 driver was
  `a80b222939e7ec81522221740de5346b9799ff68492a875d996fdf947138eaa7`). Package it
  with `Exynos9810Ufs.inf`, `Inf2Cat /os:10_VB_ARM64` and a test signature on the
  `.sys` and `.cat`.
- **HSI2C**: `msbuild drivers\Exynos9810Hsi2c\Exynos9810Hsi2c.vcxproj /p:Configuration=Release /p:Platform=ARM64`
  (needs the WDK's Visual Studio integration); see its README.
- **Touch**: `drivers/S6SY761Touch/README.md`.

All three are test-signed, so Windows runs them with test signing on, which the
installer enables in the image it builds. In this fork's releases the UFS driver
is signed with the fork's own test certificate (`CN=S9 WoA Test Signing (S-wave
fork)`); HSI2C and touch are upstream 0.1.0's builds, signed with upstream's.

## Safety

Installing Windows **erases Android** and touches low-level partitions. The
installer backs up EFS, modem calibration and the partition table before any
destructive step. Keep those backups: they are how you return to stock. There is
no warranty — see the license.

## ❤️ Support the Project

If this project has helped you, please consider showing your support! A small
donation helps me dedicate more time to projects like this.

**[Patreon](https://patreon.com/ntdev) | [PayPal](https://paypal.me/ntdev2) | [Ko-fi](https://ko-fi.com/ntdev)**

## Credits

This project reuses code and follows designs from other projects, and would not
exist without them:

- **[Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)**
  (Robotix22, N1kroks, halal-beef and contributors): the CNTFRQ_EL0 shellcode
  Windows needs to boot on this SoC, and the winload patch targets around it
  (MIT); the DXE RAM-manager approach; and its Exynos 9810 Windows-boot work as
  the reference for this firmware.
- **[Renegade Project / edk2-msm](https://github.com/edk2-porting/edk2-msm)** and
  **[edk2-exynos](https://github.com/sonic011gamer/edk2-exynos)**: the boot shim
  every UEFI image starts with (BSD-2-Clause), and the firmware's load layout.
- **[WOA-Project](https://github.com/WOA-Project)** (Ben (Bingxing) Wang /
  imbushuo, Gustave Monce / gus33000, the DuoWoA authors):
  [Lumia950XLPkg](https://github.com/WOA-Project/Lumia950XLPkg) and
  [mu_andromeda_platforms](https://github.com/WOA-Project/mu_andromeda_platforms),
  whose structure the firmware's DSC/FDF follow, and the kernel-patching
  approach that started with Samuel Tulach's
  [rainbow](https://github.com/SamuelTulach/rainbow).
- **[TheMorc (Richard Gráčik)](https://github.com/TheMorc/S6SY761Touch)**: the
  S6SY761 touchscreen driver ours is derived from, itself built on Microsoft's
  vhidmini2 sample and Gustave Monce's nt36xxx_win, porting Andi Shyti's Linux
  driver.
- Microsoft's Windows-driver-samples (SkeletonI2C, vhidmini2), TianoCore EDK2,
  the mainline Linux and postmarketOS Exynos 9810 communities, TWRP, and more.

[NOTICE](NOTICE) has the full list, what came from where, and the license texts.

## License

Firmware, drivers (except the touch driver), the installer and the release tools
are under the **BSD-2-Clause-Patent** license (see [LICENSE](LICENSE)). The
touchscreen driver under `drivers/S6SY761Touch/` is **GPL-2.0-only**; its own
`LICENSE` file governs that subtree. The HSI2C driver under
`drivers/Exynos9810Hsi2c/` is BSD-2-Clause-Patent **AND MS-PL** (its framework
scaffolding comes from Microsoft's SkeletonI2C sample; see its README and
`MS-PL.txt`).
