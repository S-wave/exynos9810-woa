# Installer architecture

This document is for maintainers of the project. For how to *use* the installer,
see the top-level [README](../../README.md).

## Layout

```
installer/
  src/
    S9Woa.Installer.Core/   reusable engine, no UI dependency
      Device/               ADB client, device snapshot, eligibility checks, actions
      Host/                 host-PC preflight (admin, disk, tools)
      Image/                Windows image servicing and slim (tiny11-style) plans
      Processes/            process launching / output capture helpers
      Recovery/             UCR1 "restart to recovery" ticket protocol
      Stages/               StageCatalog + resumable InstallState
      Toolset/              first-run toolset: detect, acquire and remember tools
    S9Woa.Installer.App/    WinUI 3 front-end
      Wizard.cs             IWizardStep contract and step-rail model
      MainWindow            wizard shell: step rail, content surface, footer
      Pages/                Welcome, Setup, Host, Phone, Image, Install (steps); Tools, About (side pages)
  tests/
    S9Woa.Installer.Core.Tests/   xUnit tests for the engine
```

The engine (`S9Woa.Installer.Core`) has no UI dependency so it can be tested and
scripted. The WinUI app is a thin shell over it.

## Wizard shell

The app is a linear wizard. `MainWindow` owns navigation: a step rail on the
left (numbered circles that turn into checkmarks), the current page on a
layered content surface over Mica, and a fixed footer with *Step n of 6*, **Back**
and one accent primary button. Each step page implements `IWizardStep`:

- `CanAdvance` enables the primary button (for example, Setup only once
  `ToolsetManager.IsComplete`, Phone only for an eligible device).
- `NextLabel` names it (*Get started*, *Continue*, *Review*, *Install*).
- `OnAdvanceAsync` runs before leaving; returning `false` stays on the page
  (the Install step uses this to start the install in place).
- `CanGoBack` is `false` while a page is busy; the shell then locks Back, the
  rail and the side-page links.

The rail lets you jump back to any step already reached, never ahead of it.
Device tools and About open as side pages outside the flow. Debug builds accept
`--unlock-all-steps` to reach every step for UI work; Release builds ignore it.

## First-run toolset

`Toolset/ToolDefinitions.cs` lists everything the installer needs; the Setup
page renders one card per entry. `ToolsetManager` detects each item, acquires it,
and persists choices to `toolset.json` (release repository, local build folder,
program overrides). The install pages are locked until `ToolsetManager.IsComplete`
holds: every required item is `Ready`; optional ones (the Heimdall fallback) never block.

- **Programs** (adb, Heimdall, Zadig) — `ToolLocator` checks, in order: a
  user override, the app's `tools\` folder, winget portable installs (per-user and
  machine, packages and links), then `PATH`. `WingetClient` installs by exact id;
  success is judged by re-detection, not winget exit codes.
- **Samsung USB driver** — detected by its `dg_ssudbus` service. The user's
  downloaded installer runs only after `AuthenticodeVerifier` (`WinVerifyTrust`)
  confirms a trusted chain and a Samsung Electronics signer.
- **TWRP** — the user's download is accepted only if the name is a star2lte build
  and `BootImage` sees an `ANDROID!` header that fits RECOVERY. The installer then
  re-skins the official image into the WinRE-look recovery it actually flashes
  (see *WinRE-look recovery* below); the TWRP tool is Ready only once that build
  exists and matches the current builder.
- **UEFI and drivers** — `ReleaseClient` reads the latest release of the
  configured repository and keeps an asset only if it matches `SHA256SUMS` (a
  release without it is refused). When the release has a `firmware.json`, the
  UEFI download takes the whole firmware catalog: the catalog plus every image it
  lists, each checked against `SHA256SUMS` and then against the catalog's own
  SHA-256, staged beside the payload and swapped in only once all of them pass
  (a release with just `uefi.img` is still understood). `drivers.zip` is
  extracted with an archive-escape guard. A local build folder (`UseBuildFolder`)
  takes precedence; it imports its `firmware.json` catalog (or the newest valid
  `*uefi*.img`) and only *built* driver packages (an `.inf` next to a `.sys`).
  `tools/release/make-payload.ps1` produces the matching release assets.
- **Download-mode driver** — `DownloadModeDriver` reads the USB enumeration
  key for `VID_04E8&PID_685D`. It matters only for the optional Heimdall
  fallback, which needs Zadig to bind WinUSB; the built-in flasher does not.

Registry, signature checks, winget and HTTP are behind interfaces, so the tests
cover these paths without touching the PC or the network.

## Stages and honest gating

