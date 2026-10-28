# Hardware bring-up: overnight report (28 Oct 2026)

Goal: USB, Wi-Fi, Bluetooth, GPU acceleration, sound and all eight CPU cores on the
256 GB Galaxy S9+ (SM-G965F, board rev26). Nothing in this report has run on the phone
yet. The phone was only read from (TWRP, read-only), never written.

## Where each one stands

| Feature | State tonight | What is needed next | Realistic size |
|---|---|---|---|
| **USB host** | Firmware code written and compile-checked (`UsbHost.c`, DSDT `USB0`), off by default | Toolchain on this PC, then one build and boot test (below) | Days, if the first test is close |
| **CPU (8 cores)** | Root cause narrowed, not solved | On-phone experiments; each failed one may need a force restart | Unknown: the previous developer spent 36+ boots |
| **Bluetooth** | Hardware fully mapped, sequence known | I2C expander write for power, Windows UART + HCI transport driver | Weeks |
| **Wi-Fi** | Hardware fully mapped, firmware files copied locally | PCIe bring-up in UEFI, then a Windows port of Broadcom's driver | Months |
| **Sound** | Hardware mapped | ABOX DSP + CS47L93 codec + MAX98512 amp drivers | Months |
| **GPU acceleration** | Mapped (Mali-G72 MP18) | A Windows display driver for Mali; no source to port from | Not realistic |

**Shortcut once USB host works:** Windows' built-in drivers handle USB keyboards and
mice, USB Ethernet, many USB Wi-Fi dongles, USB Bluetooth dongles and USB audio
adapters. That covers networking, Bluetooth and sound without any native driver.

## Blocked: the UEFI can't be built on this PC yet

Every firmware change (USB host, CPU, Wi-Fi's PCIe) needs the UEFI built from source.
That needs LLVM, GNU make and the ACPI compiler `iasl`. I fetched EDK2
`edk2-stable202608` into `C:\w\fw\edk2` (it contains every module the platform uses),
but installing LLVM through winget needs a UAC prompt, and running prebuilt `iasl`
and `make` downloads was blocked by the session's safety rules while you were away.
To unblock, install them yourself:

```
winget install LLVM.LLVM
winget install GnuWin32.Make
```

`iasl`: Project Mu's `edk2-acpica-iasl` 20241212 package (what EDK2's CI uses), or
ACPICA's Windows binaries from intel.com. Put `iasl.exe` on PATH. Then link EDK2 into
place: `mklink /J C:\w\e9810\firmware\edk2 C:\w\fw\edk2`.

## USB host: what was built

`firmware/Platform/Star2LtePkg/Library/PlatformBootManagerLib/UsbHost.c`, built in with
`tools\build-uefi.ps1 -UsbHost`:

1. **Checks** (never changes) the USB clocks the boot loader leaves running, against the
   values the existing USB debug transport was validated with.
2. **VBUS:** switches on the MAX77705 charger's 5 V boost over I2C (hsi2c@14360000,
   charger 0x69), exactly as Samsung's `max77705_set_otg()` does. It does this **only when
   nothing is supplying power to the port**, so it never pushes 5 V into a charger or PC.
   It only uses the I2C bus if the boot loader left its timing programmed.
3. **PHY:** the validated USB debug bring-up, then Samsung's host-mode settings
   (`phy_exynos_usb_v3p1_config_host_mode`: D+/D- pull-downs on, forced VBUS-valid off).
4. **Controller:** DWC3 switched to host mode; Windows' inbox xHCI driver takes it from
   the new ACPI device `USB0` (`PNP0D10`, GSIV 276).

It logs each step (`=UHCLK`, `=UHID`, `=UHINTOK`, `=UHVBUSON`, `=UHGCTL`, `=UHUTMI`,
`=UHXHCI`) to the firmware record ring.

## Test plan for the morning

Needs: the toolchain above, a USB-C OTG adapter (or USB-C hub), a USB keyboard or flash
drive, and the phone **not** on a charger during the USB test.

1. **Baseline from source.** `tools\build-uefi.ps1` (no options). Flash it to BOOT from
   TWRP and boot Windows. The README warns the shipped images came from a research
   tree, so this proves the source build boots *before* anything new is tested.
   If it doesn't boot: hold Volume Down + Power, then go into TWRP and reflash the
   release image (`star2lte-uefi-22621.2428.img`) to BOOT.
2. **USB host.** `tools\build-uefi.ps1 -UsbHost`, flash, boot with the adapter and a USB
   device plugged in and no charger. Expected: Device Manager shows a
   *USB xHCI Compliant Host Controller* and the device works.
3. **If it fails:** boot to TWRP and read
   `adb shell cat /sys/fs/pstore/pmsg-ramoops-0 | grep "=UH"`. The `=UH` lines show
   which step stopped (clocks, I2C, VBUS or the controller). If Windows got far enough,
   its storage driver overwrites the ring; then try once with Windows' boot
   interrupted at the Samsung logo.

Safe failure modes: if USB host is wrong, Windows simply has no working USB. Storage,
display and touch are untouched. The VBUS step refuses to run with power present.

## CPU: what was learned

- Under TWRP's Linux **all eight cores are online** (`online=0-7`; CPUs 4-7 are Samsung
  Mongoose M3, implementer 0x53 part 0x002), through the same PSCI `CPU_ON` call.
- The shipped UEFI's CPU table enables only CPU 0. Its parking code starts the three
  other A55s, and deliberately not the big cores.
- The firmware's own notes (boot "v33") show FlexPMU reporting the big core **powered,
  released and online** after `CPU_ON`, yet it never runs the entry code, and a second
  `CPU_ON` hangs the phone.
- Ruled out tonight: Samsung's uH hypervisor (TWRP's kernel runs with `CONFIG_UH` off
  and still starts all eight), and the FlexPMU per-core sequences (Linux runs them on
  the core *after* it is already up).
- Second hurdle once they start: the Mongoose cores lack ARMv8.1 "LSE" atomics (TWRP's
  kernel is built with `CONFIG_ARM64_LSE_ATOMICS` off). Windows must not use LSE, or
  programs crash when moved to a big core.

Next experiments (each risks one force restart): start the big core with Linux's exact
calling convention and an entry point in the low kernel window (Linux uses
`__pa(secondary_entry)` around 0x8008xxxx, where the UEFI used a page just below 4 GB);
and compare the CPU/cluster register state Linux leaves before `smp_init`.

## Files

- `docs/bringup/hardware-map.md`: every address, interrupt, GPIO and firmware file
  for each feature, from the phone's live device tree and LineageOS 17.1's kernel.
- Firmware files copied from your phone's VENDOR partition (Broadcom Wi-Fi/BT, Cirrus
  codec, Maxim amp tuning) are in `C:\w\los\phone\vendor-fw`. They are proprietary and
  are **not** in the repo or any release.
- LineageOS 17.1 kernel source (`android_kernel_samsung_universal9810`, branch
  `lineage-17.1`) is unpacked in `C:\w\los\ksrc`.
