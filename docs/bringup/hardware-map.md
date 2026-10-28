# Galaxy S9+ (SM-G965F, Star2 rev26) hardware map for Windows bring-up

Sources: the phone's live device tree (`/sys/firmware/fdt` under TWRP, Linux 4.9.118),
LineageOS 17.1 star2lte `boot.img` (Linux 4.9.218, same board rev26), TWRP's running kernel,
and the phone's VENDOR partition (LineageOS-based, `CP2A.260605.016`).
Addresses are physical; GIC SPI numbers are the DT values (add 32 for the GIC interrupt ID).

## CPU
| | Cores | MPIDR | ID |
|---|---|---|---|
| LITTLE | 4x Cortex-A55 | 0x000-0x003 | implementer 0x41, part 0xd05 |
| big | 4x Samsung Mongoose M3 | 0x100-0x103 | implementer 0x53, part 0x002 |

- PSCI 0.1, `method = smc`: cpu_on 0xC4000003, cpu_off 0x84000002, cpu_suspend 0xC4000001.
- TWRP's kernel is built with `CONFIG_UH` off and `CONFIG_ARM64_LSE_ATOMICS` off: Samsung's uH
  hypervisor is not what blocks the big cores, and the Mongoose cores lack ARMv8.1 LSE atomics, so
  Windows must not use LSE once the big cores run (a second hurdle after bring-up).
- In Linux the FlexPMU per-core sequences (`flexpmu_cal_cpu_exynos9810.h`: GRP2_INTR_BID_ENABLE
  0x14070200, PMU_EVENT_INTERRUPT_ENABLE_GRP2 0x14067F08, big cores bits 0-3) run as a CPU-hotplug
  AP step ON the incoming core, after it is already running; they are not a prerequisite for CPU_ON.
- Under TWRP's Linux all eight cores are online (`online=0-7`), so EL3 CAN start the big cluster;
  the shipped UEFI only starts the A55s (its MADT enables CPU0 only and its parking code starts
  0x001-0x003). Big-core CPU_ON from UEFI returns SUCCESS but the core never arrives, and a second
  CPU_ON hangs the SoC (firmware notes, boot-v20/v26/v29).
- ACPM / FlexPMU: `acpm`, `acpm_ipc`, `exynos_flexpmu_dbg` (data 0x0204F800, 0x400).

## USB
- Synopsys DWC3 (`samsung,exynos-dwusb`) at **0x10C00000** (64 KB), SPI 0xF4 (244), `dr_mode = otg`,
  super-speed. DWC3 in host mode is an xHCI controller (Windows: inbox usbxhci via ACPI PNP0D10).
- PHY `samsung,exynos-usbdrd-phy` at 0x11100000 (+0x110A0000, +0x110B0000), combo USB2/USB3,
  PMU offsets 0x72C / 0x704, phy_version 0x300, sub 0x400.

## Wi-Fi (BCM4361 B2, FullMAC, PCIe)
- PCIe RC 0 `samsung,exynos-pcie` @ elbi 0x116A0000, phy 0x116D0000, sysreg 0x11411044,
  dbi 0x11700000, pcs 0x116C0000, **config 0x127FE000**, ia 0x11680000; SPI 0xFD (253);
  MEM window 0x11800000 + 0xFF0000; Gen2 x1; behind sysmmu 0x11420000; cache-coherent.
- Live enumeration under TWRP: 00:00.0 = 144d:ecec (root port), **01:00.0 = 14e4:441f** (BCM4361).
- GPIOs: WLAN_EN gpg0-5 (PERIC0 GPIO 0x10430000), host wake gpa0-7 (ALIVE GPIO 0x14050000),
  PERST gpf2-2 and CLKREQ gpf2-0 (FSYS1 GPIO 0x11430000), PCIe wake gpa3-2.
- Linux power-on (`dhd_custom_exynos.c`): WLAN_EN high, wait `WIFI_TURNON_DELAY`, then
  `exynos_pcie_pm_resume(0)` = PHY bring-up (`pci-exynos9810_cal.c`, 419 lines) and link training
  (`pci-exynos.c`, ~3000 lines). The Wi-Fi driver itself (`bcmdhd_100_15`) is ~211k lines.
- Firmware (VENDOR /firmware): `bcmdhd_sta.bin_b2`, `bcmdhd_clm.blob`, NVRAM per module:
  `nvram.txt_murata_r041_b2`, `nvram.txt_r02a_b2`, `nvram.txt_r02b_b2` (SEMCO revisions).

## Bluetooth (BCM4361 B2, HCI over UART)
- UART1 `samsung,exynos-uart` (USI v2) at **0x10840000**, SPI 0x1A6, 256-byte FIFO,
  clocks gate_pclk1 / gate_uart1.
- `bluetooth` node (`samsung,bcm43xx`), GPIO order per `drivers/bluetooth/bcm43xx.c`:
  0 = **BT_EN = pin 7 of a PCAL6524 I/O expander at I2C 0x22 on hsi2c@10930000** (not a SoC pin),
  1 = BT_WAKE gpg2-0, 2 = HOST_WAKE gpa2-3. btp-ldo gpm43-0.
- Linux power-on: BT_WAKE high, BT_EN high, wait 100 ms; then the HCI firmware patch (`.hcd`) is
  downloaded over the UART by the Bluetooth HAL (Download_Minidriver 0xFC2E, Write_RAM 0xFC4C chunks,
  Launch_RAM 0xFC4E, then HCI Reset).
- Firmware patch: `bcm4361B2_murata.hcd` / `bcm4361B2_semco.hcd` (pick by module).

## GPU
- ARM Mali-G72 MP18 (`arm,mali`) at **0x17500000** (0x5000), IRQs JOB 0x11A, MMU 0x119, GPU 0x11B,
  power domain `pd-embedded_g3d`, 260-572 MHz DVFS table. No Windows driver exists.

## Audio
- ABOX DSP at 0x17C50000 (sfr), sysreg 0x17C10000, SRAM 0x17D00000 (0x31000), own GIC at 0x17CF1000,
  reserved memory 0xFB200000 (40 MB), IPC through SRAM offsets 0x30000/0x30300, behind an IOMMU.
- Codec: Cirrus Logic **CS47L93** on SPI (reset gpg1-0, IRQ gpa2-2, MCLK gpb0-0), DSP firmware
  `cs47l92-dsp1-trace.wmfw`.
- USB-C / charger: Maxim **MAX77705** on hsi2c@14360000 (PMIC 0x66, charger 0x69). Host-mode VBUS =
  CHG_CNFG_00 (0xB7) OTG|BOOST (bits 1, 3) after CNFG_06 (0xBD) unlock and CNFG_08 (0xBF) 3 MHz;
  only when CHG_INT_OK (0xB2) bit 6 CHGIN_OK is clear.
- Speaker amp: Maxim **MAX98512** on I2C at 0x39 (gpp4-4/5), tuning `dsm.bin`, `dsm_tune.bin`.
- I2S buses: gpb0-1..4, gpb1-0..3, gpb1-4..7, gpb2-0..3.