`Stages/StageCatalog.cs` is the single source of truth for the install flow.
Each `StageDefinition` has an `Availability`:

- `Ready` — implemented and validated on the reference device.
- `Guided` — the user performs the step by hand; the installer explains it and
  verifies the result.
- `Experimental` — automated and unit-tested, but not yet validated end to end
  on the reference device. The installer runs these only when the user opts in
  with *Run experimental steps* on the Install page (the switch only shows while
  some stage is experimental). Every automated stage has been validated end to
  end on the reference phone, so none is experimental today.
- `NotImplemented` — planned; the installer stops before it.

`InstallState` (persisted as JSON in the data folder, see below) records
per-stage status so an install can resume. When you finish automating a stage,
add coverage in the Core tests and move it from `Experimental` to `Ready` once it
is validated on hardware. Wiring for running a stage lives in
`Pages/InstallPage.xaml.cs` (`RunStageAsync`).

### Data folder and privacy

State, `toolset.json`, logs, the toolset payload and phone backups live in
`%LOCALAPPDATA%\S9WoaInstaller`, unless a folder named `data` sits next to
`S9WoaInstaller.exe`: then the installer is portable and keeps all of it there
(`InstallState.ResolveDirectory`). Put adb, Heimdall and Zadig in the app's
`tools\platform-tools`, `tools\heimdall` and `tools\zadig` folders and the whole
installer runs from one folder.

Logs and on-screen text go through `Redactor` (`AppServices.Redact`), so a log
can be attached to an issue and the installer can be screen-recorded as it is.
Every serial adb lists (and the one remembered in the state) is replaced by
`phone-` plus 8 hex digits of its SHA-256, the Windows user folder by
`%USERPROFILE%`, and this PC's own Windows version (the full version with UBR,
the build lab string, and the version line of DISM's banner) by `(hidden)`: the
PC may run a preview or internal build. The This PC check only says whether the
Windows version is supported. Backups go to `backups\phone-<hash>`, so no path shows the
serial either; a backup an earlier version made in `backups\<serial>` is still used.

## Deploy engine

`Core/Deploy` holds the device-facing engine, all behind injectable process
seams so it is unit-tested without hardware:

- `TwrpClient` — adb-over-TWRP primitives: list partitions by name, read size,
  `dd`, sha256, push/pull. Binary payloads always move as files staged in TWRP's RAM (`/tmp`),
  never as captured stdout, so nothing is corrupted by text decoding. It also
  breaks the ntfs-3g FUSE self-deadlock from the host (`EnsureResponsiveAsync` /
  `BreakNtfsDeadlockAsync`): when a shell hangs it enumerates `/proc` over the
  adb sync service (which keeps answering when the shell does not), finds the
  wedged `mount.ntfs` daemon, pins its OOM score and trips the OOM killer with
  sysrq `f` — the fallback for a stock TWRP without the baked-in watchdog.
- `PartitionMap` — the validated star2lte partition names.
- `BootRouteService` — makes the next restart a real Windows start. It clears the
  Android bootloader control block in MISC (`boot-recovery`), acknowledges a
  `RECOVERY_PENDING` watchdog record (`rwd1_ack.ko`), then reads the two
  **retained startup records** with `rwd1_evidence_reader.ko` — RWD1 (64 bytes at
  `0xFED13D80`) and P3/SMP1 (128 bytes at `0xFED13E80`) — and zeroes whichever is
  not all zero (`rwd1_clear_poc.ko`, token-gated, and `pram_smp_clear_poc.ko`),
  then reads both back. The previous start is over by definition, so anything left
  is stale. The records are judged on **every byte**: the firmware's P3 startup
  gate halts, with the watchdog disabled, on anything but an all-zero record (the
  phone then sits on the Samsung logo for good), and the record that hung the
  reference phone had a zero first word. Stock Android, Download mode, a power
  loss and a forced reset all leave such bytes; a left-over RWD1 record sends the
  next start back to TWRP. The first-boot stage runs this before every attempt,
  refuses to restart when the records do not read back clear, and saves what it
  found to `logs\startup-records-*.txt`.
- `Gpt` / `GptTypeService` — sets the SYSTEM partition's GPT type to the EFI
  system partition type (stock Android flashes it as basic data), patching both
  GPT copies with their CRCs, saving the originals to the backup folder and
  verifying the read-back. Together with FAT32 CACHE/SYSTEM ESPs (4096-byte
  sectors) and a BCD whose devices are GPT-qualified (CACHE for the boot manager,
  USERDATA for the loader), the partitions match the reference phone exactly.
  On 128 GB and 256 GB phones only `USERDATA` differs: `PartitionMap.CheckWindowsTarget`
  requires it to start at the 64 GB offset and hold at least the 64 GB-sized Windows
  volume, and the answer file's specialize pass runs `diskpart extend filesystem`
  so the volume fills the partition.
- `BackupService` — verified identity backup (efs and friends) to the PC, with a
  device/PC sha256 cross-check and a JSON manifest. Refuses if `efs` is missing.
- `TwrpFlasher` — `ITwrpFlasher`, `TwrpFlashService` (uses the first available
  flasher) and `HeimdallTwrpFlasher` (fallback; needs Zadig's WinUSB binding).
- `Odin/` — the built-in flasher. `OdinSession` speaks Samsung's Download-mode
  protocol (ODIN/LOKE handshake, begin session with protocol-version probe,
  PIT dump in 500-byte parts, file sequences of 128 KiB or 1 MiB parts, end
  session without reboot), implemented from the protocol as documented by the
  open-source Heimdall and Thor projects. `SerialOdinTransport` carries it over
  the COM port the Samsung USB driver creates for Download mode, so no driver is
  replaced; `DownloadModePort` finds that port (a present `VID_04E8&PID_685D`
  instance). `OdinTwrpFlasher` looks up RECOVERY and BOOT in the phone's PIT by
  name, writes TWRP to both in one session (as Samsung's firmware packages do) and
  restarts the phone, which then starts TWRP from BOOT by itself: Android never
  runs, so it cannot put its stock recovery back, and the UEFI stage replaces BOOT
  later. BOOT (55 MiB) is smaller than RECOVERY (65 MiB), so BOOT gets the image
  without the zero padding a full-partition image (such as a prebuilt WinRE-look
  recovery) carries past its sections; an image still too large for BOOT is only
  flashed to RECOVERY and the phone stays in Download mode. (The Android
  `boot-recovery` request in MISC is not an option: this
  phone's Download mode fails the session with -1, or stops answering, when MISC
  is written.)
  `Pit` parses the partition table. `FlashPartition` refuses a raw (non-sparse)
  image for `USERDATA`: the bootloader writes that partition through its
  filesystem path and rejects a raw image at session close ("Invalid Magic Code!
  0x0", eMMC write −1), so it must be an Android sparse image. A simulated
  bootloader covers it in tests.
- `TransferService` — writes prepared images through TWRP. For the raw Windows
  volume it reads the NTFS `$Bitmap` (`Image/NtfsAllocation`) and writes only the
  MiBs that hold used clusters (plus the first and last MiB, for the boot sector
  and its backup), which is about 10 GiB of a 53 GiB Core volume.
  `TransferPlanner` groups them into chunks of up to 128 MiB; each is staged in
  TWRP's RAM (`/tmp`) rather than on the SD card, and the next chunk crosses USB
  while the current one is written. With *Verify every write* on (the default),
  each chunk is read back and its SHA-256 compared with the source. Skipping
  free space is safe because NTFS never reads unallocated clusters: a sparse
  copy and a full copy of the same image give identical `chkdsk` results. If the
  image is not NTFS, everything is written, with all-zero chunks filled on the
  phone. Small images (UEFI) go to BOOT whole. The service refuses a destination
  smaller than the image and refuses to write a mounted target.
- `BootFilesService` — mounts the phone's FAT EFI system partition and copies the
  built ESP tree onto it (plus a `/cache` BCD copy the firmware also consults).
- `BootConfiguration` — retargets the freshly built BCD so the boot manager and
  loader find Windows by locating `\Windows`, rather than by a partition GUID the
  raw USERDATA volume does not have. Uses only public `bcdedit` features.

### WinRE-look recovery (`Core/Twrp`)

The installer does not flash the raw TWRP the user downloads; it re-skins that
image, on the user's own PC, into a Windows-Recovery-Environment look and flashes
that. `WinReTwrpBuilder` is the whole build and is deterministic (same base image,
fonts and gear GIF → same bytes):

- it verifies the base is exactly the official `twrp-3.7.0_9-0-star2lte.img` by
  SHA-256 (and accepts a zero-padded RECOVERY dump of it);
- `AndroidBootImage` / `CpioArchive` parse and re-serialise the Samsung boot
  image and its newc ramdisk byte-for-byte (a port of the reference
  `tools/twrp-winre/bootimg.py`), and `Lzma/LzmaAlone` (the vendored public-domain
  7-Zip LZMA SDK) decompresses and recompresses the ramdisk in the exact "LZMA
  alone" framing the kernel expects;
- `PowerOffRoutePatch` applies a four-byte, hash-and-pattern-gated patch to the
  kernel so USB-connected "Turn off" powers the phone down through Samsung's hook;
- `WinReTheme` applies the committed `tools/twrp-winre/theme/reskin.xml` to the
  stock `ui.xml`/`portrait.xml` (palette, Segoe fonts, a page template with a back
  arrow and no blue header or logo, gears instead of the log on operation pages,
  dark keyboards, and the renames that hand `lock`, `singleaction_page`,
  `action_page` and `action_complete` to `winre.xml`), every one of the 57 stock
  bitmaps is replaced by original art (`assets/stock`), and the WinRE pages,
  `/sbin` scripts, icons, gears and the GPL kernel modules are embedded from
  `tools/twrp-winre`; the Segoe faces are copied from the builder's own
  `%WINDIR%\Fonts` (never redistributed). Given a folder that holds
  `winre-light/semilight/regular.ttf` instead (`WinReTwrpBuilder.ResolveFonts`), it
  uses those plus their OFL text: `tools/twrp-winre/fonts` is such a set, compiled
  from Selawik, and is what the recovery published in the releases is built with
  (the build stamp then records `fonts=open`);
- it bakes in four GPL kernel modules (`rwd1_ack`, `rwd1_evidence_reader`,
  `rwd1_clear_poc`, `pram_smp_clear_poc`) under `/sbin/s9woa`, so the recovery and
  its Troubleshoot actions can read and clear the retained startup records on the
  phone. It also ships `/sbin/rebootsystem.sh`, the hook TWRP runs before every
  restart into the system (the WinRE *Continue* tile, the stock Reboot menu, ORS):
  it clears the MISC request and the startup records, so a phone that landed in
  recovery after a power loss or a failed start does not stop at the Samsung logo
  on the way back to Windows. And it registers `winre-ntfs-watchdog.sh` as an **init service** (in
  `init.recovery.service.rc`). The watchdog has to run from init, not a TWRP boot
  script, because the ntfs-3g FUSE self-deadlock strikes while the recovery binary
  is still mounting `/data` — before any boot script runs and with adb/MTP still
  down; started at init it is already watching and breaks the deadlock (OOM-kills
  the wedged `mount.ntfs`) with no host attached;
- if the user supplied their own copy of Microsoft's UpdateOS gear animation,
  `GifDecoder` / `GearFrames` render its frames white-on-black into the image
  (with the animation speed taken from the GIF's frame delays) instead of the
  procedural gears; the GIF and its frames never leave the image being built;
- it asserts the output fits RECOVERY and that the device-tree, second stage and
  kernel (bar the patch) are unchanged.

`ToolsetManager` builds this when the user provides the official TWRP (chosen on the
Setup page, found in a build folder, or already chosen and rebuilt by *Set up
automatically* after an installer update). A `*GearAnimation*.gif` in the build
folder (for example `twrp\UpdateOS-GearAnimation.gif`) is copied into the payload
and triggers a rebuild. `Detect` reports the TWRP tool Ready only when the built
WinRE image exists and its recorded builder version, base hash and gears (built-in,
or the GIF's hash) still match, and its detail line says which gears it carries;
a prebuilt WinRE image the user supplies directly is accepted as-is. When it is a
build of this installer (its `twres/winre-build.txt` stamp names the builder),
the detail says whether that is the current builder; a different prebuilt in the
build folder replaces the previous prebuilt (never a build from the official TWRP).
The flashers use the built image.

The host side of the first boot pushes the same four modules from the toolset's
`twrp-modules` folder (`EnsureTwrpModules`): a build folder may supply some of them,
and every one it does not is filled in from the copies embedded in the installer,
so the startup-record clearers are always there even when the phone runs a
prebuilt or older recovery.

While the installer writes the phone, `TransferService`, `BootFilesService` and the
UEFI step keep a small `/tmp/s9woa/status` file (`WinReStatus`: phase, label,
percent, a detail line) current. `TwrpClient.QueueInstallStatus` appends the write
to the next shell command the installer runs anyway (each chunk's `dd`), keeping
that command's exit code, so the percentage costs no extra round trip. The
recovery's `winre-statuswatch.sh` raises the "Installing Windows" screen with one
`twrp changepage=` (the only ORS command TWRP serves without flashing its
`singleaction_page`), copies the values into Android properties with `setprop`
(the screen reads `%property.s9woa.*%` and binds its progress bar to
`property.s9woa.pct`, which TWRP re-reads on every draw), and clears it with one
more `changepage` when the file goes away. The installer also deletes a stale
`TWRP/theme/ui.zip` over adb so it cannot override the baked theme, and never uses
`twrp`/ORS on any hot path.

`Image/VhdxImageBuilder` runs the whole host build: it scripts diskpart to create
an ESP + MSR + NTFS layout in a VHDX, applies the edition, injects drivers, runs
the slim profile, writes the OOBE answer file (`UnattendXml`), runs `bcdboot`,
retargets the BCD, then exports the NTFS volume to `work\out\windows.img`
(`RawImageExporter` over a `VolumeDiskSource`) and copies the ESP to
`work\out\esp`. Those two outputs are exactly what the transfer stage writes to
the phone.

- `Image/UnattendXml` — a clean-room generator of the standard answer file.
  `oobeSystem`: a local administrator account, skipped EULA/privacy/MSA/wireless
  screens, and locale/time-zone/computer-name. This is what makes OOBE finish to
  the desktop without user input. `specialize`: display scaling
  (`UnattendOptions.Dpi`, default 275 DPI ≈ 286%; the phone's panel reports no
  physical size, so Windows would otherwise start at 100%). The unattend
  `Display/DPI` setting is no longer honoured, so `RunSynchronous` commands write
  `LogPixels` and `Win8DpiScaling` (what Settings' custom scaling writes) into the
  default user profile, which the OOBE account is copied from, and into
  `.DEFAULT` for the sign-in screen.

The validated device targets are in `Deploy/PartitionMap`: Windows to
`USERDATA`, UEFI to `BOOT`, TWRP to `RECOVERY`, and boot files to the FAT EFI
system partition.

## Image profiles

`Image/SlimPlan` turns a profile into ordered operations that `ImageServicer`
applies offline (all DISM work runs with the image's registry hives unloaded):

- **Stock** — drivers only.
- **Lite** — removes consumer inbox apps and applies ads/telemetry defaults;
  Windows Update keeps working.
- **Core** — follows tiny11Coremaker.ps1: Lite plus CBS package removal
  (Defender, IE, Media Player, language features, ...), Edge/WebView2/OneDrive and
  WinRE deletion, a component store (WinSxS) cut down to the servicing stack and
  runtime assemblies, tiny11's registry set (Windows Update and Defender off,
  Copilot/Teams/Outlook blocked) and a final `/ResetBase`. All of it works from an
  x64 PC on an ARM64 image.

## Release pipeline (maintainers only)

This public repository is **generated**, not edited in place at the source of
truth. The flow is:

1. Product source lives in a private research repository.
2. `tools/release/import_sources.py` copies only the paths named in
   `release/import-manifest.json` into this tree. Nothing outside the manifest
   is ever published (`global_exclude` drops logs, receipts, provenance, etc.).
   The installer, `tools/twrp-winre`, `tools/release` and
   `drivers/Exynos9810Hsi2c` (the sources its released binary was built from,
   apart from line endings) are not imported: they are maintained in this repository.
3. During import, two transforms keep public edits reproducible:
   - **`resolve_macros`** — a unifdef-like resolver that removes dead
     `#if`/`#ifdef` branches for named flags (used to drop diagnostic/hook code
     from the firmware and to force the portable-kernel contract in the UFS
     driver).
   - **per-entry patches** — small diffs applied after the copy, recorded with
     `--make-patches` and reapplied on every import. An overwrite guard refuses
     to import over a public file that was hand-edited without recording a patch;
     `--force` discards such edits.
4. `tools/release/scan.py` is the leak gate. It rejects vendor/kernel internals,
   build hashes, workspace paths, personal emails and stray binaries.
   `release/scan-allow.txt` lists the few intentional exceptions (the scanner's
   own tests, upstream GPL author emails, the WinPE `X:` system path, the app
   icon).

### Private, not-in-git files (back these up)

Two things are needed to reproduce a release but are intentionally **not**
committed (see `.gitignore`):

- **`.release-patches/`** — the per-entry patch files. Their *removed* lines can
  contain scrubbed private text, so they never go public. Regenerate with
  `python tools\release\import_sources.py --source <research-repo> --make-patches`.
- **`.release-denylist.txt`** — additional local scanner patterns.

If you lose these you can still ship, but you lose the automated, reproducible
scrub — you would have to redo the public edits by hand. **Back them up
privately** alongside the research repository.

### Typical maintainer loop

```powershell
# edit an imported file in this public repo, then:
python tools\release\import_sources.py --source <research-repo> --make-patches
python tools\release\import_sources.py --source <research-repo>   # verify reapply
python tools\release\scan.py                                      # must be clean
python -m pytest -q tools\release\tests
```
