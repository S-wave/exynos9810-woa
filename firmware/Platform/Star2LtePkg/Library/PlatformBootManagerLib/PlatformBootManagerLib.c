/** @file
  PlatformBootManagerLib for Star2LtePkg (Exynos 9810 / Galaxy S9+).

  This is the BDS (Boot Device Selection) platform glue. It is what turns "UEFI
  is running" into "Windows is booting":

    1. BeforeConsole: register the serial + framebuffer consoles and connect the
       GIC/serial so there is output.
    2. AfterConsole:  connect every controller (so UFS/USB enumerate), then
       create/refresh a boot option that points at the Windows Boot Manager
       (\EFI\Microsoft\Boot\bootmgfw.efi) on the UFS EFI System Partition, plus a
       UEFI Shell fallback.

  Modeled on MdeModulePkg's reference PlatformBootManagerLib, trimmed to what a
  phone port needs. Nothing here is Exynos-register-specific, so it does not
  depend on any TODO-VERIFY address — it works as soon as the DXE phase and a
  storage stack are alive.
**/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/PrintLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/IoLib.h>
#include <Platform/Exynos9810.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <Protocol/MemoryAttribute.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/ScsiPassThruExt.h>
#include <Protocol/ScsiIo.h>
#include <Protocol/Cpu.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/PartitionInfo.h>
#include <Guid/Acpi.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Guid/GlobalVariable.h>
#include <Guid/SmBios.h>
#include <IndustryStandard/Acpi63.h>
#include <IndustryStandard/PeImage.h>
#include "UsbDebug.h"

//
// Windows Boot Manager path on the EFI System Partition.
//
#define WINDOWS_BOOTMGR_PATH   L"efi\\microsoft\\boot\\bootmgfw.efi"
#define REMOVABLE_BOOT_PATH    L"efi\\boot\\bootaa64.efi"
#define BOOTMGR_EFI_PATH       L"bootmgr.efi"
#define WINDOWS_BOOTMGR_ABS    L"\\efi\\microsoft\\boot\\bootmgfw.efi"
#define REMOVABLE_BOOT_ABS     L"\\efi\\boot\\bootaa64.efi"
#define BOOTMGR_EFI_ABS        L"\\bootmgr.efi"
#define WINDOWS_BOOT_OPTION     L"Windows Boot Manager"
#define FALLBACK_BOOT_OPTION    L"UEFI Shell"

#define STAR2LTE_FAST_STORAGE_ENUM  1

//
// ext209 retained the ext95 diagnostic/latch profile and restored the kernel's
// real ASID/TLBI path. The CNTFRQ and epoch stubs share validated linker tail
// padding discovered from the loaded ntoskrnl PE instead of a build-specific offset.
//
#define STAR2LTE_EXT209_REAL_TLBI             1
#define STAR2LTE_NTOS_DYNAMIC_CAVE_SIZE       0x40u
#define STAR2LTE_NTOS_EPOCH_CAVE_OFFSET       0x20u

//
// ext210 NTOS PATCH PROBE. Diagnostic-only: a build with this set to 1 reads the
// ntoskrnl word-patch site back while ntoskrnl is still in DRAM, then resets the
// SoC BEFORE returning to winload, so Windows never starts.
//
// This exists because a SUCCESSFUL Windows boot destroys every firmware record:
// the Exynos9810Ufs miniport re-inits the PRAM ring at cursor 0 during HwInitialize,
// so =NTPATCH/=NTCMP are gone long before TWRP's pstore reads the region. A v16
// capture that reached Windows contained ONLY =UFSINIT and =UFSREBOOT (2,672 B of a
// 16,128 B ring, head zero-filled) - no firmware markers at all. Resetting here is
// the only way to get the patch telemetry out.
//
// NEVER ship this set to 1: it makes the device unable to boot Windows.
//
// RESULT (2026-08-03, candidate boot-v17-ntprobe, ring 70865A94...F8B6):
//   =NTID    b=0000000092400000 e=0091bae0 c=00549e00
//   =NTCFQ   b=0000000092400000 c=0000000092949e00 n=04
//   =NTCMP   n=01
//   =NTPATCH n=05 cf=04 i=26 b=0000000092400000
//   =NTPROBE w=52800040
// The kernel was found at 0x92400000 out of 26 PE images, the dynamic cave
// resolved to 0x549E00 exactly as Star2LteGetNtosLayout computes it for 19041,
// and the patch site reads back as `mov w0, #2`. The word patch APPLIES.
// Since Windows still reported NPACT/NPMAX/NPGRP=1 under boot-v16 with this
// exact patch live, forging PSCI v0.2 is NOT sufficient to enable SMP.
//
#define STAR2LTE_NTOS_PATCH_PROBE             0

//
// ext211 PSCI HANDOFF-STATE PROBE. Diagnostic-only, same discipline as the
// NTOS patch probe above: it emits PSCI state from the ExitBootServices hook
// and then cold-resets, so Windows never starts and the firmware ring survives
// to be read from TWRP's pstore.
//
// It reads only - PSCI_VERSION and AFFINITY_INFO. Nothing is started, nothing
// is allocated, no core state is changed.
//
// It exists because the Windows side is now proven healthy end to end
// (HALMAX=8, HALPC=8, PGATE bit 0 set, PCAPON=2, PVER=0.2, conduit and entry
// non-NULL) while NPACT is still 1, which means ntoskrnl issues PSCI CPU_ON
// and CPU_ON fails. See the probe body for the full evidence block.
//
// NEVER ship this set to 1: it makes the device unable to boot Windows.
//
// SPENT 2026-08-03. Both questions it was built to ask are answered:
// PSCI_VERSION and AFFINITY_INFO return NOT_SUPPORTED (boot-v19), and the
// selftest it protected showed CPU_ON works for the A55s and kills the SoC on
// the first M3 (boot-v20). Back to 0 so the device boots Windows again.
//
#define STAR2LTE_PSCI_HANDOFF_PROBE           0

//
// CYCLE A8: auto-reboot-to-recovery on the "no bootable device" failure path so the
// bring-up cycle is unattended. On Exynos9810, sboot boots the RECOVERY partition when
// PMU SYSIP_DAT0 (0x14060810) == INFORM_RECOVERY (0xF); a SoC software reset is PMU
// SWRESET (0x14060400) = 0x1 (from drivers/soc/samsung/exynos-reboot.c). A visible
// countdown precedes it so the panel is readable/photographable and the serial diag
// flushes, and a tight bootloop is impossible (>10 s/iteration; Download mode always escapes).
//
#ifndef STAR2LTE_AUTO_RECOVERY
#define STAR2LTE_AUTO_RECOVERY          1
#endif
#ifndef STAR2LTE_AUTO_RECOVERY_DELAY_S
#define STAR2LTE_AUTO_RECOVERY_DELAY_S  10
#endif

#ifndef STAR2LTE_BOOT_WATCHDOG
#define STAR2LTE_BOOT_WATCHDOG          1
#endif
#ifndef STAR2LTE_BOOT_WATCHDOG_TIMEOUT_S
#define STAR2LTE_BOOT_WATCHDOG_TIMEOUT_S  300
#endif
#ifndef STAR2LTE_BOOT_WATCHDOG_RESET_GRACE_S
#define STAR2LTE_BOOT_WATCHDOG_RESET_GRACE_S  30
#endif
//
// Star2Lte: the watchdog fires as a PERIODIC timer every STAR2LTE_WD_TICK_S seconds. Each tick
// appends an "=HB<n>" heartbeat directly to the PRAM trace (bypassing the event-count limit) and
// only triggers recovery once enough ticks elapse without a pet (TIMEOUT_S total). This is an
// interrupt-driven liveness probe: if =HB keeps climbing after winload stops calling boot services,
// winload is ALIVE (spinning with interrupts enabled); if =HB freezes, it faulted / masked IRQs.
//
#ifndef STAR2LTE_WD_TICK_S
#define STAR2LTE_WD_TICK_S  3
#endif

//
// Star2Lte: BOOT-ATTEMPT WATCHDOG. STAR2LTE_BOOT_WATCHDOG above is an EFI timer event, so
// it dies at ExitBootServices and cannot see a Windows-side bugcheck. A boot-start driver
// that bugchecks therefore warm-resets straight back into Windows and repeats forever, with
// no path back to recovery -- which is exactly how the phone stranded itself during the UFS
// write-probe bring-up and why the bring-up loop stopped being unattended.
//
// Close it from the one place that runs on every Windows-path boot: count attempts in a word
// that survives the warm reset, and let whoever owns the Windows side clear it once it is
// healthy, so only boots that never get that far accumulate. After
// STAR2LTE_BOOT_ATTEMPT_LIMIT consecutive unacknowledged attempts, divert to TWRP using the
// already-proven INFORM3 + SWRESET path. Recovery boots go sboot -> TWRP without running
// UEFI, so time spent in recovery never counts. Worst case this costs LIMIT boot cycles, and
// the only thing it can ever do is land in recovery, which is the lifeline partition.
//
#ifndef STAR2LTE_BOOT_ATTEMPT_WATCHDOG
#define STAR2LTE_BOOT_ATTEMPT_WATCHDOG  1
#endif

//
// Big-cluster probe builds only. MUST be 0 in anything shippable -- it turns
// every boot into a recovery divert.
//
// Measured 2026-08-03: the PRAM ring holds exactly ONE firmware boot. Boot 3's
// capture starts at the DXE dispatch chars and ends at "=RECOV r=B", so boots 1
// and 2 -- the ones that actually ran the probe -- are unreadable, and only the
// tiny PRAM latch survives. Running the probe from inside the divert branch
// makes the probe boot BE the boot that parks in TWRP, so its own "=APMPRE" /
// "=APMDIFF" records are the ones the host reads. With the limit at 1 that is a
// single ~15 s boot per experiment instead of three.
//
#ifndef STAR2LTE_BIG_PROBE_DIVERT
#define STAR2LTE_BIG_PROBE_DIVERT  0
#endif

#ifndef STAR2LTE_BOOT_ATTEMPT_LIMIT
#if STAR2LTE_BIG_PROBE_DIVERT
#define STAR2LTE_BOOT_ATTEMPT_LIMIT     1u
#else
#define STAR2LTE_BOOT_ATTEMPT_LIMIT     3u
#endif
#endif

#ifndef STAR2LTE_EBS_HW_WATCHDOG
#define STAR2LTE_EBS_HW_WATCHDOG  0
#endif
#ifndef STAR2LTE_EBS_HW_WATCHDOG_COUNT
#define STAR2LTE_EBS_HW_WATCHDOG_COUNT  0xFFF5u
#endif

#ifndef STAR2LTE_TOUCH_RAIL_FIX
#define STAR2LTE_TOUCH_RAIL_FIX  1
#endif

#ifndef STAR2LTE_RECOVERY_RESET_TRACE_SITE
#define STAR2LTE_RECOVERY_RESET_TRACE_SITE  99u
#endif

#ifndef STAR2LTE_LIVE_HUD
#define STAR2LTE_LIVE_HUD              1
#endif

#ifndef STAR2LTE_PANEL_DIAG_TEXT
//
// Re-enabled 2026-08-01. With this at 0 the whole AfterConsole diagnostic block
// is compile-time dead, which silently removed ReportStorageOnScreen() -- so
// "=WINFS blk= part= fs=" was NEVER emitted by any build, and every capture
// that lacked it was vacuous rather than informative. It is also why the trace
// shows 'r' (the ConOut-is-NULL else-branch) on every boot even though ConOut
// is demonstrably non-NULL later in UnableToBoot ('i'/'j' are both emitted).
// fs= is the only direct measurement of whether Fat mounted the Windows media.
//
#define STAR2LTE_PANEL_DIAG_TEXT       1
#endif

//
// Star2Lte (2026-07-07, ext40): RE-ENABLED as the DECISIVE handoff test. The "=KE"
// entry cave redirects ntoskrnl's first instruction (pacibsp, position-independent,
// safe to displace) to slack code that writes "=KE" to the PRAM ring at 0xFED14000,
// runs pacibsp, and returns to entry+4.
//
// The 2026-07-06 note below claimed 0xFED14000 is unmapped at kernel entry so the
// write faults. That is almost certainly WRONG: our T6 scanner hooks winload's FINAL
// branch to the kernel (0x912D500C, AFTER the page-table switch at 0x912d5080) and
// SUCCESSFULLY writes "=KJ" to 0xFED14000 immediately before "br x1". The kernel entry
// executes on those SAME tables, so 0xFED14000 must be mapped there too. Because the
// cave REPLACES the kernel's first instruction, absence of "=KE" can ONLY mean the
// kernel entry VA is never executed (winload's br x1 doesn't truly land in the kernel)
// -- which would be the real blocker, not any later kernel code we've been patching.
//   =KE present -> kernel executes entry; hang is later (focus on early kernel init).
//   =KE absent  -> handoff itself is broken (focus on the winload->kernel transfer).
//
// RESULT (2026-07-07, ext40): "=KE" IS PRESENT (PRAM shows =NTENT e=0099d340 c=00201678
// then =KE at the tail of the breadcrumb chain, after =KJ/=X0/=X1/=X2/=XP). CONFIRMED:
// winload's "br x1" lands in the kernel, the kernel's first instruction executes, and
// 0xFED14000 is mapped/writable at kernel entry. The hang is in EARLY KERNEL INIT, not
// the handoff. See STAR2LTE_INSTALL_KE_TRACE below (ext41) for the follow-up bisection.
//
// ext43: SUPERSEDED by the persistent progress LATCH (see STAR2LTE_INSTALL_KE_TRACE below).
// The ring-append "=KE" cave competed with the firmware breadcrumb flood, so a kernel marker
// survived in the ring only NONDETERMINISTICALLY (ext40 kept =KE, ext41 kept only =K0, ext42
// kept nothing). Only ever the single LAST ring write survives at the tail. Entry is now
// proved by latch code 0x01 @ 0x99D340 instead. Set to 1 only to re-run the standalone ring
// "=KE" handoff test in isolation (it conflicts with the latch entry hook at 0x99D340).
#ifndef STAR2LTE_INSTALL_KE_ENTRY_CAVE
#define STAR2LTE_INSTALL_KE_ENTRY_CAVE 0
#endif

#ifndef STAR2LTE_KERNEL_EPOCH_TRACE
#define STAR2LTE_KERNEL_EPOCH_TRACE 0
#endif

#define STAR2LTE_KERNEL_EPOCH_MAGIC       SIGNATURE_32 ('K', 'E', 'P', '1')
#define STAR2LTE_KERNEL_EPOCH_RECORD      0x00000000FED17FB0ULL
#define STAR2LTE_KERNEL_EPOCH_ENTRY_RVA   0x0099D340u
#define STAR2LTE_KERNEL_EPOCH_WAYPOINT_RVA      0x0099D468u
#define STAR2LTE_KERNEL_EPOCH_WAYPOINT_CAVE_RVA 0x002016B8u

//
// Persistent boot-attempt counter (see STAR2LTE_BOOT_ATTEMPT_WATCHDOG above). Lives inside
// the mapped ring PAGE but ABOVE the 0x3F00 flood cap -- ring data ends at 0xFED1400C+0x3F00
// = 0xFED17F0B -- so neither the firmware's trace nor the driver's telemetry can reach it and
// it survives the warm reset. It sits in the free window 0xFED17F0C..0xFED17F5F and is
// deliberately clear of the UFS driver's crash latch at 0xFED17F40/0xFED17F44, RAM_STATUS at
// 0xFED17F60, and the KiSystemStartup latch at 0xFED17F80. Both words are 4-byte aligned:
// every access here is a naturally-aligned 32-bit load/store.
//
#define STAR2LTE_BOOT_ATTEMPT_MAGIC       SIGNATURE_32 ('B', 'A', 'T', '1')
#define STAR2LTE_BOOT_ATTEMPT_MAGIC_ADDR  0x00000000FED17F10ULL
#define STAR2LTE_BOOT_ATTEMPT_COUNT_ADDR  0x00000000FED17F14ULL

#ifndef STAR2LTE_INSTALL_KE_FB_CAVE
#define STAR2LTE_INSTALL_KE_FB_CAVE 0
#endif

//
// Star2Lte (2026-07-07, ext43): KiSystemStartup progress via a PERSISTENT LATCH. Ring
// markers proved unreliable: the 0xFED14000 ring is linear (caps at 0x3F00, no wrap) and is
// re-flooded from ~offset 0 on every UEFI boot, so a kernel marker appended after winload's
// =X2 survives only until the NEXT bootloop's flood overwrites it -- only the single last
// ring write is ever captured. Instead, each hook now writes a monotonically-increasing CODE
// to a FIXED word at 0xFED17F80 (inside the mapped ring PAGE, proven writable at kernel entry
// by =X2 landing at ~0xFED176xx, but ABOVE the 0x3F00 flood cap = 0xFED17F0B, so the firmware
// never overwrites it and it survives the watchdog reset). The furthest-reached hook leaves
// its code; the NEXT boot's firmware reads 0xFED17F80 and prints "=KLAT vNN" into the BDS
// trace (which IS reliably captured, unlike the kernel-tail ring markers). Each hooked site's
// displaced instruction is non-PC-relative (pacibsp/mrs/ldr/mov/str), so relocation into the
// slack cave is safe. =KLAT vNN => furthest sub-call reached (see the code map in the block).
//
#ifndef STAR2LTE_INSTALL_KE_TRACE
#define STAR2LTE_INSTALL_KE_TRACE 0
#endif

#ifndef STAR2LTE_DISK_ACTIVITY_INDICATOR
#define STAR2LTE_DISK_ACTIVITY_INDICATOR 0
#endif

//
// CYCLE A10: use the PROVEN early-build (m1_power.c, DEVICE-VERIFIED) recovery-reboot
// scheme. sboot reads the Samsung SEC reset-reason from PMU INFORM3 (+0x080C), value
// 0x12345670|mode (mode 4 = recovery; mode 1 = download was CONFIRMED). A8's mistake was
// writing 0xF to SYSIP_DAT0 (+0x810) - the WRONG reg/value -> sboot saw NORMAL -> looped
// to UEFI. INFORM3 + PMU SWRESET (+0x400)=1 lands the next power-on in TWRP.
//
#define EXYNOS9810_PMU_INFORM3          0x1406080CULL   // SEC reset-reason reg
#define EXYNOS9810_PMU_INFORM2          0x14060808ULL
#define EXYNOS9810_PMU_SYSIP_DAT0       0x14060810ULL
#define EXYNOS9810_PMU_SWRESET          0x14060400ULL   // SoC software reset
#define EXYNOS9810_PMU_WDT_DISABLE      0x14060408ULL
#define EXYNOS9810_PMU_WDT_MASK_RESET   0x1406040CULL
#define EXYNOS9810_WDT_BASE             0x10050000ULL   // watchdog_cl0
#define SEC_POWER_RESET                 0x12345678u
#define SEC_REBOOT_REASON_RECOVERY      0x12345674u     // 0x12345670 | mode 4
#define EXYNOS_INFORM_RECOVERY          0xFu

#define STAR2LTE_WDT_WTCON              0x00u
#define STAR2LTE_WDT_WTDAT              0x04u
#define STAR2LTE_WDT_WTCNT              0x08u
#define STAR2LTE_WDT_CLUSTER0_RESET_BIT  (1u << 24)
#define STAR2LTE_WDT_ENABLE_RESET       ((0x5Cu << 8) | (3u << 3) | (1u << 5) | 1u)

//
// DIAG (Star2Lte bring-up): pram breadcrumb (0xFED14000, 'DBGC') so we can trace
// the BDS phase, which otherwise runs with no on-screen output. Remove once a
// real console is confirmed on the panel.
//
// LINEAR (v15 bisect): PW[1] is the ramoops `start` field. It is written to 0
// once, at magic init, and never advanced. Writes are capped at 0x3F00 so the
// ring retains the FIRST 16,128 bytes. This is the v11 form and is the only
// arrangement observed to leave the ramoops zone valid enough for Linux to
// expose /sys/fs/pstore/pmsg-ramoops-0.
// LINEAR CAP (v11 form). The circular variant that honoured the ramoops
// `start` field (PW[1]) is reverted while bisecting the v11 -> v12 Windows
// logo hang. Writes stop once the ring is full; PW[1] is left at 0.
#define BDS_DPUT(ch)  do {                                            \
    volatile UINT32 *PW = (volatile UINT32 *)(UINTN)0xFED14000ULL;    \
    volatile UINT8  *PB = (volatile UINT8  *)(UINTN)0xFED14000ULL;    \
    UINT32 PS;                                                        \
    if (PW[0] != 0x43474244u) { PW[0]=0x43474244u; PW[1]=0; PW[2]=0; }\
    PS = PW[2];                                                       \
    if (PS < 0x3F00u) { PB[12u+PS] = (UINT8)(ch); PW[2] = PS + 1u; }  \
    __asm__ __volatile__ ("dsb sy" ::: "memory");                     \
  } while (0)

//
// CYCLE A10: persistent-RAM string/hex writers (same 0xFED14000 'DBGC' buffer as
// BDS_DPUT -> /sys/fs/pstore/pmsg-ramoops-0) for the LBA-sweep capture, read from
// TWRP with `tools/diag-cycle.ps1 -Read`.
//
//
// SMP DIAG: optional tee. When mBdsTeeBuf is non-NULL every byte that passes
// through BdsPramByte -- and therefore every byte from BdsPramStr/Hex/Hex64/
// StatusLite, which all funnel through it -- is also appended here. This exists
// because the Windows UFS miniport maps the same 0xFED14000 ring and rewinds it
// at init, so firmware bytes never survive to pstore. The tee is drained to a
// raw GPT partition instead.
//
STATIC UINT8   *mBdsTeeBuf = NULL;
STATIC UINT32  mBdsTeeLen  = 0;
STATIC UINT32  mBdsTeeMax  = 0;

STATIC
VOID
BdsPramByte (
  IN UINT8  Ch
  )
{
  volatile UINT32  *PW = (volatile UINT32 *)(UINTN)0xFED14000ULL;
  volatile UINT8   *PB = (volatile UINT8  *)(UINTN)0xFED14000ULL;
  UINT32           PS;

  if ((mBdsTeeBuf != NULL) && (mBdsTeeLen < mBdsTeeMax)) {
    mBdsTeeBuf[mBdsTeeLen++] = Ch;
  }

  if (PW[0] != 0x43474244u) {
    PW[0] = 0x43474244u; PW[1] = 0; PW[2] = 0;
  }
  PS = PW[2];
  //
  // Linear cap (v11 form): write only while the ring has room and leave the
  // ramoops `start` field at 0. Reverted from the circular variant pending a
  // bisect of the v11 -> v12 Windows-logo hang.
  //
  if (PS < 0x3F00u) {
    PB[12u + PS] = Ch;
    PW[2] = PS + 1u;
  }

#if STAR2LTE_USB_DEBUG_MODE == STAR2LTE_USB_DEBUG_HID
  Star2LteUsbDebugWriteByte (Ch);
#endif
}

STATIC
VOID
BdsPramStr (
  IN CONST CHAR8  *Str
  )
{
  while (*Str != '\0') {
    BdsPramByte ((UINT8)*Str++);
  }
}

STATIC
VOID
BdsPramHex (
  IN UINT32  Val,
  IN UINTN   Nibbles
  )
{
  CONST CHAR8  *Hex = "0123456789abcdef";

  while (Nibbles-- > 0) {
    BdsPramByte ((UINT8)Hex[(Val >> (Nibbles * 4u)) & 0xFu]);
  }
}

STATIC
VOID
BdsPramHex64 (
  IN UINT64  Val,
  IN UINTN   Nibbles
  )
{
  while (Nibbles > 8) {
    BdsPramHex ((UINT32)(Val >> ((Nibbles - 8u) * 4u)), 8);
    Nibbles -= 8;
  }

  BdsPramHex ((UINT32)Val, Nibbles);
}

STATIC
VOID
BdsPramStatusLite (
  IN EFI_STATUS  Status
  )
{
  BdsPramByte (EFI_ERROR (Status) ? (UINT8)'E' : (UINT8)'S');
  BdsPramHex ((UINT32)(Status & 0xFFFFu), 4);
}

#if STAR2LTE_TOUCH_RAIL_FIX
//
// TWRP's normal shutdown disables the SEC_TS 1.8 V and 3.0 V supplies. Restore
// only their enable-mode bits through the Exynos SPEEDY controller; voltage
// selector mismatches abort all writes.
//
#define STAR2LTE_SPEEDY_CTRL_OFFSET             0x000u
#define STAR2LTE_SPEEDY_FIFO_CTRL_OFFSET        0x004u
#define STAR2LTE_SPEEDY_CMD_OFFSET              0x008u
#define STAR2LTE_SPEEDY_INT_ENABLE_OFFSET       0x00Cu
#define STAR2LTE_SPEEDY_INT_STATUS_OFFSET       0x010u
#define STAR2LTE_SPEEDY_TX_DATA_OFFSET          0x034u
#define STAR2LTE_SPEEDY_RX_DATA_OFFSET          0x038u

#define STAR2LTE_SPEEDY_CTRL_ENABLE             BIT0
#define STAR2LTE_SPEEDY_CTRL_RESET              BIT31
#define STAR2LTE_SPEEDY_FIFO_RESET              BIT31
#define STAR2LTE_SPEEDY_FIFO_SINGLE_BYTE        0x00000101u
#define STAR2LTE_SPEEDY_CMD_RANDOM              BIT19
#define STAR2LTE_SPEEDY_CMD_WRITE               BIT20
#define STAR2LTE_SPEEDY_TRANSFER_DONE           BIT0
#define STAR2LTE_SPEEDY_TIMEOUT_ERRORS          (BIT1 | BIT2 | BIT3)
#define STAR2LTE_SPEEDY_FIFO_TX_ALMOST_EMPTY    BIT4
#define STAR2LTE_SPEEDY_FIFO_RX_ALMOST_FULL     BIT8
#define STAR2LTE_SPEEDY_RX_FIFO_TRAILER         BIT9
#define STAR2LTE_SPEEDY_RX_MODE_ERROR            BIT16
#define STAR2LTE_SPEEDY_CRC_ERROR               BIT17
#define STAR2LTE_SPEEDY_RX_END_ERROR             BIT18
#define STAR2LTE_SPEEDY_TX_LINE_BUSY_ERROR       BIT20
#define STAR2LTE_SPEEDY_TX_STOP_ERROR            BIT21
#define STAR2LTE_SPEEDY_REMOTE_RESET             BIT31
#define STAR2LTE_SPEEDY_READ_INT_ENABLE          \
  (STAR2LTE_SPEEDY_TRANSFER_DONE | STAR2LTE_SPEEDY_TIMEOUT_ERRORS | \
   STAR2LTE_SPEEDY_FIFO_RX_ALMOST_FULL | STAR2LTE_SPEEDY_RX_FIFO_TRAILER | \
   STAR2LTE_SPEEDY_RX_MODE_ERROR | STAR2LTE_SPEEDY_CRC_ERROR | \
   STAR2LTE_SPEEDY_RX_END_ERROR | STAR2LTE_SPEEDY_REMOTE_RESET)
#define STAR2LTE_SPEEDY_WRITE_INT_ENABLE         \
  (STAR2LTE_SPEEDY_TRANSFER_DONE | STAR2LTE_SPEEDY_TIMEOUT_ERRORS | \
   STAR2LTE_SPEEDY_FIFO_TX_ALMOST_EMPTY | \
   STAR2LTE_SPEEDY_TX_LINE_BUSY_ERROR | STAR2LTE_SPEEDY_TX_STOP_ERROR | \
   STAR2LTE_SPEEDY_REMOTE_RESET)
#define STAR2LTE_SPEEDY_PROTOCOL_ERRORS          \
  (STAR2LTE_SPEEDY_RX_MODE_ERROR | STAR2LTE_SPEEDY_RX_END_ERROR | \
   STAR2LTE_SPEEDY_TX_LINE_BUSY_ERROR | STAR2LTE_SPEEDY_TX_STOP_ERROR)
#define STAR2LTE_SPEEDY_ALL_ERRORS              \
  (STAR2LTE_SPEEDY_TIMEOUT_ERRORS | STAR2LTE_SPEEDY_CRC_ERROR | \
   STAR2LTE_SPEEDY_PROTOCOL_ERRORS | STAR2LTE_SPEEDY_REMOTE_RESET)
#define STAR2LTE_SPEEDY_POLL_COUNT              1000u
#define STAR2LTE_SPEEDY_PMIC_SLAVE              0x01u

#define STAR2LTE_S2MPS18_LDO35_CTRL             0x6Cu
#define STAR2LTE_S2MPS18_LDO43_CTRL             0x74u
#define STAR2LTE_S2MPS18_SELECTOR_MASK          0x3Fu
#define STAR2LTE_S2MPS18_ENABLE_MASK            0xC0u
#define STAR2LTE_S2MPS18_LDO35_SELECTOR_1V8     0x2Cu
#define STAR2LTE_S2MPS18_LDO43_SELECTOR_3V0     0x30u

STATIC_ASSERT (
  STAR2LTE_SPEEDY_READ_INT_ENABLE == 0x8007030Fu,
  "SPEEDY read interrupt mask mismatch"
  );
STATIC_ASSERT (
  STAR2LTE_SPEEDY_WRITE_INT_ENABLE == 0x8030001Fu,
  "SPEEDY write interrupt mask mismatch"
  );
STATIC_ASSERT (
  STAR2LTE_SPEEDY_ALL_ERRORS == 0x8037000Eu,
  "SPEEDY terminal error mask mismatch"
  );

STATIC
UINTN
Star2LteSpeedyRegister (
  IN UINTN  Offset
  )
{
  return (UINTN)(EXYNOS_SPEEDY_BASE + Offset);
}

STATIC
EFI_STATUS
Star2LteSpeedyStatusToEfiStatus (
  IN UINT32  SpeedyStatus
  )
{
  if ((SpeedyStatus & STAR2LTE_SPEEDY_REMOTE_RESET) != 0) {
    return EFI_DEVICE_ERROR;
  }

  if ((SpeedyStatus & STAR2LTE_SPEEDY_TIMEOUT_ERRORS) != 0) {
    return EFI_TIMEOUT;
  }

  if ((SpeedyStatus & STAR2LTE_SPEEDY_CRC_ERROR) != 0) {
    return EFI_CRC_ERROR;
  }

  if ((SpeedyStatus & STAR2LTE_SPEEDY_PROTOCOL_ERRORS) != 0) {
    return EFI_PROTOCOL_ERROR;
  }

  return EFI_DEVICE_ERROR;
}

STATIC
VOID
Star2LteSpeedyQuiesce (
  VOID
  )
{
  MmioWrite32 (
    Star2LteSpeedyRegister (STAR2LTE_SPEEDY_INT_ENABLE_OFFSET),
    0
    );
  MmioWrite32 (
    Star2LteSpeedyRegister (STAR2LTE_SPEEDY_INT_STATUS_OFFSET),
    0xFFFFFFFFu
    );
}

STATIC
EFI_STATUS
Star2LteSpeedyTransfer8 (
  IN     UINT8    Register,
  IN     BOOLEAN  Write,
  IN OUT UINT8    *Value,
  OUT    UINT32   *RawStatus OPTIONAL
  )
{
  UINT32  Command;
  UINT32  SpeedyStatus;
  UINTN   Poll;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (RawStatus != NULL) {
    *RawStatus = 0;
  }

  // Complete FIFO reset before programming the one-byte trigger levels.
  MmioWrite32 (
    Star2LteSpeedyRegister (STAR2LTE_SPEEDY_FIFO_CTRL_OFFSET),
    STAR2LTE_SPEEDY_FIFO_RESET
    );
  (VOID)MmioRead32 (
          Star2LteSpeedyRegister (STAR2LTE_SPEEDY_FIFO_CTRL_OFFSET)
          );
  MmioWrite32 (
    Star2LteSpeedyRegister (STAR2LTE_SPEEDY_FIFO_CTRL_OFFSET),
    STAR2LTE_SPEEDY_FIFO_SINGLE_BYTE
    );
  MmioWrite32 (
    Star2LteSpeedyRegister (STAR2LTE_SPEEDY_INT_STATUS_OFFSET),
    0xFFFFFFFFu
    );
  MmioWrite32 (
    Star2LteSpeedyRegister (STAR2LTE_SPEEDY_INT_ENABLE_OFFSET),
    Write ? STAR2LTE_SPEEDY_WRITE_INT_ENABLE :
            STAR2LTE_SPEEDY_READ_INT_ENABLE
    );

  Command = ((((UINT32)STAR2LTE_SPEEDY_PMIC_SLAVE << 8) | Register) << 7) |
            STAR2LTE_SPEEDY_CMD_RANDOM;
  if (Write) {
    Command |= STAR2LTE_SPEEDY_CMD_WRITE;
  }

  MmioWrite32 (
    Star2LteSpeedyRegister (STAR2LTE_SPEEDY_CMD_OFFSET),
    Command
    );
  if (Write) {
    MmioWrite32 (
      Star2LteSpeedyRegister (STAR2LTE_SPEEDY_TX_DATA_OFFSET),
      *Value
      );
  }

  SpeedyStatus = 0;
  for (Poll = 0; Poll < STAR2LTE_SPEEDY_POLL_COUNT; Poll++) {
    SpeedyStatus = MmioRead32 (
                     Star2LteSpeedyRegister (
                       STAR2LTE_SPEEDY_INT_STATUS_OFFSET
                       )
                     );
    if ((SpeedyStatus &
         (STAR2LTE_SPEEDY_TRANSFER_DONE |
          STAR2LTE_SPEEDY_ALL_ERRORS)) != 0) {
      break;
    }

    gBS->Stall (1);
  }

  if (RawStatus != NULL) {
    *RawStatus = SpeedyStatus;
  }

  if (Poll == STAR2LTE_SPEEDY_POLL_COUNT) {
    Star2LteSpeedyQuiesce ();
    return EFI_TIMEOUT;
  }

  if ((SpeedyStatus & STAR2LTE_SPEEDY_ALL_ERRORS) != 0) {
    Star2LteSpeedyQuiesce ();
    return Star2LteSpeedyStatusToEfiStatus (SpeedyStatus);
  }

  if ((SpeedyStatus & STAR2LTE_SPEEDY_TRANSFER_DONE) == 0) {
    Star2LteSpeedyQuiesce ();
    return EFI_DEVICE_ERROR;
  }

  MmioWrite32 (
    Star2LteSpeedyRegister (STAR2LTE_SPEEDY_INT_STATUS_OFFSET),
    0xFFFFFFFFu
    );
  if (!Write) {
    *Value = (UINT8)MmioRead32 (
                       Star2LteSpeedyRegister (
                         STAR2LTE_SPEEDY_RX_DATA_OFFSET
                         )
                       );
  }

  Star2LteSpeedyQuiesce ();
  return EFI_SUCCESS;
}

STATIC
VOID
Star2LteTraceTouchRailPhase (
  IN UINT8       Phase,
  IN EFI_STATUS  Status,
  IN UINT8       Value,
  IN UINT32      RawStatus
  )
{
  BdsPramStr ("=TS p=");
  BdsPramHex (Phase, 2);
  BdsPramStr (" s=");
  BdsPramStatusLite (Status);
  BdsPramStr (" v=");
  BdsPramHex (Value, 2);
  BdsPramStr (" is=");
  BdsPramHex (RawStatus, 8);
  BdsPramByte ((UINT8)'\n');
}

STATIC
EFI_STATUS
Star2LteInitializeSpeedy (
  OUT UINT32  *Control
  )
{
  if (Control == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Star2LteSpeedyQuiesce ();
  MmioOr32 (
    Star2LteSpeedyRegister (STAR2LTE_SPEEDY_CTRL_OFFSET),
    STAR2LTE_SPEEDY_CTRL_RESET
    );
  gBS->Stall (10);
  MmioOr32 (
    Star2LteSpeedyRegister (STAR2LTE_SPEEDY_CTRL_OFFSET),
    STAR2LTE_SPEEDY_CTRL_ENABLE
    );

  *Control = MmioRead32 (
               Star2LteSpeedyRegister (STAR2LTE_SPEEDY_CTRL_OFFSET)
               );
  if ((*Control & STAR2LTE_SPEEDY_CTRL_ENABLE) == 0) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
Star2LteEnableTouchRails (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Ldo35;
  UINT8       Ldo43;
  UINT32      RawStatus;
  UINT32      Control;
  BOOLEAN     Ldo35Enabled;
  BOOLEAN     Ldo43Enabled;

  BDS_DPUT ('T');
  Control = 0;
  Status  = Star2LteInitializeSpeedy (&Control);
  Star2LteTraceTouchRailPhase (0x01, Status, (UINT8)Control, Control);
  if (EFI_ERROR (Status)) {
    BDS_DPUT ('!');
    return Status;
  }

  Ldo35     = 0xFF;
  RawStatus = 0;
  Status    = Star2LteSpeedyTransfer8 (
                STAR2LTE_S2MPS18_LDO35_CTRL,
                FALSE,
                &Ldo35,
                &RawStatus
                );
  Star2LteTraceTouchRailPhase (0x35, Status, Ldo35, RawStatus);
  if (EFI_ERROR (Status)) {
    BDS_DPUT ('!');
    return Status;
  }

  Ldo43     = 0xFF;
  RawStatus = 0;
  Status    = Star2LteSpeedyTransfer8 (
                STAR2LTE_S2MPS18_LDO43_CTRL,
                FALSE,
                &Ldo43,
                &RawStatus
                );
  Star2LteTraceTouchRailPhase (0x43, Status, Ldo43, RawStatus);
  if (EFI_ERROR (Status)) {
    BDS_DPUT ('!');
    return Status;
  }

  if ((Ldo35 & STAR2LTE_S2MPS18_SELECTOR_MASK) !=
      STAR2LTE_S2MPS18_LDO35_SELECTOR_1V8) {
    Status = EFI_ABORTED;
    Star2LteTraceTouchRailPhase (0xA1, Status, Ldo35, 0);
    BDS_DPUT ('!');
    return Status;
  }

  if ((Ldo43 & STAR2LTE_S2MPS18_SELECTOR_MASK) !=
      STAR2LTE_S2MPS18_LDO43_SELECTOR_3V0) {
    Status = EFI_ABORTED;
    Star2LteTraceTouchRailPhase (0xA2, Status, Ldo43, 0);
    BDS_DPUT ('!');
    return Status;
  }

  Ldo35Enabled = (BOOLEAN)(
                   (Ldo35 & STAR2LTE_S2MPS18_ENABLE_MASK) ==
                   STAR2LTE_S2MPS18_ENABLE_MASK
                   );
  Ldo43Enabled = (BOOLEAN)(
                   (Ldo43 & STAR2LTE_S2MPS18_ENABLE_MASK) ==
                   STAR2LTE_S2MPS18_ENABLE_MASK
                   );
  if (!Ldo35Enabled && Ldo43Enabled) {
    Status = EFI_ABORTED;
    Star2LteTraceTouchRailPhase (0xA3, Status, Ldo43, 0);
    BDS_DPUT ('!');
    return Status;
  }

  if (!Ldo35Enabled) {
    Ldo35    |= STAR2LTE_S2MPS18_ENABLE_MASK;
    RawStatus = 0;
    Status    = Star2LteSpeedyTransfer8 (
                  STAR2LTE_S2MPS18_LDO35_CTRL,
                  TRUE,
                  &Ldo35,
                  &RawStatus
                  );
    Star2LteTraceTouchRailPhase (0xE5, Status, Ldo35, RawStatus);
    if (EFI_ERROR (Status)) {
      BDS_DPUT ('!');
      return Status;
    }
  }

  if (!Ldo43Enabled) {
    // Match SEC_TS power-on ordering: DVDD first, then AVDD after 1 ms.
    gBS->Stall (1000);
    Ldo43    |= STAR2LTE_S2MPS18_ENABLE_MASK;
    RawStatus = 0;
    Status    = Star2LteSpeedyTransfer8 (
                  STAR2LTE_S2MPS18_LDO43_CTRL,
                  TRUE,
                  &Ldo43,
                  &RawStatus
                  );
    Star2LteTraceTouchRailPhase (0xE3, Status, Ldo43, RawStatus);
    if (EFI_ERROR (Status)) {
      BDS_DPUT ('!');
      return Status;
    }
  }

  Ldo35     = 0xFF;
  RawStatus = 0;
  Status    = Star2LteSpeedyTransfer8 (
                STAR2LTE_S2MPS18_LDO35_CTRL,
                FALSE,
                &Ldo35,
                &RawStatus
                );
  Star2LteTraceTouchRailPhase (0xB5, Status, Ldo35, RawStatus);
  if (EFI_ERROR (Status) ||
      (Ldo35 != (STAR2LTE_S2MPS18_ENABLE_MASK |
                 STAR2LTE_S2MPS18_LDO35_SELECTOR_1V8))) {
    if (!EFI_ERROR (Status)) {
      Status = EFI_DEVICE_ERROR;
    }

    BDS_DPUT ('!');
    return Status;
  }

  Ldo43     = 0xFF;
  RawStatus = 0;
  Status    = Star2LteSpeedyTransfer8 (
                STAR2LTE_S2MPS18_LDO43_CTRL,
                FALSE,
                &Ldo43,
                &RawStatus
                );
  Star2LteTraceTouchRailPhase (0xB3, Status, Ldo43, RawStatus);
  if (EFI_ERROR (Status) ||
      (Ldo43 != (STAR2LTE_S2MPS18_ENABLE_MASK |
                 STAR2LTE_S2MPS18_LDO43_SELECTOR_3V0))) {
    if (!EFI_ERROR (Status)) {
      Status = EFI_DEVICE_ERROR;
    }

    BDS_DPUT ('!');
    return Status;
  }

  BDS_DPUT ('t');
  return EFI_SUCCESS;
}
#endif


#define STAR2LTE_CALLER_RA()  ((UINT64)(UINTN)__builtin_return_address (0))

STATIC
UINT64
Star2LteReadVbarEl1 (
  VOID
  )
{
  UINT64  Val;

  __asm__ __volatile__ ("mrs %0, vbar_el1" : "=r" (Val));
  return Val;
}

STATIC
UINT64
Star2LteReadDaif (
  VOID
  )
{
  UINT64  Val;

  __asm__ __volatile__ ("mrs %0, daif" : "=r" (Val));
  return Val;
}

//
// MPIDR_EL1 of the running (boot/BSP) core. The BSP runs all of DXE/BDS, winload,
// and ntoskrnl's KiSystemStartup, so this identifies the core Windows treats as the
// boot processor. Our MADT lists the LITTLE A55 (MPIDR 0x0) first; if the device
// boots on a big M3 (0x100) that GICC ordering is wrong.
//
STATIC
UINT64
Star2LteReadMpidrEl1 (
  VOID
  )
{
  UINT64  Val;

  __asm__ __volatile__ ("mrs %0, mpidr_el1" : "=r" (Val));
  return Val;
}

STATIC
UINT64
Star2LteReadCurrentEl (
  VOID
  )
{
  UINT64  Val;

  __asm__ __volatile__ ("mrs %0, CurrentEL" : "=r" (Val));
  return Val;
}

STATIC
UINT64
Star2LteReadTtbr0El1 (
  VOID
  )
{
  UINT64  Val;

  __asm__ __volatile__ ("mrs %0, ttbr0_el1" : "=r" (Val));
  return Val;
}

STATIC
UINT64
Star2LteReadTcrEl1 (
  VOID
  )
{
  UINT64  Val;

  __asm__ __volatile__ ("mrs %0, tcr_el1" : "=r" (Val));
  return Val;
}

//
// Walk the EL1 stage-1 TTBR0 page tables for VA and dump each level's descriptor
// to PRAM under the given Tag. Assumes the EDK II ArmMmuLib default for this
// platform: 4 KB granule, 48-bit VA (T0SZ=16) => 4 levels (L0..L3), VA[47:39]/
// [38:30]/[29:21]/[20:12]. The walk only READS the translation tables (identity-
// mapped DRAM, always mapped WB) so it is safe even when VA itself is unmapped.
// Descriptor[1:0]: 0b11=table(L0-2)/page(L3), 0b01=block(L0-2), else invalid.
// This is the decisive probe for the pre-EBS L3 translation fault at ~0xbbc59xxx:
// comparing the descriptors captured early in BDS vs at fault time tells us whether
// the page was never mapped (firmware/image-protection) or got unmapped by
// winload's pre-EBS allocation storm (map/pagetable corruption).
//
STATIC
VOID
Star2LteDumpPtWalk (
  IN CONST CHAR8  *Tag,
  IN UINT64       Va
  )
{
  UINT64           TableBase;
  UINT64           Desc;
  UINTN            Level;
  UINTN            Shift;
  UINT64           Idx;
  volatile UINT64  *Tbl;

  Desc = 0;
  BdsPramStr ("\n=");
  BdsPramStr (Tag);
  BdsPramStr (" va=");
  BdsPramHex64 (Va, 16);
  BdsPramStr (" tcr=");
  BdsPramHex64 (Star2LteReadTcrEl1 (), 16);
  BdsPramStr (" ttbr0=");
  BdsPramHex64 (Star2LteReadTtbr0El1 (), 16);

  //
  // Base of the L0 table: TTBR0 BADDR bits, ASID/CnP cleared, page-aligned.
  //
  TableBase = Star2LteReadTtbr0El1 () & 0x0000FFFFFFFFF000ULL;

  for (Level = 0; Level <= 3; Level++) {
    Shift = 39 - (Level * 9);
    Idx   = (Va >> Shift) & 0x1FFULL;
    Tbl   = (volatile UINT64 *)(UINTN)TableBase;
    Desc  = Tbl[Idx];

    BdsPramStr (" L");
    BdsPramHex ((UINT32)Level, 1);
    BdsPramByte ('[');
    BdsPramHex ((UINT32)Idx, 3);
    BdsPramStr ("]=");
    BdsPramHex64 (Desc, 16);

    if ((Desc & 0x3ULL) != 0x3ULL) {
      //
      // 0b01 at L0-2 = valid block mapping; anything else (0b00/0b10, or 0b01 at
      // L3) = invalid / translation fault source. Cannot descend either way.
      //
      if (((Desc & 0x3ULL) == 0x1ULL) && (Level < 3)) {
        BdsPramStr ("(blk)");
      } else {
        BdsPramStr ("(inv)");
      }
      break;
    }

    if (Level == 3) {
      BdsPramStr ("(pg)");
      break;
    }

    TableBase = Desc & 0x0000FFFFFFFFF000ULL;
  }

  BdsPramByte ('\n');
}

//
// ext71: Full stage-1 page-table CONTENT scan of the Windows kernel PT root
// (TTBR0 @ 0x907AA000 low half, TTBR1 @ 0x907AA800 high half; 4 KB granule,
// T0SZ/T1SZ=17 => 256-entry L0, 512-entry L1..L3). Read via winload's identity
// map (physical == virtual for DRAM at the ExitBootServices point, BEFORE the
// kernel PT is installed at br x1). This is the decisive H-A-content test: uH/RKP
// traps the kernel tlbi and walks THIS table; if any LEAF descriptor maps a VA to
// an output PA inside the secure/protected band [0xBC800000, 0x1_0000_0000)
// (Secure DRAM 0xBC800000, Secure PGTBL 0xE0000000, H-Arx) that is what its walker
// chokes on. Bounded (table cap + <=12 samples) so it cannot flood PRAM or run
// away on a malformed / self-referential table.
//
#define STAR2LTE_PT_DRAM_LO    0x80000000ULL
#define STAR2LTE_PT_DRAM_HI    0x100000000ULL
#define STAR2LTE_PT_MAPPED_HI  0xBC800000ULL   // top of UEFI-mapped DRAM (safe to dereference)
#define STAR2LTE_PT_SECURE_LO  0xBC800000ULL
#define STAR2LTE_PT_TABLE_CAP  0x4000u
#ifndef STAR2LTE_PT_PREFLUSH_L2
#define STAR2LTE_PT_PREFLUSH_L2  0
#endif

STATIC UINT32  mStar2LtePtTables;
STATIC UINT32  mStar2LtePtLeaves;
STATIC UINT32  mStar2LtePtSusp;
STATIC UINT32  mStar2LtePtPreFlush;
STATIC UINT64  mStar2LtePtMinPa;
STATIC UINT64  mStar2LtePtMaxPa;

//
// Scan-time stale-walk recovery state (see the exception handler). While the kernel
// PT walk runs, winload is FROZEN (we call the scan synchronously from inside our
// ExitBootServices hook), yet some VAs fault DFSC=0x07 because winload freed an L3
// and rebuilt the region as a 2 MB block WITHOUT a full TLB invalidate. The handler
// flushes the single faulting VA and retries.
//
STATIC volatile BOOLEAN  mStar2LteScanActive   = FALSE;
STATIC volatile UINT32   mStar2LteScanRetry    = 0;
STATIC volatile UINT64   mStar2LteScanLastFar  = 0;
STATIC volatile UINT32   mStar2LteScanFaultLog = 0;
STATIC volatile UINT32   mStar2LteTlbiProbe    = 0;   // 0=none, 1=pre printed, 2=post printed

#if STAR2LTE_PT_PREFLUSH_L2
STATIC
VOID
Star2LtePreFlushKernelVa (
  IN UINT64  Va
  )
{
  UINT64  Page;

  Page = (Va >> 12) & 0x00000FFFFFFFFFFFULL;
  __asm__ __volatile__ ("tlbi vaae1, %0" :: "r" (Page) : "memory");
}
#endif

STATIC
VOID
Star2LteScanPtLevel (
  IN UINT64  TablePa,
  IN UINTN   Level,
  IN UINT64  VaBase
  )
{
  volatile UINT64  *Tbl;
  UINTN            Index;
  UINTN            Count;
  UINT64           Desc;
  UINT64           OutPa;
  UINT64           NextTbl;
  UINT64           EntryVa;
  UINTN            Shift;

  if (mStar2LtePtTables >= STAR2LTE_PT_TABLE_CAP) {
    return;
  }
  if ((TablePa < STAR2LTE_PT_DRAM_LO) || (TablePa >= STAR2LTE_PT_MAPPED_HI)) {
    return;   // never dereference a table page outside UEFI's identity map
  }

  mStar2LtePtTables++;
  Shift = 39 - (Level * 9);
  Count = (Level == 0) ? 256 : 512;   // T0SZ/T1SZ=17 => 256-entry top level
  Tbl   = (volatile UINT64 *)(UINTN)TablePa;   // inputs are already clean PAs
                                               // (TTBR1 root is 2 KB-aligned at +0x800)

  for (Index = 0; Index < Count; Index++) {
    Desc = Tbl[Index];
    if ((Desc & 0x1ULL) == 0) {
      continue;
    }
    EntryVa = VaBase + ((UINT64)Index << Shift);
#if STAR2LTE_PT_PREFLUSH_L2
    if ((Level == 2) && (mStar2LtePtPreFlush < 0x800u)) {
      Star2LtePreFlushKernelVa (EntryVa);
      mStar2LtePtPreFlush++;
    }
#endif

    if (((Desc & 0x3ULL) == 0x3ULL) && (Level < 3)) {
      NextTbl = Desc & 0x0000FFFFFFFFF000ULL;
      Star2LteScanPtLevel (NextTbl, Level + 1, EntryVa);
      continue;
    }

    OutPa = Desc & 0x0000FFFFFFFFF000ULL;
    mStar2LtePtLeaves++;
    if (OutPa < mStar2LtePtMinPa) {
      mStar2LtePtMinPa = OutPa;
    }
    if (OutPa > mStar2LtePtMaxPa) {
      mStar2LtePtMaxPa = OutPa;
    }
    //
    // Flag ONLY the genuinely RKP-protected physical bands (H-A suspects): Secure
    // DRAM [0xBC800000,0xC0000000), Secure PGTBL [0xE0000000,0xE1900000), and the
    // low "RKP kernel zone" [0x80094000,0x82900000) that resets on writes. Device
    // MMIO (framebuffer 0xCC000000, GIC 0x10101000) and normal conventional RAM
    // [0x90000000,0xBC800000) are EXPECTED maps and are NOT flagged — winload
    // already writes rkp(0xAF800000)/tima(0xB8000000) in the working boot, and
    // ext46 proved carving them regresses winload. If any =SUSP fires the kernel PT
    // maps a VA into a protected band = the uH/RKP tlbi-walk smoking gun.
    //
    if (((OutPa >= 0xBC800000ULL) && (OutPa < 0xC0000000ULL)) ||
        ((OutPa >= 0xE0000000ULL) && (OutPa < 0xE1900000ULL)) ||
        ((OutPa >= 0x80094000ULL) && (OutPa < 0x82900000ULL))) {
      if (mStar2LtePtSusp < 12) {
        BdsPramStr ("\n=SUSP l=");
        BdsPramHex ((UINT32)Level, 1);
        BdsPramStr (" va=");
        BdsPramHex64 (EntryVa, 16);
        BdsPramStr (" d=");
        BdsPramHex64 (Desc, 16);
      }
      mStar2LtePtSusp++;
    }
  }
}

STATIC
VOID
Star2LteScanKernelPt (
  VOID
  )
{
  UINT64  Root0;

  mStar2LtePtTables = 0;
  mStar2LtePtLeaves = 0;
  mStar2LtePtSusp   = 0;
  mStar2LtePtPreFlush = 0;
  mStar2LtePtMinPa  = 0xFFFFFFFFFFFFFFFFULL;
  mStar2LtePtMaxPa  = 0;

  BdsPramStr ("\n=EBST0 ");
  BdsPramHex64 (Star2LteReadTtbr0El1 (), 16);

  //
  // Scan the WINDOWS KERNEL PT root (0x907AA000), NOT the live TTBR0. At this hook
  // (right after ExitBootServices returns) the live TTBR0 is still WINLOAD's identity
  // map; winload installs the kernel TTBR only later, in OslArchTransferToKernel.
  // But the kernel PT is fully BUILT before EBS (no allocator exists post-EBS), so
  // 0x907AA000 is complete and readable here via winload's flat DRAM identity map.
  // The root is rock-stable at 0x907AA000 (=TTBR 0x907AA000 in 25+ captures). TTBR0
  // L0 (low half) is at 0x907AA000; the kernel's TTBR1 L0 (high half) shares the same
  // 4 KB page at +0x800 (T0SZ/T1SZ=17 => 2 KB top-level tables, matching the kernel's
  // `add x9,x8,#0x800`). This is the decisive H-A test: does the kernel PT map any VA
  // to an output PA in the secure/protected band [0xBC800000, 0x1_0000_0000)?
  //
  Root0 = 0x907AA000ULL;
  mStar2LteScanActive   = TRUE;
  mStar2LteScanRetry    = 0;
  mStar2LteScanLastFar  = 0;
  mStar2LteScanFaultLog = 0;
  __asm__ __volatile__ ("dsb ish" ::: "memory");
  Star2LteScanPtLevel (Root0, 0, 0x0ULL);
  Star2LteScanPtLevel (Root0 + 0x800ULL, 0, 0xFFFF800000000000ULL);
  __asm__ __volatile__ ("dsb ish; isb" ::: "memory");
  mStar2LteScanActive = FALSE;

  BdsPramStr ("\n=PTSCAN t=");
  BdsPramHex (mStar2LtePtTables, 6);
  BdsPramStr (" lf=");
  BdsPramHex (mStar2LtePtLeaves, 6);
  BdsPramStr (" sp=");
  BdsPramHex (mStar2LtePtSusp, 4);
  BdsPramStr (" pf=");
  BdsPramHex (mStar2LtePtPreFlush, 4);
  BdsPramStr (" min=");
  BdsPramHex64 (mStar2LtePtMinPa, 12);
  BdsPramStr (" max=");
  BdsPramHex64 (mStar2LtePtMaxPa, 12);
  BdsPramByte ('\n');
}

//
// On-demand page-fault healer. The pre-EBS DxeCore allocation storm faults on a
// firmware-pool VA (~0xbbc59xxx) whose containing 2 MB block has been split to an
// L3 table with that 4 KB page left INVALID (esr DFSC=0x07, level-3 translation
// fault) even though it sits inside identity-mapped WB DRAM. Rather than dead-loop,
// the exception handler calls this to install a valid L3 entry for the faulting
// page (identity, cloned attributes from a valid sibling in the same L3 table, but
// forced RW + AF), then resumes execution so the faulting instruction re-runs and
// succeeds. Only ever fires for DFSC=0x07 in DRAM (block already split => L3 table
// exists), so it never has to split a block itself.
//
#define STAR2LTE_HEAL_MAX  0x4000u
STATIC UINT32  mStar2LteHealCount = 0;

STATIC
BOOLEAN
Star2LteHealPage (
  IN UINT64  Va
  )
{
  UINT64           TableBase;
  UINT64           Desc;
  UINTN            Level;
  UINTN            Shift;
  UINT64           Idx;
  volatile UINT64  *Tbl;
  UINT64           CloneDesc;
  UINTN            i;
  UINT64           VaPage;

  TableBase = Star2LteReadTtbr0El1 () & 0x0000FFFFFFFFF000ULL;

  //
  // Descend L0..L2. Each must be a table descriptor (bits[1:0]==0b11); if we hit
  // an invalid or block descriptor there is no L3 table to poke, so bail (this
  // matches DFSC=0x07 only, where the L3 table already exists).
  //
  for (Level = 0; Level <= 2; Level++) {
    Shift = 39 - (Level * 9);
    Idx   = (Va >> Shift) & 0x1FFULL;
    Tbl   = (volatile UINT64 *)(UINTN)TableBase;
    Desc  = Tbl[Idx];
    if ((Desc & 0x3ULL) != 0x3ULL) {
      return FALSE;
    }
    TableBase = Desc & 0x0000FFFFFFFFF000ULL;
  }

  //
  // TableBase = L3 table for Va. Find any valid sibling page to clone the platform
  // memory attributes (MAIR index, shareability, NS) from — guaranteed correct for
  // this region without hardcoding the platform's MAIR layout.
  //
  Tbl       = (volatile UINT64 *)(UINTN)TableBase;
  CloneDesc = 0;
  for (i = 0; i < 512; i++) {
    if ((Tbl[i] & 0x3ULL) == 0x3ULL) {
      CloneDesc = Tbl[i];
      break;
    }
  }
  if (CloneDesc == 0) {
    return FALSE;
  }

  //
  // Sibling attribute/control bits + our page's PA (identity). Force RW (AP[2]=0),
  // access flag set, and a valid page descriptor.
  //
  Desc  = (CloneDesc & ~0x0000FFFFFFFFF000ULL) | (Va & 0x0000FFFFFFFFF000ULL);
  Desc &= ~(1ULL << 7);   // AP[2]=0 -> read/write at EL1
  Desc |=  (1ULL << 10);  // AF=1
  Desc  = (Desc & ~0x3ULL) | 0x3ULL;

  Idx        = (Va >> 12) & 0x1FFULL;
  Tbl[Idx]   = Desc;
  __asm__ __volatile__ ("dsb ishst" ::: "memory");

  VaPage = (Va >> 12) & 0x0000000FFFFFFFFFULL;
  __asm__ __volatile__ ("tlbi vaae1, %0" :: "r" (VaPage) : "memory");
  __asm__ __volatile__ ("dsb ish; isb" ::: "memory");
  return TRUE;
}

STATIC
UINT64
Star2LteReadSp (
  VOID
  )
{
  UINT64  Val;

  __asm__ __volatile__ ("mov %0, sp" : "=r" (Val));
  return Val;
}

STATIC
UINT64
Star2LteReadFp (
  VOID
  )
{
  UINT64  Val;

  __asm__ __volatile__ ("mov %0, x29" : "=r" (Val));
  return Val;
}

STATIC
BOOLEAN
Star2LteLooksLikeDramPtr (
  IN UINT64  Ptr
  )
{
  return (BOOLEAN)((Ptr >= 0x0000000090000000ULL) && (Ptr < 0x00000000BC800000ULL) && ((Ptr & 0x7ULL) == 0));
}

STATIC UINT64  mStar2LteLastFramePra;
STATIC UINT64  mStar2LteLastFramePfp;
STATIC BOOLEAN mStar2LteAfterFinalProbe;
STATIC BOOLEAN mStar2LteAfterFinalMapDumped;
STATIC UINT32  mStar2LteAfterFinalTraceCount;
STATIC UINT64  mStar2LteSwStepEnd;
STATIC UINT64  mStar2LteSwStepLow;
STATIC UINT64  mStar2LteSwStepHigh;
STATIC UINT32  mStar2LteSwStepCount;
STATIC UINT32  mStar2LteSwStepMax;
STATIC BOOLEAN mStar2LteKctxQuiet;
STATIC BOOLEAN mStar2LteNtosCntfrqPatched;
STATIC UINT64  mStar2LteKctxTtbr0;

#define STAR2LTE_BRK_COUNT  4u
#define STAR2LTE_NTOS_SCAN_BASE   0x0000000090000000ULL
#define STAR2LTE_NTOS_SCAN_LIMIT  0x00000000BC800000ULL
#define STAR2LTE_NTOS_MAX_IMAGE_SIZE  0x08000000u
#define STAR2LTE_PE_SIGNATURE     0x00004550u
#define STAR2LTE_PE_MACHINE_ARM64 0xAA64u
#define STAR2LTE_PE32_PLUS_MAGIC  0x020Bu

typedef struct {
  UINT64   Address;
  UINT32   Original;
  BOOLEAN  Armed;
  BOOLEAN  Software;
} STAR2LTE_BRK_PROBE;

typedef struct {
  UINT32  Offset;
  UINT32  Orig;
  UINT32  New;
} STAR2LTE_NTOS_WORD_PATCH;

typedef struct {
  UINT32  TextRva;
  UINT32  TextSize;
  UINT32  CaveRva;
  UINT32  EntryRva;
} STAR2LTE_NTOS_LAYOUT;

STATIC CONST STAR2LTE_NTOS_WORD_PATCH  mStar2LteNtosWordPatch[] = {
  { 0x2179E8u, 0xD53BE040u, 0xD53BE020u },
  { 0x23B184u, 0xD53BE04Du, 0xD53BE02Du },
  { 0x44B38Cu, 0xD53BE340u, 0xD53BE240u },
  { 0x44B66Cu, 0xD51BE341u, 0xD51BE241u },
  { 0x44B6F0u, 0xD51BE328u, 0xD51BE228u },
  { 0x26CDC0u, 0xD5381028u, 0xD2800008u },
  { 0x53FB08u, 0xD5381028u, 0xD2800008u },
  { 0x49EE8Cu, 0xD5181029u, 0xD503201Fu },
  // Skip ntoskrnl's PMU init callback; PMU EL0/EL1 sysregs can trap under Samsung EL2.
  { 0x44B7F0u, 0xD53B9C08u, 0x52800000u },
  { 0x44B7F4u, 0x92800509u, 0xD65F03C0u },
  { 0x99D72Cu, 0xD2800004u, 0xD503201Fu },
  { 0x99D730u, 0xD2800003u, 0xD503201Fu },
  { 0x99D734u, 0x528007C0u, 0xD503201Fu },
  { 0x99D738u, 0x97EBD252u, 0xD503201Fu },
  //
  // v16 SMP: present PSCI v0.2 to the Windows HAL so it enables CPU_ON.
  //
  // Samsung's EL3 monitor implements LEGACY PSCI 0.1 -- the live device tree
  // reads `compatible = "arm,psci"` (not arm,psci-0.2/1.0) and supplies explicit
  // ids cpu_on=0xC4000003, cpu_off=0x84000002, cpu_suspend=0xC4000001. A 0.1
  // monitor has NO PSCI_VERSION (0x84000000) function at all.
  //
  // The Windows HAL probes for PSCI_VERSION anyway. On a 0.1 monitor that probe
  // returns garbage, the HAL classifies PSCI as unsupported, never issues
  // CPU_ON, and Windows runs one processor no matter what the MADT enables. That
  // matches every measurement: MADT 8/8 enabled, FADT PSCI_COMPLIANT, CPU_ON
  // ok=7/7 from UEFI, yet NPACT=NPMAX=NPGRP=1.
  //
  // Rewriting the version-probe call site to return major=0/minor=2 selects a
  // genuine PSCI 0.2 capability table: CPU_ON is marked supported and, correctly
  // for this monitor, PSCI_FEATURES/SYSTEM_OFF/SYSTEM_RESET are not (unlike a 1.0
  // table, which would advertise features Samsung's EL3 does not implement).
  //
  // The site is a PC-relative BL, so its encoded word is load-address
  // independent, and the generic loop below only writes where *Insn == Orig --
  // on any other kernel build this entry is inert.
  //
  { 0x9CEFA0u, 0x97E0F638u, 0x52800040u },
};

STATIC STAR2LTE_BRK_PROBE  mStar2LteBrkProbe[STAR2LTE_BRK_COUNT];

STATIC
BOOLEAN
Star2LteLooksLikeCodePtr (
  IN UINT64  Address
  )
{
  return (BOOLEAN)((Address >= 0x0000000090000000ULL) && (Address < 0x00000000BC800000ULL) && ((Address & 0x3ULL) == 0));
}

STATIC
VOID
Star2LteFlushPatchedInstruction (
  IN UINT64  Address
  )
{
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Address, sizeof (UINT32));
  InvalidateInstructionCacheRange ((VOID *)(UINTN)Address, sizeof (UINT32));
  __asm__ __volatile__ ("dsb sy\nic iallu\ndsb sy\nisb" ::: "memory");
}

STATIC
UINT64
Star2LteFindBytes (
  IN UINT64       Base,
  IN UINTN        Size,
  IN CONST UINT8  *Needle,
  IN UINTN        NeedleSize
  )
{
  UINTN  Offset;

  if ((Needle == NULL) || (NeedleSize == 0) || (NeedleSize > Size)) {
    return 0;
  }

  for (Offset = 0; Offset <= (Size - NeedleSize); Offset++) {
    if (CompareMem ((CONST VOID *)(UINTN)(Base + Offset), Needle, NeedleSize) == 0) {
      return Base + Offset;
    }
  }

  return 0;
}

STATIC
BOOLEAN
Star2LteGetNtosLayout (
  IN  UINT64                 Base,
  IN  UINTN                  ImageSize,
  OUT STAR2LTE_NTOS_LAYOUT  *Layout
  )
{
  STATIC CONST CHAR8         NtosName[] = "ntoskrnl.exe";
  EFI_IMAGE_DOS_HEADER       *DosHeader;
  EFI_IMAGE_NT_HEADERS64     *NtHeader;
  EFI_IMAGE_EXPORT_DIRECTORY *ExportDirectory;
  EFI_IMAGE_SECTION_HEADER   *Section;
  UINT32                     ExportRva;
  UINT32                     NameRva;
  UINT32                     SectionAlignment;
  UINT32                     UsedSize;
  UINT64                     UsedEnd;
  UINT64                     SectionEnd;
  UINT64                     CaveRva;
  UINTN                      HeaderSize;
  UINTN                      Index;

  if ((Base == 0) || (Layout == NULL) || (ImageSize < EFI_PAGE_SIZE)) {
    return FALSE;
  }

  DosHeader = (EFI_IMAGE_DOS_HEADER *)(UINTN)Base;
  if ((DosHeader->e_magic != EFI_IMAGE_DOS_SIGNATURE) ||
      (DosHeader->e_lfanew < sizeof (EFI_IMAGE_DOS_HEADER)) ||
      ((UINTN)DosHeader->e_lfanew > ImageSize - sizeof (EFI_IMAGE_NT_HEADERS64)))
  {
    return FALSE;
  }

  NtHeader = (EFI_IMAGE_NT_HEADERS64 *)(UINTN)(Base + DosHeader->e_lfanew);
  if ((NtHeader->Signature != EFI_IMAGE_NT_SIGNATURE) ||
      (NtHeader->FileHeader.Machine != EFI_IMAGE_MACHINE_AARCH64) ||
      (NtHeader->OptionalHeader.Magic != EFI_IMAGE_NT_OPTIONAL_HDR64_MAGIC) ||
      (NtHeader->OptionalHeader.SizeOfImage > ImageSize) ||
      (NtHeader->OptionalHeader.NumberOfRvaAndSizes <= EFI_IMAGE_DIRECTORY_ENTRY_EXPORT))
  {
    return FALSE;
  }

  ExportRva = NtHeader->OptionalHeader.DataDirectory[EFI_IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
  if ((ExportRva == 0) ||
      ((UINTN)ExportRva > ImageSize - sizeof (EFI_IMAGE_EXPORT_DIRECTORY)))
  {
    return FALSE;
  }

  ExportDirectory = (EFI_IMAGE_EXPORT_DIRECTORY *)(UINTN)(Base + ExportRva);
  NameRva         = ExportDirectory->Name;
  if (((UINTN)NameRva > ImageSize - sizeof (NtosName)) ||
      (CompareMem ((CONST VOID *)(UINTN)(Base + NameRva), NtosName, sizeof (NtosName)) != 0))
  {
    return FALSE;
  }

  if ((NtHeader->FileHeader.NumberOfSections == 0) ||
      (NtHeader->FileHeader.NumberOfSections > 64))
  {
    return FALSE;
  }

  HeaderSize = (UINTN)DosHeader->e_lfanew +
               OFFSET_OF (EFI_IMAGE_NT_HEADERS64, OptionalHeader) +
               NtHeader->FileHeader.SizeOfOptionalHeader;
  if ((HeaderSize > ImageSize) ||
      ((UINTN)NtHeader->FileHeader.NumberOfSections >
       (ImageSize - HeaderSize) / sizeof (EFI_IMAGE_SECTION_HEADER)))
  {
    return FALSE;
  }

  SectionAlignment = NtHeader->OptionalHeader.SectionAlignment;
  if ((SectionAlignment < sizeof (UINT32)) ||
      (SectionAlignment > 0x10000u) ||
      ((SectionAlignment & (SectionAlignment - 1u)) != 0))
  {
    return FALSE;
  }

  Section = (EFI_IMAGE_SECTION_HEADER *)(UINTN)(Base + HeaderSize);
  for (Index = 0; Index < NtHeader->FileHeader.NumberOfSections; Index++, Section++) {
    if ((CompareMem (Section->Name, ".text", sizeof (".text")) != 0) ||
        ((Section->Characteristics & EFI_IMAGE_SCN_MEM_EXECUTE) == 0) ||
        ((Section->Characteristics & EFI_IMAGE_SCN_MEM_DISCARDABLE) != 0))
    {
      continue;
    }

    UsedSize = (Section->Misc.VirtualSize > Section->SizeOfRawData) ?
               Section->Misc.VirtualSize :
               Section->SizeOfRawData;
    UsedEnd = (UINT64)Section->VirtualAddress + UsedSize;
    if ((UsedEnd > ImageSize) ||
        ((UINT64)Section->VirtualAddress + Section->Misc.VirtualSize > ImageSize))
    {
      return FALSE;
    }

    SectionEnd = ALIGN_VALUE (UsedEnd, SectionAlignment);
    CaveRva    = ALIGN_VALUE (UsedEnd, STAR2LTE_NTOS_EPOCH_CAVE_OFFSET);
    if ((SectionEnd > ImageSize) ||
        (CaveRva + STAR2LTE_NTOS_DYNAMIC_CAVE_SIZE > SectionEnd))
    {
      return FALSE;
    }

    Layout->TextRva  = Section->VirtualAddress;
    Layout->TextSize = Section->Misc.VirtualSize;
    Layout->CaveRva  = (UINT32)CaveRva;
    Layout->EntryRva = NtHeader->OptionalHeader.AddressOfEntryPoint;
    return TRUE;
  }

  return FALSE;
}

STATIC
UINT32
Star2LtePatchNtosPsciMemProtect (
  IN UINT64  Base,
  IN UINTN   ImageSize
  )
{
  STATIC CONST UINT8  Pattern[] = {
    0xD5, 0x02, 0x00, 0x18, 0x03, 0x00, 0x80, 0xD2,
    0x02, 0x00, 0x80, 0xD2, 0x01, 0x00, 0x80, 0xD2,
  };
  UINT64           Match;
  UINT64           Patch;
  UINTN            ScanSize;
  volatile UINT32  *Insn;

  if (ImageSize < sizeof (Pattern)) {
    return 0;
  }

  ScanSize = (ImageSize < 0x01000000u) ? ImageSize : 0x01000000u;
  Match = Star2LteFindBytes (Base, ScanSize, Pattern, sizeof (Pattern));
  if (Match == 0) {
    return 0;
  }

  Patch = Match - (8u * sizeof (UINT32));
  if (!Star2LteLooksLikeCodePtr (Patch)) {
    return 0;
  }

  Insn = (volatile UINT32 *)(UINTN)Patch;
  if (*Insn == 0xD65F03C0u) {
    return 0;
  }

  *Insn = 0xD65F03C0u;
  Star2LteFlushPatchedInstruction (Patch);
  return 1;
}

STATIC
UINT32
Star2LtePatchNtosAtBase (
  IN  UINT64                       Base,
  IN  UINTN                        ImageSize,
  IN  BOOLEAN                      PhysicalCheck,
  IN  CONST STAR2LTE_NTOS_LAYOUT  *Layout,
  OUT UINT32                       *CntfrqPatched
  );

STATIC
UINT32
Star2LteBranchInstruction (
  IN UINT64  From,
  IN UINT64  To
  );

STATIC
VOID
Star2LtePatchNtosCntfrqInMemory (
  VOID
  );

STATIC
UINT32
Star2LtePatchNtosAtBase (
  IN  UINT64                       Base,
  IN  UINTN                        ImageSize,
  IN  BOOLEAN                      PhysicalCheck,
  IN  CONST STAR2LTE_NTOS_LAYOUT  *Layout,
  OUT UINT32                       *CntfrqPatched
  )
{
  STATIC CONST UINT32  CntfrqStub[] = {
    0xD2A03188u, // movz x8, #0x018c, lsl #16
    0xF2975008u, // movk x8, #0xba80
    0xD65F03C0u, // ret
  };
  BOOLEAN          StubReady;
  UINT32           Count;
  UINT32           CntfrqCount;
  UINT32           ExpectedBranch;
  UINT64           Cave;
  UINT64           Address;
  UINT64           ScanEnd;
  INT64            Delta;
  volatile UINT32  *Insn;
  UINTN            Index;

  if ((Base == 0) || (ImageSize < EFI_PAGE_SIZE) || (Layout == NULL)) {
    return 0;
  }

  Count = 0;
  CntfrqCount = 0;
  if (CntfrqPatched != NULL) {
    *CntfrqPatched = 0;
  }

  //
  // Use the first 12 bytes of the linker's zero-filled .text tail. This resolves
  // to 0x5A9C00 on 22621 and 0x549E00 on 19041, while remaining tied to PE
  // section geometry rather than either build's fixed RVAs.
  //
  Cave      = Base + Layout->CaveRva;
  Insn      = (volatile UINT32 *)(UINTN)Cave;
  StubReady = (BOOLEAN)((CompareMem ((CONST VOID *)(UINTN)Cave, CntfrqStub, sizeof (CntfrqStub)) == 0) ||
                        ((Insn[0] == 0) && (Insn[1] == 0) && (Insn[2] == 0)));

  if (StubReady &&
      (!PhysicalCheck ||
       Star2LteLooksLikeCodePtr (Cave + sizeof (CntfrqStub) - sizeof (UINT32))))
  {
    volatile UINT32  *CaveInsn;

    CaveInsn = (volatile UINT32 *)(UINTN)Cave;
    for (Index = 0; Index < ARRAY_SIZE (CntfrqStub); Index++) {
      CaveInsn[Index] = CntfrqStub[Index];
      Star2LteFlushPatchedInstruction (Cave + (Index * sizeof (UINT32)));
    }

    ScanEnd = (UINT64)Layout->TextRva + Layout->TextSize;
    for (Index = Layout->TextRva;
         ((UINT64)Index + sizeof (UINT32) <= ScanEnd) &&
         (Index + sizeof (UINT32) <= ImageSize);
         Index += sizeof (UINT32))
    {
      Address = Base + Index;
      Delta   = (INT64)Cave - (INT64)Address;
      if ((PhysicalCheck && !Star2LteLooksLikeCodePtr (Address)) ||
          ((Delta & 0x3) != 0) ||
          (Delta < -0x08000000LL) ||
          (Delta > 0x07FFFFFCLL))
      {
        continue;
      }

      Insn           = (volatile UINT32 *)(UINTN)Address;
      ExpectedBranch = Star2LteBranchInstruction (Address, Cave) | 0x80000000u;
      if (*Insn == 0xD53BE008u) { // mrs x8, cntfrq_el0
        *Insn = ExpectedBranch;
        Star2LteFlushPatchedInstruction (Address);
        CntfrqCount++;
      } else if (*Insn == ExpectedBranch) {
        CntfrqCount++;
      }
    }

    if (CntfrqCount != 0) {
      BdsPramStr ("\n=NTCFQ b=");
      BdsPramHex64 (Base, 16);
      BdsPramStr (" c=");
      BdsPramHex64 (Cave, 16);
      BdsPramStr (" n=");
      BdsPramHex (CntfrqCount, 2);
    }
    Count += CntfrqCount;
  }

  if (CntfrqPatched != NULL) {
    *CntfrqPatched = CntfrqCount;
  }

  Address = Base + 0x44AE64u;
  if ((0x44AE64u + (2u * sizeof (UINT32)) <= ImageSize) &&
      (!PhysicalCheck || Star2LteLooksLikeCodePtr (Address + 4u))) {
    Insn = (volatile UINT32 *)(UINTN)Address;
    if ((Insn[0] == 0xD53BE008u) && (Insn[1] == 0xD3407D09u)) {
      Insn[0] = 0xD2975009u;             // mov  x9, #0xba80
      Insn[1] = 0xF2A03189u;             // movk x9, #0x018c, lsl #16 (26000000)
      Star2LteFlushPatchedInstruction (Address);
      Star2LteFlushPatchedInstruction (Address + 4u);
      Count++;
    }
  }

  Address = Base + 0x44B1A8u;
  if ((0x44B1A8u + sizeof (UINT32) <= ImageSize) &&
      (!PhysicalCheck || Star2LteLooksLikeCodePtr (Address))) {
    Insn = (volatile UINT32 *)(UINTN)Address;
    if (*Insn == 0x54000060u) {          // b.eq +0x0c after comparing CNTFRQ with saved freq
      *Insn = 0x14000003u;               // b    +0x0c (force consistency check OK)
      Star2LteFlushPatchedInstruction (Address);
      Count++;
    }
  }

  Address = Base + 0x44B884u;
  if ((0x44B884u + (2u * sizeof (UINT32)) <= ImageSize) &&
      (!PhysicalCheck || Star2LteLooksLikeCodePtr (Address + 4u))) {
    Insn = (volatile UINT32 *)(UINTN)Address;
    if ((Insn[0] == 0xD53BE008u) && (Insn[1] == 0xD3407D09u)) {
      Insn[0] = 0xD2975009u;             // mov  x9, #0xba80
      Insn[1] = 0xF2A03189u;             // movk x9, #0x018c, lsl #16 (26000000)
      Star2LteFlushPatchedInstruction (Address);
      Star2LteFlushPatchedInstruction (Address + 4u);
      Count++;
    }
  }

  for (Index = 0; Index < sizeof (mStar2LteNtosWordPatch) / sizeof (mStar2LteNtosWordPatch[0]); Index++) {
    if (mStar2LteNtosWordPatch[Index].Offset + sizeof (UINT32) > ImageSize) {
      continue;
    }

    Address = Base + mStar2LteNtosWordPatch[Index].Offset;
    if (PhysicalCheck && !Star2LteLooksLikeCodePtr (Address)) {
      continue;
    }

    Insn = (volatile UINT32 *)(UINTN)Address;
    if (*Insn == mStar2LteNtosWordPatch[Index].Orig) {
      *Insn = mStar2LteNtosWordPatch[Index].New;
      Star2LteFlushPatchedInstruction (Address);
      Count++;
    }
  }

  Index = Star2LtePatchNtosPsciMemProtect (Base, ImageSize);
  Count += (UINT32)Index;

  BdsPramStr ("\n=NTCMP n=");
  BdsPramHex (Count - CntfrqCount, 2);

  return Count;
}

STATIC
BOOLEAN
Star2LteGetPeImageSize (
  IN  UINT64  Base,
  OUT UINTN   *ImageSize
  )
{
  UINT32  Lfanew;
  UINT32  SizeOfImage;
  UINT64  ImageEnd;

  if ((ImageSize == NULL) || !Star2LteLooksLikeDramPtr (Base)) {
    return FALSE;
  }

  if (*(volatile UINT16 *)(UINTN)Base != 0x5A4Du) {
    return FALSE;
  }

  Lfanew = *(volatile UINT32 *)(UINTN)(Base + 0x3Cu);
  if ((Lfanew < 0x40u) || (Lfanew > 0x1000u)) {
    return FALSE;
  }

  if (*(volatile UINT32 *)(UINTN)(Base + Lfanew) != STAR2LTE_PE_SIGNATURE) {
    return FALSE;
  }

  if (*(volatile UINT16 *)(UINTN)(Base + Lfanew + 0x04u) != STAR2LTE_PE_MACHINE_ARM64) {
    return FALSE;
  }

  if (*(volatile UINT16 *)(UINTN)(Base + Lfanew + 0x18u) != STAR2LTE_PE32_PLUS_MAGIC) {
    return FALSE;
  }

  SizeOfImage = *(volatile UINT32 *)(UINTN)(Base + Lfanew + 0x50u);
  if ((SizeOfImage < EFI_PAGE_SIZE) || (SizeOfImage > STAR2LTE_NTOS_MAX_IMAGE_SIZE)) {
    return FALSE;
  }

  ImageEnd = Base + ALIGN_VALUE ((UINTN)SizeOfImage, EFI_PAGE_SIZE);
  if ((ImageEnd <= Base) || (ImageEnd > STAR2LTE_NTOS_SCAN_LIMIT)) {
    return FALSE;
  }

  *ImageSize = ALIGN_VALUE ((UINTN)SizeOfImage, EFI_PAGE_SIZE);
  return TRUE;
}

STATIC
UINT32
Star2LtePatchNtosImagesInDram (
  OUT UINT32  *ImageCount,
  OUT UINT64  *TraceBase,
  OUT UINT32  *CntfrqPatched
  )
{
  STAR2LTE_NTOS_LAYOUT  Layout;
  UINT32  Count;
  UINT32  CntfrqCount;
  UINT32  Images;
  UINT32  Patched;
  UINT32  PatchedCntfrq;
  UINT64  Base;
  UINTN   ImageSize;

  Count  = 0;
  CntfrqCount = 0;
  Images = 0;
  if (TraceBase != NULL) {
    *TraceBase = 0;
  }
  if (CntfrqPatched != NULL) {
    *CntfrqPatched = 0;
  }

  for (Base = STAR2LTE_NTOS_SCAN_BASE; (Base + EFI_PAGE_SIZE) <= STAR2LTE_NTOS_SCAN_LIMIT; Base += EFI_PAGE_SIZE) {
    if (!Star2LteGetPeImageSize (Base, &ImageSize)) {
      continue;
    }

    Images++;
    if (!Star2LteGetNtosLayout (Base, ImageSize, &Layout)) {
      //
      // Warm boots can leave stale PE headers whose claimed image range
      // overlaps the live kernel.  Only skip a range after ntos validation.
      //
      continue;
    }

    if ((TraceBase != NULL) && (*TraceBase == 0)) {
      *TraceBase = Base;
    }

    BdsPramStr ("\n=NTID b=");
    BdsPramHex64 (Base, 16);
    BdsPramStr (" e=");
    BdsPramHex (Layout.EntryRva, 8);
    BdsPramStr (" c=");
    BdsPramHex (Layout.CaveRva, 8);

    PatchedCntfrq = 0;
    Patched = Star2LtePatchNtosAtBase (
                Base,
                ImageSize,
                TRUE,
                &Layout,
                &PatchedCntfrq
                );
    Count  += Patched;
    CntfrqCount += PatchedCntfrq;

    if (ImageSize > EFI_PAGE_SIZE) {
      Base += ImageSize - EFI_PAGE_SIZE;
    }
  }

  if (ImageCount != NULL) {
    *ImageCount = Images;
  }
  if (CntfrqPatched != NULL) {
    *CntfrqPatched = CntfrqCount;
  }

  return Count;
}

//
// Star2Lte (ext43): plant a persistent PROGRESS-LATCH hook at an arbitrary in-function
// ntoskrnl site. Overwrites the instruction at Base+SiteRva with a branch into a private
// slack cave; the cave writes a single 32-bit progress CODE to a FIXED word at LatchAddr
// (0xFED17F80: inside the mapped ring PAGE but ABOVE the 0x3F00 flood cap, so neither the
// firmware breadcrumb flood nor the watchdog reset destroys it), runs the displaced original
// instruction, then branches back to site+4. Unlike the ring-append marker hook, the latch
// survives the flood/reset so the NEXT boot's firmware can read+print it reliably. The
// displaced instruction MUST be non-PC-relative (pacibsp/mrs/ldr/mov/str) so it relocates
// safely. Only x3/x17 are touched and they are saved/restored. Returns 1 if planted, else 0.
//

//
// ext49 TRANSLATION-REGIME DUMP HOOK. Like Star2LteInstallNtosLatchHook (writes the 1-byte
// progress Code to the latch word) but also snapshots, at the TTBR reprogram site (0x4AC044,
// the `add x9,x8,#0x800` just before `msr ttbr1_el1,x9`):
//   LatchAddr+0x10 (0xFED17F90) = x8              (the new TTBR0 root winload is about to install)
//   LatchAddr+0x18 (0xFED17F98) = adr x3,.        (RUNTIME PC of the cave word: identity ~0xBA56xxxx
//                                                  => kernel runs identity-mapped; 0xFFFF.... => high VA)
//   LatchAddr+0x20 (0xFED17FA0) = TCR_EL1         (T0SZ/T1SZ/granule => VA size & starting level/index)
//   LatchAddr+0x28 (0xFED17FA8) = TTBR0_EL1        (the CURRENT TTBR0 base+ASID -- does the switch change the base?)
// ext49 proved the kernel runs at HIGH VA (=RPC 0xFFFFF800.., TTBR1 space) and the switch at 0x4AC044
// is an ASID install (base 0x907AA000/800 unchanged; =CTT1 already = x8+0x800). So [x8] index 0 = 0
// (ext48) is EXPECTED (TTBR0 = empty user half). ext50 captures the CURRENT TTBR0: if its base equals
// the new x8 base (0x907AA000) the switch only sets the ASID and the pram stays mapped, making the
// forward latch probes (0x14/0x15/0x16 after the msr/tlbi) reliable. The stub touches ONLY x3/x17
// (saved/restored); x8/x9 are preserved for the imminent msr ttbr0_el1,x8 / msr ttbr1_el1,x9. It reads
// NO memory off x8 (all sources are registers/sysregs) so it cannot fault. BDS prints the four words
// one boot late (=TTBR / =RPC / =TCR / =CTT0) next to the =KLAT read.
//
// ext67 restores this hook to dump-only. ext65's pre-flush completed once but, when combined with
// ASID-zero in ext66, it became the blocker at v13 before the ASID-zero instruction could execute.
// Keep the useful register snapshot but do not issue TLBI here; ext67 cleanly tests ASID=0 + skip.
//

//
// ext86: BRACKET the ORIGINAL barrier with a momentarily-zeroed ASID (supersedes ext84's
// ASID-zero cave-tlbi and ext85's root-swap, BOTH of which hung at v15).
//
// AIRTIGHT NEW DISCRIMINATOR (ext69 vs ext84/ext85): uH/RKP accepts the kernel's
// `tlbi vmalle1` ONLY when it executes at its LEGITIMATE address 0x4A54D0 AND the live
// TTBR ASID is 0. Evidence:
//   ext69 : forced ASID=0 in the kernel's own TTBR loads, then the ORIGINAL `bl 0x4A54D0`
//           ran the tlbi at 0x4A54D0 with ASID=0 -> COMPLETED, reached v1d.
//   ext84 : identical tlbi (ASID=0) but issued INLINE from the cave (a different PC) -> HUNG v15.
//   ext85 : tlbi from the cave with TTBR0 root-swapped                              -> HUNG v15.
// So relocating the tlbi into the cave is the bug: uH validates the trapped instruction's PC.
// ext69 proved the fix mechanism (legit-PC tlbi + ASID=0) but left the ASID permanently 0,
// which corrupted downstream init (it hung later at v1d and needed the =SKA7/=SK64/=SK42
// band-aids).
//
// ext86 fix: a cave stub at 0x4AC074 that (1) saves the live TTBR0/TTBR1 (real new ASID),
// (2) clears the ASID field in both (base unchanged), (3) `bl 0x4A54D0` -> runs the ORIGINAL
// barrier's `tlbi vmalle1` at its LEGITIMATE PC with ASID=0 (reproduces ext69's completing
// flush; vmalle1 invalidates ALL EL1&0 entries regardless of ASID, so the flush stays
// complete), then (4) RESTORES the real TTBR0/TTBR1 so the kernel's intended new-ASID context
// is live on return -> no downstream corruption, no band-aids. x30 is saved/restored because
// the inner `bl` clobbers it. Roots read live via mrs -> KASLR-safe. The inner bl target is
// PC-relative (alias-invariant), so it is correct at the stub's high-half execution VA.
// On `ret` it lands at 0x4AC078 (v16).
//   =KLAT v16 climbing past v1d..v23 => legit-PC tlbi with ASID=0 completes and the ASID
//     restore lets init proceed -> the core blocker is broken.
//   =KLAT still v15 => even the legit-PC ASID=0 tlbi now hangs (contradicts ext69) -> the
//     PC/ASID model is wrong; pivot (e.g. compare TCR/SCTLR/VBAR context uH may also gate on).
//

STATIC
VOID
Star2LtePatchNtosCntfrqInMemory (
  VOID
  )
{
  UINT32  Count;
  UINT32  CntfrqCount;
  UINT32  ImageCount;
  UINT64  TraceBase;

  if (mStar2LteNtosCntfrqPatched) {
    return;
  }

  TraceBase = 0;
  CntfrqCount = 0;
  Count = Star2LtePatchNtosImagesInDram (
            &ImageCount,
            &TraceBase,
            &CntfrqCount
            );

  BdsPramStr ("\n=NTPATCH n=");
  BdsPramHex (Count, 2);
  BdsPramStr (" cf=");
  BdsPramHex (CntfrqCount, 2);
  BdsPramStr (" i=");
  BdsPramHex (ImageCount, 2);
  if (TraceBase != 0) {
    BdsPramStr (" b=");
    BdsPramHex64 (TraceBase, 16);
  }



  if (CntfrqCount != 0) {
    mStar2LteNtosCntfrqPatched = TRUE;
  }

  if (TraceBase != 0) {


    //
    // DIAG (2026-07-04): plant a pass-through marker at ntoskrnl's entry point.
    // Entry branches to executable slack at the end of PAGELK, the cave writes
    // "=KE" to PRAM, restores clobbered registers, executes the displaced first
    // instruction, then branches back to entry+4. Unlike the old spin marker,
    // this lets the kernel continue if the marker write succeeds.
    //
  }
}

STATIC
UINT32
Star2LteBranchInstruction (
  IN UINT64  From,
  IN UINT64  To
  );

STATIC
VOID
Star2LteInstallTTraceSite (
  IN UINT64  SiteAddress,
  IN UINTN   SiteIdx
  )
{
  STATIC CONST UINT32  RecoveryResetStubTemplate[41] = {
    0xD2880011u, 0xF2BFDA31u, 0xB9400A21u, 0x91003222u, 0x8B010042u, 0x528007A3u,
    0x38001443u, 0x52800A83u, 0x38001443u, 0x52800643u, 0x38001443u, 0x528007A3u,
    0x38001443u, 0x52800A43u, 0x38001443u, 0x38001443u, 0xCB110042u, 0x51003042u,
    0xB9000A22u, 0xD5033F9Fu, 0xD2810101u, 0xF2A280C1u, 0x528ACF02u, 0x72A24682u,
    0xB9000022u, 0xD2810181u, 0xF2A280C1u, 0x528ACE82u, 0x72A24682u, 0xB9000022u,
    0xD2810201u, 0xF2A280C1u, 0x528001E2u, 0xB9000022u, 0xD5033F9Fu, 0xD2808001u,
    0xF2A280C1u, 0x52800022u, 0xB9000022u, 0xD5033F9Fu, 0x14000000u
  };
  //
  // ext45: the CNTFRQ scanner loop [26..48] below is DISABLED. Evidence across ext41..ext44
  // is a smoking gun: "=KJ/=X0/=X1/=X2" are ALWAYS captured but "=XP" (written by this stub
  // AFTER the scanner, immediately before "br x1") is NEVER present -> winload hangs INSIDE
  // this stub's module-list scanner (a faulting/looping walk over live module DllBases),
  // before it ever branches to the kernel. So the kernel never starts, which is exactly why
  // the persistent latch stayed at the firmware sentinel (=KLAT vE7). To confirm+unblock:
  // words [26..30] now write latch code 0xB1 to 0xFED17F80 and branch straight to [49]
  // (cache-maint + =XP + "br x1"), skipping the scanner entirely. Next boot's =KLAT then reads
  //   0xB1     => br x1 reached (scanner WAS the hang) but kernel didn't run our 0xBA368000
  //               hooks -> that copy is stale / kernel runs elsewhere.
  //   0x01..08 => kernel ran AND our entry/init hooks fired -> 0xBA368000 IS the live copy;
  //               scanner was the hang; real progress.
  //   0xE7     => br x1 still not reached -> hang is elsewhere (not the scanner).
  //
  STATIC CONST UINT32  T6JumpDumpStub[81] = {
    0xD2880009u, 0xF2BFDA29u, 0xB940092Au, 0x9100312Bu, 0x8B0A016Bu, 0x528007ACu,
    0x3800156Cu, 0x5280096Cu, 0x3800156Cu, 0x5280094Cu, 0x3800156Cu, 0xAA1E03E8u,
    0xAA0003EDu, 0x5280060Cu, 0x94000033u, 0xAA0103EDu, 0x5280062Cu, 0x94000030u,
    0xAA0203EDu, 0x5280064Cu, 0x9400002Du, 0xCB09016Au, 0x5100314Au, 0xB900092Au,
    0xD5033F9Fu, 0xD2800011u, 0xD28FF00Cu, 0xF2BFDA2Cu, 0x5280162Du, 0xB900018Du,
    0x14000013u, 0x72BA540Fu, 0xEB0400BFu, 0x54000200u, 0xF94018A6u, 0xB94040A7u,
    0xB4000166u, 0x34000147u, 0x8B0700C7u, 0xB94000D0u, 0x6B0E021Fu, 0x54000061u,
    0xB90000CFu, 0x91000631u, 0x910010C6u, 0xEB0700DFu, 0x54FFFF23u, 0xF94000A5u,
    0x17FFFFF0u, 0xD5033F9Fu, 0xD508751Fu, 0xD5033F9Fu, 0xD5033FDFu, 0xB940092Au,
    0x9100312Bu, 0x8B0A016Bu, 0xAA1103EDu, 0x52800A0Cu, 0x94000007u, 0xCB09016Au,
    0x5100314Au, 0xB900092Au, 0xD5033F9Fu, 0xAA0803FEu, 0xD61F0020u, 0x528007AEu,
    0x3800156Eu, 0x52800B0Eu, 0x3800156Eu, 0x3800156Cu, 0x5280020Eu, 0xD37CFDAFu,
    0xF10029FFu, 0x9100C1F0u, 0x9100DDF1u, 0x9A913210u, 0x38001570u, 0xD37CEDADu,
    0x710005CEu, 0x54FFFF01u, 0xD65F03C0u
  };
  UINTN            Wj;
  UINT64           CaveI;
  volatile UINT32  *Cave;
  volatile UINT32  *Site;
  UINT32           RecoveryResetStub[41];
  UINT32           TraceStub[20];

  Site  = (volatile UINT32 *)(UINTN)SiteAddress;
  CaveI = 0x0000000091489800ULL + (SiteIdx * 0x80u);

  if (SiteIdx == STAR2LTE_RECOVERY_RESET_TRACE_SITE) {
    CaveI = 0x0000000091489C00ULL;
    Cave  = (volatile UINT32 *)(UINTN)CaveI;
    for (Wj = 0; Wj < sizeof (RecoveryResetStubTemplate) / sizeof (RecoveryResetStubTemplate[0]); Wj++) {
      RecoveryResetStub[Wj] = RecoveryResetStubTemplate[Wj];
    }

    RecoveryResetStub[9] = 0x52800003u | (((UINT32)('0' + SiteIdx)) << 5); // movz w3, #'0'+idx

    for (Wj = 0; Wj < sizeof (RecoveryResetStub) / sizeof (RecoveryResetStub[0]); Wj++) {
      Cave[Wj] = RecoveryResetStub[Wj];
      Star2LteFlushPatchedInstruction (CaveI + (Wj * sizeof (UINT32)));
    }

    *Site = Star2LteBranchInstruction (SiteAddress, CaveI);
    Star2LteFlushPatchedInstruction (SiteAddress);
    return;
  }

  if (SiteIdx == 6u) {
    CaveI = 0x0000000091489C00ULL;
    Cave  = (volatile UINT32 *)(UINTN)CaveI;
    for (Wj = 0; Wj < sizeof (T6JumpDumpStub) / sizeof (T6JumpDumpStub[0]); Wj++) {
      Cave[Wj] = T6JumpDumpStub[Wj];
      Star2LteFlushPatchedInstruction (CaveI + (Wj * sizeof (UINT32)));
    }

    *Site = Star2LteBranchInstruction (SiteAddress, CaveI);
    Star2LteFlushPatchedInstruction (SiteAddress);
    return;
  }

  TraceStub[0]  = 0xA9BF0BE1u;                                    // stp  x1, x2, [sp, #-0x10]!
  TraceStub[1]  = 0xA9BF47E3u;                                    // stp  x3, x17, [sp, #-0x10]!
  TraceStub[2]  = 0xD2880011u;                                    // movz x17, #0x4000
  TraceStub[3]  = 0xF2BFDA31u;                                    // movk x17, #0xFED1, lsl #16
  TraceStub[4]  = 0xB9400A21u;                                    // ldr  w1, [x17, #8]
  TraceStub[5]  = 0x91003222u;                                    // add  x2, x17, #0xC
  TraceStub[6]  = 0x8B010042u;                                    // add  x2, x2, x1
  TraceStub[7]  = 0x528007A3u;                                    // movz w3, #0x3d ('=')
  TraceStub[8]  = 0x38001443u;                                    // strb w3, [x2], #1
  TraceStub[9]  = 0x52800A83u;                                    // movz w3, #0x54 ('T')
  TraceStub[10] = 0x38001443u;                                    // strb w3, [x2], #1
  TraceStub[11] = 0x52800003u | (((UINT32)('0' + SiteIdx)) << 5); // movz w3, #'0'+idx
  TraceStub[12] = 0x38001443u;                                    // strb w3, [x2], #1
  TraceStub[13] = 0xCB110042u;                                    // sub  x2, x2, x17
  TraceStub[14] = 0x51003042u;                                    // sub  w2, w2, #0xC
  TraceStub[15] = 0xB9000A22u;                                    // str  w2, [x17, #8]
  TraceStub[16] = 0xA8C147E3u;                                    // ldp  x3, x17, [sp], #0x10
  TraceStub[17] = 0xA8C10BE1u;                                    // ldp  x1, x2, [sp], #0x10
  TraceStub[18] = *Site;                                          // displaced original insn
  TraceStub[19] = Star2LteBranchInstruction (CaveI + (19u * sizeof (UINT32)), SiteAddress + sizeof (UINT32));

  Cave = (volatile UINT32 *)(UINTN)CaveI;
  for (Wj = 0; Wj < sizeof (TraceStub) / sizeof (TraceStub[0]); Wj++) {
    Cave[Wj] = TraceStub[Wj];
    Star2LteFlushPatchedInstruction (CaveI + (Wj * sizeof (UINT32)));
  }

  *Site = Star2LteBranchInstruction (SiteAddress, CaveI);
  Star2LteFlushPatchedInstruction (SiteAddress);
}

//
// ext#18: MMU-off PRAM page-table injector. ext#17 proved cave 0x91489000 and winload code are
// already mapped in winload's new TTBR0, but PRAM 0xFED14000 stops at L1[3] == 0. This stub runs
// in the same MMU-off window and installs L2/L3 chains for PRAM and the PMU reset page 0x14060000
// before winload enables the new tables. The four table pages are allocated/zeroed below and
// patched into literal slots at offsets 0x308/0x310/0x318/0x320. The 202-word body was assembled
// with LLVM (files\ttbr-pram-inject-stub.s); the install fn appends 'b 0x912d50a8' at end_stub
// (Cave[202]) after re-executing dsb+isb.
//
STATIC
VOID
Star2LteInstallTtbrDumpStub (
  VOID
  )
{
  STATIC EFI_PHYSICAL_ADDRESS  PramPtScratch;
  STATIC CONST UINT32          TtbrStub[202] = {
    0xAA1E03E8u, 0xD2880011u, 0xF2BFDA31u, 0xB9400A2Au, 0x9100322Bu, 0x8B0A016Bu,
    0x528007ACu, 0x3800156Cu, 0x52800A0Cu, 0x3800156Cu, 0x5280092Cu, 0x3800156Cu,
    0x940000B2u, 0x528007ACu, 0x3800156Cu, 0x52800A8Cu, 0x3800156Cu, 0x5280060Cu,
    0x3800156Cu, 0xAA0003E9u, 0x9400009Fu, 0x940000A9u, 0x528007ACu, 0x3800156Cu,
    0x52800A0Cu, 0x3800156Cu, 0x5280064Cu, 0x3800156Cu, 0x100014CDu, 0xF94001A6u,
    0xAA0603E9u, 0x94000094u, 0x9400009Eu, 0x528007ACu, 0x3800156Cu, 0x52800A0Cu,
    0x3800156Cu, 0x5280066Cu, 0x3800156Cu, 0x100013ADu, 0xF94001A7u, 0xAA0703E9u,
    0x94000089u, 0x94000093u, 0xD29E000Fu, 0xF2BFFFEFu, 0xF2C1FFEFu, 0x8A0F0010u,
    0xF940020Eu, 0x8A2F01C5u, 0x528007ACu, 0x3800156Cu, 0x52800A4Cu, 0x3800156Cu,
    0x5280060Cu, 0x3800156Cu, 0xAA0E03E9u, 0x9400007Au, 0x94000084u, 0xD29E000Fu,
    0xF2BFFFEFu, 0xF2C1FFEFu, 0x8A0F01D0u, 0xF9400E0Eu, 0x528007ACu, 0x3800156Cu,
    0x528009ECu, 0x3800156Cu, 0x5280062Cu, 0x3800156Cu, 0xAA0E03E9u, 0x9400006Cu,
    0x94000076u, 0xAA0500C9u, 0xF9000E09u, 0x528007ACu, 0x3800156Cu, 0x528009CCu,
    0x3800156Cu, 0x5280062Cu, 0x3800156Cu, 0x94000062u, 0x9400006Cu, 0xAA0500E9u,
    0xF907D8C9u, 0x528007ACu, 0x3800156Cu, 0x528009CCu, 0x3800156Cu, 0x5280064Cu,
    0x3800156Cu, 0x94000058u, 0x94000062u, 0xD2880009u, 0xF2BFDA29u, 0xAA050129u,
    0xF90450E9u, 0x528007ACu, 0x3800156Cu, 0x528009CCu, 0x3800156Cu, 0x5280066Cu,
    0x3800156Cu, 0x9400004Cu, 0x94000056u, 0x528007ACu, 0x3800156Cu, 0x52800A0Cu,
    0x3800156Cu, 0x5280068Cu, 0x3800156Cu, 0x10000AEDu, 0xF94001A6u, 0xAA0603E9u,
    0x94000041u, 0x9400004Bu, 0x528007ACu, 0x3800156Cu, 0x52800A0Cu, 0x3800156Cu,
    0x528006ACu, 0x3800156Cu, 0x100009CDu, 0xF94001A7u, 0xAA0703E9u, 0x94000036u,
    0x94000040u, 0xD29E000Fu, 0xF2BFFFEFu, 0xF2C1FFEFu, 0xF940020Eu, 0x528007ACu,
    0x3800156Cu, 0x528009ECu, 0x3800156Cu, 0x5280060Cu, 0x3800156Cu, 0xAA0E03E9u,
    0x94000029u, 0x94000033u, 0xAA0500C9u, 0xF9000209u, 0x528007ACu, 0x3800156Cu,
    0x528009ACu, 0x3800156Cu, 0x5280062Cu, 0x3800156Cu, 0x9400001Fu, 0x94000029u,
    0xAA0500E9u, 0xF90280C9u, 0x528007ACu, 0x3800156Cu, 0x528009ACu, 0x3800156Cu,
    0x5280064Cu, 0x3800156Cu, 0x94000015u, 0x9400001Fu, 0xD2800009u, 0xF2A280C9u,
    0xAA050129u, 0xF90180E9u, 0x528007ACu, 0x3800156Cu, 0x528009ACu, 0x3800156Cu,
    0x5280066Cu, 0x3800156Cu, 0x94000009u, 0x94000013u, 0xD5033F9Fu, 0xD5033FDFu,
    0x94000010u, 0xAA0803FEu, 0xD5033F9Fu, 0xD5033FDFu, 0x14000018u, 0x5280020Du,
    0x93C9F129u, 0x92400D2Au, 0xF100295Fu, 0x9100C14Fu, 0x91015D4Cu, 0x1A8CB1EFu,
    0x3800156Fu, 0x710005ADu, 0x54FFFF01u, 0xD65F03C0u, 0xCB110169u, 0x51003129u,
    0xB9000A29u, 0xD65F03C0u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u
  };
  UINT64           StubBase;
  volatile UINT32  *Cave;
  volatile UINT64  *Literal;
  EFI_PHYSICAL_ADDRESS Scratch;
  EFI_STATUS       Status;
  UINTN            Wk;

  StubBase = 0x0000000091489000ULL;   // cave page base (already RO-cleared for the T-slots)
  Cave     = (volatile UINT32 *)(UINTN)StubBase;

  if (PramPtScratch == 0) {
    Scratch = 0;
    Status  = gBS->AllocatePages (AllocateAnyPages, EfiRuntimeServicesData, 4, &Scratch);
    if (EFI_ERROR (Status)) {
      BdsPramStr ("\n=PIFAIL ");
      BdsPramHex ((UINT32)Status, 8);
      return;
    }
    PramPtScratch = Scratch;
  }

  ZeroMem ((VOID *)(UINTN)PramPtScratch, 4 * EFI_PAGE_SIZE);
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)PramPtScratch, 4 * EFI_PAGE_SIZE);

  for (Wk = 0; Wk < sizeof (TtbrStub) / sizeof (TtbrStub[0]); Wk++) {
    Cave[Wk] = TtbrStub[Wk];
    Star2LteFlushPatchedInstruction (StubBase + (Wk * sizeof (UINT32)));
  }

  Literal    = (volatile UINT64 *)(UINTN)(StubBase + 0x308u);
  Literal[0] = (UINT64)PramPtScratch;
  Literal[1] = (UINT64)(PramPtScratch + EFI_PAGE_SIZE);
  Literal[2] = (UINT64)(PramPtScratch + (2 * EFI_PAGE_SIZE));
  Literal[3] = (UINT64)(PramPtScratch + (3 * EFI_PAGE_SIZE));
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)(StubBase + 0x308u), 4 * sizeof (UINT64));
  BdsPramStr ("\n=PIS ");
  BdsPramHex64 ((UINT64)PramPtScratch, 16);

  //
  // end_stub (Wk == 202): rejoin the switch at 0x912d50a8 -- after the stub re-executes
  // displaced dsb+isb from 0x912d50a0/0x912d50a4. The stub's 'b end_stub' lands here.
  //
  Cave[Wk] = Star2LteBranchInstruction (StubBase + (Wk * sizeof (UINT32)), 0x00000000912D50A8ULL);
  Star2LteFlushPatchedInstruction (StubBase + (Wk * sizeof (UINT32)));
}

//
// ext#7: inject an MMU-ON 'tlbi vmalle1' at a chosen site via a detour. winload's real
// address-space switch (fn @0x912d5080, called by 'bl 0x180001080' @0x913c1668) flushes with
// 'tlbi vmalle1' while the MMU is OFF (between the SCTLR_EL1 disable/enable) -- the Samsung
// uH/RKP hyp @EL2 cannot emulate an MMU-off tlbi and HANGS. We NOP that MMU-off tlbi and
// re-issue the SAME flush here @0x913c166c (displaced insn = 'dsb sy', position-independent),
// right after the switch returns, with the MMU back ON. Self-test =STLBI1/2 proved MMU-on EL1
// tlbi is emulated fine.
//
// ext#9 (2026-07-06) H1-vs-H2 DISCRIMINATOR: ext#7 reached =T0 but not =T1 (0x913c1674), an
// ambiguity between H1 (the injected flush genuinely hangs) and H2 (flush worked but winload's
// post-switch tables no longer map PRAM @0xFED14000, so the T1 PRAM-write faults and we go
// blind). The passive screen can't tell them apart (winload doesn't repaint post-switch). So
// right AFTER the flush the stub PAINTS A WHITE BAND to the framebuffer (STAR2LTE_FB_BASE
// 0xCC000000, top ~364 rows), preserving x0-x3 (flags are dead here until the 'tst' @0x913c1690).
//   * screen shows the white band  => we executed PAST the injected flush (cave reachable, flush
//     did NOT hang, FB mapped) => H2 CONFIRMED = flush works, winload advanced -> breakthrough.
//   * screen stays frozen-logo     => inconclusive (H1, or FB/cave unmapped post-switch); the
//     fault would go to winload's own VBAR and PRAM is dead, so only a POSITIVE is decisive.
// Stub fits the 0x80-byte (32-word) cave slot with room to spare.
//

STATIC
VOID
Star2LtePatchWinloadTimers (
  VOID
  )
{
  STATIC BOOLEAN                 mDone;
  EFI_MEMORY_ATTRIBUTE_PROTOCOL  *MemAttr;
  EFI_STATUS                     Status;
  STATIC CONST UINT64            Sites[] = {
    //
    // Star2Lte (2026-07-06) T-TRACE EXTENSION #7 -- THE FIX: re-issue the switch flush MMU-ON.
    // ext#6 RESULTS (both decisive): (1) EL1 TLBI SELF-TEST in our UEFI PASSED (=STLBI0/1/2) => the
    // uH/RKP hyp EMULATES MMU-ON EL1 tlbi fine; it is NOT a blanket tlbi wall. (2) =T0=T1=T2 (T3
    // absent) => winload hangs INSIDE the page-walker (bl 0x913c1340). Refined model: winload's
    // switch (fn @0x912d5080) flushes with 'tlbi vmalle1' while MMU is OFF; the hyp can't emulate an
    // MMU-off tlbi and hangs. NOPing it (kept) skipped the ESSENTIAL flush -> stale TLB -> the walker
    // reads page tables through stale translations and faults. FIX this cycle:
    //   * keep the MMU-off switch tlbi @0x912d50c8 NOPed (hyp can't do it),
    //   * INJECT an MMU-ON 'tlbi vmalle1' INLINE in the switch leaf's tail @0x912d50e0 (ext#10). In
    //     ext#7-9 this was a cave detour @0x913c166c, but a full 'tlbi vmalle1' flushes the STALE TLB
    //     entry that mapped the cave itself, so the stub's very NEXT instruction fetch (still in the
    //     cave @0x91489xxx) misses winload's new tables and faults -> the flush appears to do nothing.
    //     The leaf tail (0x912d50e0/e4, a redundant 'adr x4;br x4' before 'ret') runs MMU-ON in
    //     winload's OWN transition code, which the new tables MUST map (winload returns through it),
    //   * UN-NOP the walker's MMU-on vale1 @0x913c1328 (removed from TlbiSites) -- it works & is needed.
    // loaded = objdump - 0xEED2C000. Sites execution-ordered:
    //   T0 0x913c165c  SENTINEL: right BEFORE "bl 0x912d5080" (switch)
    //   T1 0x913c1674  POST-FLUSH confirm: survived the injected MMU-on tlbi (add x22,x8,#0x8a0)
    //   T2 0x913c16f8  PRE page-walker: right before "bl 0x913c1340"
    //   T3 0x913c1708  POST page-walker: the walk returned (mov x23,#0xc)  <== KEY: fixed?
    //   T4 0x912f83c4  MM fn returned to caller (mov x0,x21)
    //   T5 0x912f84b8  caller: pre-final-transfer (before "bl 0x912d5000" final handoff)
    //   T6 0x912d500c  final branch to the kernel entry after switching to the kernel stack
    // NOTE (2026-07-06 ext#8 aborted): reducing Sites[] to T0-only to "go dark + watch screen" REGRESSED
    // the boot to a PRE-EBS DxeCore data-abort (winload's =AFP allocations shifted 0x1000 with the smaller
    // FD -> layout noise, not an H1/H2 signal). Restored the full 6-site ext#7 layout as the known-good
    // baseline (reaches =EBSOK + =T0). Post-switch observability (H1 vs H2) must NOT rely on removing
    // sites; use a live post-flush signal instead (framebuffer @0xCC000000 paint / on-screen).
    //
    // ext#13 RESULT (trace 191334): re-armed T1-T5 WITH the inline leaf flush -> only =T0
    // appeared, T1-T5 ALL absent. So winload's real post-switch tables do NOT map the cave
    // @0x91489xxx (the ext#7-9 inference holds for the inline flush too). Post-switch cave
    // breadcrumbs need map-injection. Reverted to T0-only (clean native post-switch baseline).
    // ext#14 adds a SAFE leaf-tail TTBR dump (pre-flush, stale-mapped cave) to capture winload's
    // NEW post-switch TTBR0/TTBR1 -- the base needed to design the injector (see leaf-tail block).
    0x00000000913C165CULL,   // T0 pre-switch sentinel (before "bl 0x912d5080")
    0x00000000913C1674ULL,   // T1 post-switch, after the transition returns
    0x00000000913C16F8ULL,   // T2 before winload's page-walker call
    0x00000000913C1708ULL,   // T3 after winload's page-walker returns
    0x00000000912F83C4ULL,   // T4 MM fn returned to caller
    0x00000000912F84B8ULL,   // T5 caller pre-final-transfer
    0x00000000912D500CULL,   // T6 final branch to the kernel entry
  };
  UINT64  CaveBase;
  UINTN   SiteIdx;

  if (mDone) {
    return;
  }
  mDone = TRUE;

  //
  // ---- EL1 TLBI SELF-TEST (ext#6) ------------------------------------------------
  // We run at EL1 (measured: =VBAR0 el=04). CORE QUESTION: does the Samsung uH/RKP hyp
  // @EL2 trap-and-HANG our EL1 TLBI (HCR_EL2.TTLB), or does it emulate+return? This runs
  // in OUR UEFI (no winload complexity) right before we patch/boot winload. BdsPramStr
  // writes to the PRAM ring IMMEDIATELY (dsb), so a marker that prints BEFORE a hanging
  // tlbi still survives the watchdog reset and is readable.
  //   =STLBI0 then hang (no =STLBI1) => our EL1 'tlbi vmalle1' HANGS => hyp wall confirmed
  //                                     for the whole-TLB form; winload's hang is the same.
  //   =STLBI0=STLBI1 then hang        => vmalle1 ok but per-VA 'tlbi vae1' hangs.
  //   =STLBI0=STLBI1=STLBI2           => EL1 TLBI works (hyp emulates) => winload's T1->T2
  //                                     hang is NOT a plain tlbi trap => rethink (stale-TLB
  //                                     from our NOPs, or RKP arms later via winload's hvc).
  BdsPramStr ("\n=STLBI0");
  __asm__ __volatile__ ("dsb sy\ntlbi vmalle1\ndsb sy\nisb" ::: "memory");
  BdsPramStr ("=STLBI1");
  {
    UINT64  TestVa = 0x00000000912E4000ULL;
    __asm__ __volatile__ ("dsb sy\ntlbi vae1, %0\ndsb sy\nisb" :: "r" (TestVa >> 12) : "memory");
  }
  BdsPramStr ("=STLBI2");

  //
  // ext55: PHYSICAL CNTFRQ_EL0 probe. RESULT (ext55 boot): =CFQ0=0 confirms the physical
  // register is 0 (the "empty CNTFRQ" errata is real). An EL1 `msr cntfrq_el0` then FAULTED
  // (=EXC ec=00, Undefined) -> uH forbids EL1 writes to CNTFRQ_EL0; we CANNOT program it from
  // EL1. Since Mu boots WinPE on BIT-17 under the SAME EL1 constraint (it can't set the
  // physical reg either) yet succeeds, uH does NOT depend on physical CNTFRQ -> the tlbi hang
  // is NOT a uH-timer-spin. So we now ONLY read (no write, which undefs and kills BDS):
  //   =CFQ0 = physical CNTFRQ_EL0 (0 on this device).
  //
  {
    UINT64  CFq;
    __asm__ __volatile__ ("mrs %0, cntfrq_el0" : "=r" (CFq));
    BdsPramStr ("\n=CFQ0 ");
    BdsPramHex64 (CFq, 16);
  }

  //
  // ext55: ASID-context TLBI probe. Disassembly (2026-07-07) shows the kernel's hanging
  // tlbi at 0x4A54E8 is a LOCAL `tlbi vmalle1` (0xD5088708) -- byte-identical to =STLBI0
  // above, which PASSES. The ONLY difference: the kernel issues it immediately after
  // `msr ttbr1_el1,x8` (0x4AC06C) installs a NON-ZERO ASID (0x4AC068 `orr x8,x9,x10,lsl#48`).
  // Reproduce exactly that here -- install a non-zero ASID in TTBR1_EL1 (BASE unchanged, so
  // uH's page-table CONTENT checks still pass), then issue the same local tlbi:
  //   =ATST2 present, =ATST3 ABSENT => the tlbi/dsb hangs specifically when a non-zero ASID
  //     is live -> KERNEL HANG REPRODUCED IN FIRMWARE (iterate fixes at firmware speed, no
  //     winload/kernel needed).
  //   =ATST3 present => a bare non-zero ASID is NOT the trigger -> the difference is the
  //     kernel's own page-table CONTENT (which RKP validates), not the ASID switch alone.
  //
  {
    UINT64  Ttbr1;
    __asm__ __volatile__ ("mrs %0, ttbr1_el1" : "=r" (Ttbr1));
    BdsPramStr ("\n=ATST1 ");
    BdsPramHex64 (Ttbr1, 16);
    __asm__ __volatile__ ("msr ttbr1_el1, %0\n\tisb" :: "r" (Ttbr1 | (1ULL << 48)) : "memory");
    BdsPramStr ("=ATST2");
    __asm__ __volatile__ ("dsb sy\n\ttlbi vmalle1\n\tdsb sy\n\tisb" ::: "memory");
    BdsPramStr ("=ATST3");
    __asm__ __volatile__ ("msr ttbr1_el1, %0\n\tisb\n\tdsb sy\n\ttlbi vmalle1\n\tdsb sy\n\tisb" :: "r" (Ttbr1) : "memory");
    BdsPramStr ("=ATST4");
  }

  //
  // ext57: FAITHFUL kernel-ASID TLBI reproduction. The =ATST test above set bit48 of
  // TTBR1_EL1, but UEFI's TCR_EL1.A1=0 sources the ASID from TTBR0 -- so that "ASID" was
  // INERT (effective ASID stayed 0), which is why =ATST completed. The captured kernel
  // TCR_EL1 = 0x15B5513511 has AS=1 (16-bit ASIDs) and A1=1 (ASID sourced FROM TTBR1). So
  // the kernel's `tlbi vmalle1` at 0x4A54E8 runs in a genuine non-zero-16-bit-ASID context
  // we never reproduced. Do it now: set AS=1 + A1=1, install a real 16-bit ASID in TTBR1
  // (BASE unchanged so uH's table-content checks still pass), then the same local tlbi.
  //   =UTCR                        => UEFI's own TCR_EL1 (confirm AS=0/A1=0 pre-change).
  //   =A16A present, =A16B ABSENT  => a LIVE 16-bit/A1=1 ASID makes the tlbi/dsb HANG in
  //                                   firmware => KERNEL HANG ROOT-CAUSED to ASID width/source
  //                                   (fix: force the kernel to AS=0 8-bit ASIDs, or a uH shim).
  //   =A16B present                => 16-bit A1=1 ASID is NOT the trigger => it is the kernel's
  //                                   own TTBR1 table CONTENT (base 0x907AA000), not ASID config.
  //   =A16A absent (hang after =UTCR) => uH traps the `msr tcr_el1` write itself.
  //
  {
    UINT64  Utcr, Uttbr1;
    __asm__ __volatile__ ("mrs %0, tcr_el1"   : "=r" (Utcr));
    __asm__ __volatile__ ("mrs %0, ttbr1_el1" : "=r" (Uttbr1));
    BdsPramStr ("\n=UTCR ");
    BdsPramHex64 (Utcr, 16);
    // AS = bit36 (16-bit ASID), A1 = bit22 (ASID from TTBR1). Match the kernel's regime.
    __asm__ __volatile__ ("msr tcr_el1, %0\n\tisb" :: "r" (Utcr | (1ULL << 36) | (1ULL << 22)) : "memory");
    __asm__ __volatile__ ("msr ttbr1_el1, %0\n\tisb" :: "r" (Uttbr1 | (1ULL << 48)) : "memory");
    BdsPramStr ("=A16A");
    __asm__ __volatile__ ("dsb sy\n\ttlbi vmalle1\n\tdsb sy\n\tisb" ::: "memory");
    BdsPramStr ("=A16B");
    // Restore the ASID source/width and TTBR1, then a clean flush.
    __asm__ __volatile__ ("msr ttbr1_el1, %0\n\tisb" :: "r" (Uttbr1) : "memory");
    __asm__ __volatile__ ("msr tcr_el1, %0\n\tisb\n\tdsb sy\n\ttlbi vmalle1\n\tdsb sy\n\tisb" :: "r" (Utcr) : "memory");
    BdsPramStr ("=A16C");
  }

  //
  // ext61: IPS self-test. Kernel TCR_EL1 = 0x15B5513511 has IPS=0b101 (48-bit PA);
  // UEFI's own TCR (under which =A16 passes) has IPS=0b010 (40-bit PA). This is the
  // ONE remaining TCR bit that is SAFE to flip live (IPS is a superset -- UEFI's own
  // <40-bit mappings stay valid, so the post-`msr` fetch survives) and that we have
  // NEVER reproduced. uH's trapped-tlbi emulation walks the guest tables using
  // TCR.IPS; if its walker mishandles IPS=48-bit it could stall the dsb -- our exact
  // symptom. Set IPS=101 + AS=1 + A1=1 + live 16-bit TTBR1 ASID, then the local tlbi.
  //   =IPS0 present, =IPS1 ABSENT => IPS=48-bit makes the tlbi HANG in firmware =>
  //                                  FIRMWARE REPRO of the kernel wall (huge -- lets us
  //                                  bisect a fix in the fast self-test loop).
  //   =IPS0=IPS1 present          => IPS is not the trigger either => the wall needs the
  //                                  kernel's actual TTBR CONTENT (only live at runtime).
  //
  {
    UINT64  Utcr, Uttbr1, Ktcr;
    __asm__ __volatile__ ("mrs %0, tcr_el1"   : "=r" (Utcr));
    __asm__ __volatile__ ("mrs %0, ttbr1_el1" : "=r" (Uttbr1));
    // Base = UEFI TCR; force IPS(34:32)=0b101, AS(36)=1, A1(22)=1. Leave T0SZ/granule.
    Ktcr = (Utcr & ~(7ULL << 32)) | (5ULL << 32) | (1ULL << 36) | (1ULL << 22);
    __asm__ __volatile__ ("msr tcr_el1, %0\n\tisb"   :: "r" (Ktcr) : "memory");
    __asm__ __volatile__ ("msr ttbr1_el1, %0\n\tisb" :: "r" (Uttbr1 | (1ULL << 48)) : "memory");
    BdsPramStr ("\n=IPS0");
    __asm__ __volatile__ ("dsb sy\n\ttlbi vmalle1\n\tdsb sy\n\tisb" ::: "memory");
    BdsPramStr ("=IPS1");
    __asm__ __volatile__ ("msr ttbr1_el1, %0\n\tisb" :: "r" (Uttbr1) : "memory");
    __asm__ __volatile__ ("msr tcr_el1, %0\n\tisb\n\tdsb sy\n\ttlbi vmalle1\n\tdsb sy\n\tisb" :: "r" (Utcr) : "memory");
    BdsPramStr ("=IPS2");
  }

  //
  // ext62: EPD1 / TTBR1-walk-enable self-test -- the MOST faithful pre-runtime repro yet.
  // The captured kernel TCR_EL1 = 0x15B5513511 has EPD1=0 (TTBR1 translation ENABLED) and
  // T1SZ=17, so the CPU/uH treat TTBR1_EL1 as a LIVE high-half translation base. EVERY prior
  // firmware tlbi test (=STLBI/=ATST/=A16/=IPS) ran under UEFI's TCR with EPD1=1 (TTBR1 walks
  // DISABLED) -- uH never had to look at TTBR1. But Samsung's RKP exists to protect the
  // kernel's HIGH-HALF page tables; on a trapped `tlbi vmalle1` it may RE-WALK / RE-VALIDATE
  // TTBR1. With EPD1=1 that path is skipped (=> all our tests complete); with EPD1=0 + a real
  // walkable TTBR1 it runs -- plausibly the exact stall. A `tlbi` never walks tables on bare
  // metal, so if enabling TTBR1 changes the outcome the culprit is uH's EMULATION, not the CPU.
  // Set EPD1=0, T1SZ=17, IPS=48, AS=1, A1=1, TTBR1=<UEFI root table>|ASID, keep UEFI T0SZ so
  // the running low-half code stays mapped; then the same local `tlbi vmalle1`.
  //   =EPD0 present, =EPD1 ABSENT, no =EXC => enabling TTBR1 walks HANGS the tlbi in firmware
  //                                           => FIRMWARE REPRO of the kernel wall (breakthrough:
  //                                              bisect the fix in the fast self-test loop).
  //   =EPD0 then =EXC                       => uH's walk faulted on our stand-in table (the walk
  //                                              IS reachable; retry with a kernel-shaped table).
  //   =EPD0=EPD1=EPD2 present               => even a live TTBR1 completes => the trigger is the
  //                                              exact CONTENT of the kernel's TTBR1 (runtime-only).
  //
  {
    UINT64  Utcr, Uttbr0, Uttbr1, Ktcr;
    __asm__ __volatile__ ("mrs %0, tcr_el1"   : "=r" (Utcr));
    __asm__ __volatile__ ("mrs %0, ttbr0_el1" : "=r" (Uttbr0));
    __asm__ __volatile__ ("mrs %0, ttbr1_el1" : "=r" (Uttbr1));
    // Kernel-like high-half regime; keep UEFI's T0SZ so the live low-half stays valid.
    Ktcr  = Utcr;
    Ktcr &= ~(7ULL    << 32); Ktcr |= (5ULL  << 32);   // IPS  = 0b101 (48-bit PA)
    Ktcr |=  (1ULL    << 36);                           // AS   = 1 (16-bit ASID)
    Ktcr |=  (1ULL    << 22);                           // A1   = 1 (ASID sourced from TTBR1)
    Ktcr &= ~(1ULL    << 23);                           // EPD1 = 0 (TTBR1 walks ENABLED)
    Ktcr &= ~(0x3FULL << 16); Ktcr |= (17ULL << 16);   // T1SZ = 17 (match kernel)
    // Point TTBR1 at UEFI's OWN root table (valid, walkable RAM) + a live 16-bit ASID.
    // Order: install TTBR1 while walks are still OFF, THEN enable them via the TCR write.
    __asm__ __volatile__ ("msr ttbr1_el1, %0\n\tisb" :: "r" ((Uttbr0 & 0xFFFFFFFFFFFFULL) | (1ULL << 48)) : "memory");
    __asm__ __volatile__ ("msr tcr_el1, %0\n\tisb"   :: "r" (Ktcr) : "memory");
    BdsPramStr ("\n=EPD0");
    __asm__ __volatile__ ("dsb sy\n\ttlbi vmalle1\n\tdsb sy\n\tisb" ::: "memory");
    BdsPramStr ("=EPD1");
    // Restore: disable TTBR1 walks (TCR first) BEFORE restoring the base, then a clean flush.
    __asm__ __volatile__ ("msr tcr_el1, %0\n\tisb" :: "r" (Utcr) : "memory");
    __asm__ __volatile__ ("msr ttbr1_el1, %0\n\tisb\n\tdsb sy\n\ttlbi vmalle1\n\tdsb sy\n\tisb" :: "r" (Uttbr1) : "memory");
    BdsPramStr ("=EPD2");
  }

  //
  // ext59: BROADCAST (inner-shareable) TLBI test. The LOCAL `tlbi vmalle1` completes in
  // firmware every way we try (=STLBI/=ATST/=A16). The kernel's hang site is a local
  // vmalle1 too -- BUT Samsung's uH/RKP may PROMOTE a trapped EL1 vmalle1 to a broadcast
  // `vmalle1is` (to keep every core's shadow-TLB coherent under RKP). A broadcast waits for
  // a DVM-sync ACK from the OTHER 7 cores; if our firmware leaves the secondaries in a
  // non-participating state (powered off / no coherency), that ACK never arrives and the
  // dsb-after-broadcast hangs -- exactly our symptom. Mu-Silicium runs WinPE with "only one
  // core", i.e. it avoids the multi-core DVM path. Issue the broadcast form ourselves:
  //   =BC0 present, =BC1 ABSENT => BROADCAST tlbi HANGS in firmware => the kernel hang is the
  //                                 DVM/secondary-core wall (fix: park secondaries coherently,
  //                                 or stop uH promoting -> single-core handoff like Mu).
  //   =BC0=BC1 present          => broadcast completes too => not a DVM wall; look elsewhere.
  //
  BdsPramStr ("\n=BC0");
  __asm__ __volatile__ ("dsb ish\n\ttlbi vmalle1is\n\tdsb ish\n\tisb" ::: "memory");
  BdsPramStr ("=BC1");

  //
  // PASSTHROUGH TRACER (the CNTVCT->CNTPCT patch is REMOVED - it broke winload BEFORE
  // 0x912e5724). At each "mov w20,w0" return point in winload's pre-EBS handoff fn,
  // plant a log-and-CONTINUE stub that writes "=T<n>" to the PRAM ring, preserving
  // x1/x2/x3/x17 so the probe can sit before calls that still need live arguments.
  // It then runs the displaced instruction and branches back. The
  // =T<n> trail shows how far winload gets in ONE boot; the last =T<n> is the last
  // handoff return point reached before the hang. Stubs live in a cave at 0x91489800.
  //
  MemAttr = NULL;
  Status  = gBS->LocateProtocol (&gEfiMemoryAttributeProtocolGuid, NULL, (VOID **)&MemAttr);
  if (EFI_ERROR (Status) || (MemAttr == NULL)) {
    BdsPramStr ("\n=WT noattr");
    return;
  }

  //
  // TEST: winload only reached 0x912e5724 when the software STEP WINDOW was enabled;
  // with it off it hangs BEFORE the handoff fn (no =T0). The step window's lasting
  // side effect is clearing RO on winload's 0x91312xxx handoff pages (0x91311000,
  // 0x91312000, 0x91313000). Reproduce JUST that (no single-stepping): if =T0 then
  // fires, this RO-clear is the real fix.
  //
  {
    UINT64  Rp;

    for (Rp = 0x00000000912E4000ULL; Rp <= 0x0000000091313000ULL; Rp += EFI_PAGE_SIZE) {
      MemAttr->ClearMemoryAttributes (MemAttr, Rp, EFI_PAGE_SIZE, EFI_MEMORY_RO);
    }
    BdsPramStr ("\n=ROCLR");
  }

  CaveBase = 0x0000000091489800ULL;
  Status   = MemAttr->ClearMemoryAttributes (MemAttr, CaveBase & ~(UINT64)EFI_PAGE_MASK, EFI_PAGE_SIZE, EFI_MEMORY_RO | EFI_MEMORY_RP);
  if (EFI_ERROR (Status)) {
    BdsPramStr ("\n=WT nocave");
    return;
  }

  for (SiteIdx = 0; SiteIdx < sizeof (Sites) / sizeof (Sites[0]); SiteIdx++) {
    if ((SiteIdx >= 4u) && (SiteIdx != 6u) && (SiteIdx != STAR2LTE_RECOVERY_RESET_TRACE_SITE)) {
      continue;
    }

    Status = MemAttr->ClearMemoryAttributes (MemAttr, Sites[SiteIdx] & ~(UINT64)EFI_PAGE_MASK, EFI_PAGE_SIZE, EFI_MEMORY_RO | EFI_MEMORY_RP);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Star2LteInstallTTraceSite (Sites[SiteIdx], SiteIdx);
  }

  //
  // ext#10: the MMU-ON flush is now injected INLINE in the switch leaf's tail (see the
  // TlbiSites block below), NOT as a cave detour @0x913c166c. A cave-based flush self-defeats:
  // 'tlbi vmalle1' invalidates the stale TLB entry mapping the cave, so the stub's next fetch
  // faults. 0x913c166c is left UNTOUCHED -- winload's own 'dsb sy' there becomes the flush's
  // completion barrier once the leaf returns.
  //

  {
    volatile UINT32  *BrkSite;
    volatile UINT32  *TlbiSite;
    volatile UINT32  *MicrovisorMode;
    UINTN            TlbiIdx;
    STATIC CONST UINT64  TlbiSites[] = {
      0x0000000091312144ULL,
      0x00000000913121C4ULL,
      0x00000000913C15B0ULL,   // final tlbi vmalle1 in the T2->T3 descriptor walker
      0x00000000912D50C8ULL,   // tlbi vmalle1 in winload's final EL1 MMU/TTBR switch (fn @0x912d5080)
                               // -- kept NOPed (it runs MMU-OFF; hyp can't emulate). NOPing it alone
                               // is what advanced winload PAST the switch (ckpt 006). ext#16 adds no
                               // leaf-tail flush; it only detours the switch ENTRY to dump the tables.
      // ext#7: 0x913C1328 (walker vale1) REMOVED -- it runs MMU-ON, the hyp emulates it, and the
      // page-walker needs it for its own break-before-make. NOPing it (ext#5) did not help anyway.
    };

    for (TlbiIdx = 0; TlbiIdx < sizeof (TlbiSites) / sizeof (TlbiSites[0]); TlbiIdx++) {
      // Ensure the tlbi site's page is writable (the bulk RO-clear above only covers
      // 0x912e4000..0x91313000; the switch-routine site @0x912d50c8 is outside that).
      MemAttr->ClearMemoryAttributes (MemAttr, TlbiSites[TlbiIdx] & ~(UINT64)EFI_PAGE_MASK, EFI_PAGE_SIZE, EFI_MEMORY_RO | EFI_MEMORY_RP);
      TlbiSite = (volatile UINT32 *)(UINTN)TlbiSites[TlbiIdx];
      if ((*TlbiSite == 0xD5088708u) || (*TlbiSite == 0xD508871Fu) || (*TlbiSite == 0xD50887A1u)) { // tlbi vmalle1 (Rt=8/31) or vale1,x1
        *TlbiSite = 0xD503201Fu;      // nop
        Star2LteFlushPatchedInstruction (TlbiSites[TlbiIdx]);
        BdsPramStr ("\n=WTLBI");
        BdsPramHex ((UINT32)TlbiIdx, 1);
      }
    }

    //
    // ext#16 PRE-SWITCH WALK/DUMP (supersedes the ext#15b leaf-tail walk + self-reset). The switch
    // routine (fn @0x912d5080, bl target @0x913c1668) does, in order:
    //   0x912d5080 mrs x4,SCTLR_EL1  (FIRST insn -- MMU still ON, old identity map still active)
    //   0x912d509c msr SCTLR_EL1,x4  (MMU OFF)   ... set TCR/TTBR0/TTBR1 ...
    //   0x912d50c8 tlbi vmalle1      (MMU OFF -- NOPed above; the hyp cannot emulate MMU-off tlbi)
    //   0x912d50d4 msr SCTLR_EL1,x3  (MMU ON) ; 0x912d50e0 adr x4 ; br x4 ; 0x912d50e8 ret
    //
    // We detour the FIRST instruction (0x912d5080) instead of the leaf tail, because there the MMU
    // is still ON with winload's OLD identity map (TTBR0=0xbc7ef000) and x0/x1/x2/x3 already hold
    // the NEW TTBR0/TTBR1/TCR/SCTLR -- so the stub reads winload's NEW tables RELIABLY (no post-
    // switch TLB-miss faults, which is what broke the ext#15 leaf-tail walk). The stub dumps
    // =T0/=T1/=Ct/=Ma/=S3 and walks cave/PRAM/winload-code, then re-execs the displaced
    // 'mrs x4,SCTLR_EL1' and 'b 0x912d5084' to let the switch run its natural course (winload
    // advances past the switch as before -- the MMU-off tlbi stays NOPed -- and hangs later; force
    // TWRP to read the ring). The leaf tail (0x912d50e0) is left PRISTINE this round (the pre-
    // ext#17 physical dump detours after winload has disabled the MMU, so TTBR page tables are
    // read by physical address instead of through winload's pre-switch VA map.
    //
    {
      volatile UINT32  *SwEntry;

      MemAttr->ClearMemoryAttributes (MemAttr, 0x00000000912D50A0ULL & ~(UINT64)EFI_PAGE_MASK, EFI_PAGE_SIZE, EFI_MEMORY_RO | EFI_MEMORY_RP);
      SwEntry = (volatile UINT32 *)(UINTN)0x00000000912D50A0ULL;
      if (*SwEntry == 0xD5033F9Fu) { // dsb sy (first insn after winload disables MMU)
        Star2LteInstallTtbrDumpStub ();
        *SwEntry = Star2LteBranchInstruction (0x00000000912D50A0ULL, 0x0000000091489000ULL); // b <MMU-off physical TTBR probe>
        Star2LteFlushPatchedInstruction (0x00000000912D50A0ULL);
        BdsPramStr ("\n=WPHY");
      } else {
        BdsPramStr ("\n=WPHY? ");
        BdsPramHex (*SwEntry, 8);
      }
    }

    MicrovisorMode = (volatile UINT32 *)(UINTN)0x0000000091504C80ULL;
    *MicrovisorMode = 1u;
    __asm__ __volatile__ ("dsb sy" ::: "memory");
    BdsPramStr ("\n=WMODE1");

    BrkSite = (volatile UINT32 *)(UINTN)0x00000000912E4644ULL;
    if (*BrkSite == 0xD43E0080u) { // brk #0xf004
      *BrkSite = 0x14000007u;      // b 0x912e4660
      Star2LteFlushPatchedInstruction (0x00000000912E4644ULL);
      BdsPramStr ("\n=WBRK");
    }

    BrkSite = (volatile UINT32 *)(UINTN)0x0000000091312A30ULL;
    if (*BrkSite == 0xD43E0060u) { // brk #0xf003
      *BrkSite = 0x14000001u;      // b 0x91312a34
      Star2LteFlushPatchedInstruction (0x0000000091312A30ULL);
      BdsPramStr ("\n=WBRK3");
    }
  }

  BdsPramStr ("\n=WTRACE n=00");
}

STATIC
UINT32
Star2LteBranchInstruction (
  IN UINT64  From,
  IN UINT64  To
  )
{
  return 0x14000000u | ((UINT32)(((INT64)To - (INT64)From) >> 2) & 0x03FFFFFFu);
}


STATIC
VOID
Star2LtePatchSoftwareBrkProbe (
  IN UINTN   Index,
  IN UINT64  Address
  )
{
  volatile UINT32  *Insn;

  if ((Index >= STAR2LTE_BRK_COUNT) || !Star2LteLooksLikeCodePtr (Address)) {
    return;
  }

  Insn = (volatile UINT32 *)(UINTN)Address;
  mStar2LteBrkProbe[Index].Address  = Address;
  mStar2LteBrkProbe[Index].Original = *Insn;
  mStar2LteBrkProbe[Index].Armed    = TRUE;
  mStar2LteBrkProbe[Index].Software = TRUE;
  *Insn = 0xD4200000u | ((0xA80u + (UINT32)Index) << 5);
  Star2LteFlushPatchedInstruction (Address);
}

STATIC
VOID
Star2LteRestoreSoftwareBrkProbe (
  IN UINTN  Index
  )
{
  volatile UINT32  *Insn;

  if ((Index >= STAR2LTE_BRK_COUNT) || !mStar2LteBrkProbe[Index].Armed || !mStar2LteBrkProbe[Index].Software) {
    return;
  }

  Insn = (volatile UINT32 *)(UINTN)mStar2LteBrkProbe[Index].Address;
  *Insn = mStar2LteBrkProbe[Index].Original;
  Star2LteFlushPatchedInstruction (mStar2LteBrkProbe[Index].Address);
  mStar2LteBrkProbe[Index].Armed = FALSE;
}

STATIC
INT64
Star2LteSignExtend (
  IN UINT64  Value,
  IN UINTN   Bits
  )
{
  UINT64  SignBit;

  SignBit = 1ULL << (Bits - 1u);
  return (INT64)((Value ^ SignBit) - SignBit);
}

STATIC
BOOLEAN
Star2LteDecodeBranchTarget (
  IN  UINT64  Address,
  IN  UINT32  Insn,
  OUT UINT64  *Target
  )
{
  INT64  Offset;

  if (Target == NULL) {
    return FALSE;
  }

  if ((Insn & 0x7C000000u) == 0x14000000u) {
    Offset  = Star2LteSignExtend (Insn & 0x03FFFFFFu, 26u) << 2;
    *Target = (UINT64)((INT64)Address + Offset);
    return TRUE;
  }

  if ((Insn & 0xFF000010u) == 0x54000000u) {
    Offset  = Star2LteSignExtend ((Insn >> 5) & 0x7FFFFu, 19u) << 2;
    *Target = (UINT64)((INT64)Address + Offset);
    return TRUE;
  }

  if ((Insn & 0x7E000000u) == 0x34000000u) {
    Offset  = Star2LteSignExtend ((Insn >> 5) & 0x7FFFFu, 19u) << 2;
    *Target = (UINT64)((INT64)Address + Offset);
    return TRUE;
  }

  if ((Insn & 0x7E000000u) == 0x36000000u) {
    Offset  = Star2LteSignExtend ((Insn >> 5) & 0x3FFFu, 14u) << 2;
    *Target = (UINT64)((INT64)Address + Offset);
    return TRUE;
  }

  return FALSE;
}


//
// Star2Lte (2026-08-08): Star2LteStartSoftwareStepWindow was removed here.
//
// It planted a BRK into winload's live code and single-stepped it through the
// exception vectors, which raced winload's own TTBR/VBAR switch and hung the
// boot roughly half the time. See the BOOT RELIABILITY FIX note at the
// after-final-map probe. The BRK helpers below are retained because the
// exception handler still references them.
//

//
// Star2Lte: recognise the AArch64 EL1 system-register writes that make up winload's
// kernel-transition context restore. Returns a short name when Insn is
// MSR {SCTLR,TTBR0,TTBR1,TCR,MAIR,VBAR}_EL1, Xt (Rt ignored), else NULL. These are the
// points past which BRK-based single-stepping is unsafe: the TTBR switch remaps the
// code page holding our planted breakpoint and the VBAR write replaces the exception
// vectors the stepper relies on.
//
STATIC
CONST CHAR8 *
Star2LteKernelContextMsrName (
  IN UINT32  Insn
  )
{
  switch (Insn & ~0x1Fu) {
    case 0xD5181000u: return "SCTLR_EL1";
    case 0xD5182000u: return "TTBR0_EL1";
    case 0xD5182020u: return "TTBR1_EL1";
    case 0xD5182040u: return "TCR_EL1";
    case 0xD518A200u: return "MAIR_EL1";
    case 0xD518C000u: return "VBAR_EL1";
    default:          return NULL;
  }
}

STATIC
BOOLEAN
Star2LteHandleBrkProbe (
  IN EFI_SYSTEM_CONTEXT_AARCH64  *Ctx
  )
{
  UINTN  Index;

  if (Ctx == NULL) {
    return FALSE;
  }

  for (Index = 0; Index < STAR2LTE_BRK_COUNT; Index++) {
    if (mStar2LteBrkProbe[Index].Armed && (mStar2LteBrkProbe[Index].Address == Ctx->ELR)) {
      UINT32  Original;
      UINT64  BranchTarget;
      UINT64  Next;
      UINT64  ReturnTarget;
      UINTN   RestoreIndex;

      Original = mStar2LteBrkProbe[Index].Original;
      for (RestoreIndex = 0; RestoreIndex < STAR2LTE_BRK_COUNT; RestoreIndex++) {
        Star2LteRestoreSoftwareBrkProbe (RestoreIndex);
      }
      __asm__ __volatile__ ("isb" ::: "memory");
      mStar2LteSwStepCount++;

      {
        CONST CHAR8  *KctxReg;
        BOOLEAN       Quiet;

        KctxReg = Star2LteKernelContextMsrName (Original);
        Quiet   = mStar2LteKctxQuiet;

        //
        // Normal per-instruction record. Suppressed once we enter winload's kernel-
        // context restore (quiet mode), so the six =KCTX register values below survive
        // the ~16 KB PRAM cap instead of being buried under ~24 more step records.
        //
        if (FALSE && !Quiet) {
          BdsPramStr ("\n=SWB n=");
          BdsPramHex (mStar2LteSwStepCount, 2);
          BdsPramStr (" p=");
          BdsPramHex ((UINT32)Ctx->ELR, 8);
          BdsPramStr (" o=");
          BdsPramHex (Original, 8);
        }

        if (KctxReg != NULL) {
          UINT32  Rt;
          UINT64  Val;

          //
          // winload is loading the TTBR/TCR/MAIR/SCTLR/VBAR values ntoskrnl will run
          // with. Log each target value, then go quiet for the rest of the restore.
          // STOP at the VBAR_EL1 write: after it winload owns the exception vectors, so
          // the next planted BRK would trap into winload's own handler and wedge the
          // handoff. All BRKs are restored above, so returning lets winload finish the
          // kernel branch natively.
          //
          Rt  = Original & 0x1Fu;
          Val = (Rt < 31u) ? ((volatile UINT64 *)Ctx)[Rt] : 0u;
          if ((Original & ~0x1Fu) == 0xD5182000u) { // MSR TTBR0_EL1
            mStar2LteKctxTtbr0 = Val;
          }
          if ((Original & ~0x1Fu) == 0xD518C000u) {
            BdsPramStr ("\n=KCTXV");
          }
          mStar2LteKctxQuiet = TRUE;

          if ((Original & ~0x1Fu) == 0xD518C000u) {
            mStar2LteSwStepCount = mStar2LteSwStepMax;
            return TRUE;
          }
        }

        if (Original == 0xD5088708u) {
          Ctx->ELR += sizeof (UINT32);
          if (!Quiet) {
            BdsPramStr (" skip=tlbi");
          }
        }
        if (!Quiet && ((mStar2LteSwStepCount <= 4u) || ((mStar2LteSwStepCount & 0x7u) == 0))) {
          BdsPramStr (" a=");
          BdsPramHex ((UINT32)Ctx->X0, 8);
          BdsPramStr (" b=");
          BdsPramHex ((UINT32)Ctx->X1, 8);
        }
      }
      Next = Ctx->ELR + sizeof (UINT32);
      if ((mStar2LteSwStepCount < mStar2LteSwStepMax) &&
          (Next < mStar2LteSwStepEnd) && (Next >= mStar2LteSwStepLow) && (Next < mStar2LteSwStepHigh)) {
        Star2LtePatchSoftwareBrkProbe (0u, Next);
      }
      if (((Original & 0xFFFFFC1Fu) == 0xD65F0000u) && (mStar2LteSwStepCount < mStar2LteSwStepMax)) {
        ReturnTarget = Ctx->LR;
        if (Star2LteLooksLikeCodePtr (ReturnTarget) && (ReturnTarget >= mStar2LteSwStepLow) &&
            (ReturnTarget < mStar2LteSwStepHigh)) {
          if (!mStar2LteKctxQuiet) {
            BdsPramStr (" r=");
            BdsPramHex ((UINT32)ReturnTarget, 8);
          }
          Star2LtePatchSoftwareBrkProbe (1u, ReturnTarget);
        }
      }
      if (Star2LteDecodeBranchTarget (Ctx->ELR, Original, &BranchTarget) &&
          (mStar2LteSwStepCount < mStar2LteSwStepMax) && (BranchTarget != Next) &&
          Star2LteLooksLikeCodePtr (BranchTarget) && (BranchTarget >= mStar2LteSwStepLow) &&
          (BranchTarget < mStar2LteSwStepHigh)) {
        if (!mStar2LteKctxQuiet) {
          BdsPramStr (" t=");
          BdsPramHex ((UINT32)BranchTarget, 8);
        }
        Star2LtePatchSoftwareBrkProbe (1u, BranchTarget);
      }
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
BOOLEAN
Star2LteAfterFinalTraceBegin (
  IN CONST CHAR8  *Tag
  )
{
  if (!mStar2LteAfterFinalProbe || (mStar2LteAfterFinalTraceCount >= 48u)) {
    return FALSE;
  }

  mStar2LteAfterFinalTraceCount++;
  BdsPramStr ("\n=");
  BdsPramStr (Tag);
  BdsPramStr (" n=");
  BdsPramHex (mStar2LteAfterFinalTraceCount, 2);
  return TRUE;
}

STATIC
VOID
Star2LteAfterFinalServiceTrace (
  IN CONST CHAR8  *Service,
  IN UINT64       Arg0,
  IN UINT64       Arg1
  )
{
  return;

  if (Star2LteAfterFinalTraceBegin ("AFS")) {
    BdsPramStr (" s=");
    BdsPramStr (Service);
    BdsPramStr (" a=");
    BdsPramHex64 (Arg0, 12);
    BdsPramStr (" b=");
    BdsPramHex64 (Arg1, 12);
    BdsPramStr (" ra=");
    BdsPramHex64 (STAR2LTE_CALLER_RA (), 16);
  }
}

STATIC
VOID
Star2LteDumpAfterFinalDescriptor (
  IN UINTN                  Index,
  IN EFI_MEMORY_DESCRIPTOR  *Desc
  )
{
  BdsPramStr ("\n=AFD i=");
  BdsPramHex ((UINT32)Index, 4);
  BdsPramStr (" t=");
  BdsPramHex ((UINT32)Desc->Type, 2);
  BdsPramStr (" p=");
  BdsPramHex64 (Desc->PhysicalStart, 10);
  BdsPramStr (" v=");
  BdsPramHex64 (Desc->VirtualStart, 10);
  BdsPramStr (" n=");
  BdsPramHex64 (Desc->NumberOfPages, 8);
  BdsPramStr (" a=");
  BdsPramHex64 (Desc->Attribute, 16);
}

STATIC
VOID
Star2LteTraceFrameChain (
  VOID
  )
{
  UINT64           Fp;
  UINT64           CallerFp;
  UINT64           CallerRa;
  volatile UINT64  *Frame;

  Fp = Star2LteReadFp ();
  BdsPramStr (" fp=");
  BdsPramHex64 (Fp, 16);
  if (!Star2LteLooksLikeDramPtr (Fp)) {
    return;
  }

  Frame    = (volatile UINT64 *)(UINTN)Fp;
  CallerFp = Frame[0];
  CallerRa = Frame[1];
  BdsPramStr (" cfp=");
  BdsPramHex64 (CallerFp, 16);
  BdsPramStr (" cra=");
  BdsPramHex64 (CallerRa, 16);
  if (!Star2LteLooksLikeDramPtr (CallerFp)) {
    return;
  }

  Frame = (volatile UINT64 *)(UINTN)CallerFp;
  mStar2LteLastFramePfp = Frame[0];
  mStar2LteLastFramePra = Frame[1];
  BdsPramStr (" pfp=");
  BdsPramHex64 (Frame[0], 16);
  BdsPramStr (" pra=");
  BdsPramHex64 (Frame[1], 16);
}

STATIC
VOID
Star2LtePramCpuSnapshot (
  IN CONST CHAR8  *Tag
  )
{
  BdsPramStr ("\n=");
  BdsPramStr (Tag);
  BdsPramStr (" vbar=");
  BdsPramHex64 (Star2LteReadVbarEl1 (), 16);
  BdsPramStr (" daif=");
  BdsPramHex ((UINT32)Star2LteReadDaif (), 4);
  BdsPramStr (" el=");
  BdsPramHex ((UINT32)Star2LteReadCurrentEl (), 2);
  BdsPramStr (" sp=");
  BdsPramHex64 (Star2LteReadSp (), 16);
  BdsPramStr (" mpidr=");
  BdsPramHex64 (Star2LteReadMpidrEl1 (), 16);
  BdsPramByte ('\n');
}

STATIC
VOID
EFIAPI
Star2LteCpuExceptionTrace (
  IN CONST EFI_EXCEPTION_TYPE  InterruptType,
  IN CONST EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  EFI_SYSTEM_CONTEXT_AARCH64  *Ctx;

  Ctx = SystemContext.SystemContextAArch64;
  if ((InterruptType == EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS) && Star2LteHandleBrkProbe (Ctx)) {
    return;
  }

  //
  // ON-DEMAND HEAL fast-path. For a translation fault (DFSC 0x04..0x07) on a DRAM
  // VA, install the missing page and RESUME instead of dead-looping. The very first
  // such fault falls through to emit the full =EXC record + =PTF walk (one-time
  // root-cause capture) and heals at the bottom; every later fault heals quietly
  // here so winload's pre-EBS allocation storm can rip through without flooding the
  // PRAM ring.
  //
  if (Ctx != NULL) {
    UINT32  Ec2   = (UINT32)((Ctx->ESR >> 26) & 0x3FU);
    UINT32  Dfsc2 = (UINT32)(Ctx->ESR & 0x3FU);
    UINT64  Far2  = Ctx->FAR;

    if (((Ec2 == 0x24U) || (Ec2 == 0x25U) || (Ec2 == 0x20U) || (Ec2 == 0x21U)) &&
        ((Dfsc2 & 0x3CU) == 0x04U) &&
        (Far2 >= 0x0000000080000000ULL) && (Far2 < 0x00000000BC800000ULL) &&
        (mStar2LteHealCount < STAR2LTE_HEAL_MAX) &&
        (mStar2LteHealCount > 0)) {
      if (Star2LteHealPage (Far2)) {
        mStar2LteHealCount++;
        if (mStar2LteHealCount <= 12) {
          BdsPramStr ("\n=PTH n=");
          BdsPramHex (mStar2LteHealCount, 4);
          BdsPramStr (" far=");
          BdsPramHex64 (Far2, 16);
          BdsPramByte ('\n');
        }
        return;
      }
    }
  }

  //
  // SCAN-TIME stale-walk recovery. While Star2LteScanKernelPt walks winload's FROZEN
  // page tables, some VAs (e.g. the kernel PT root 0x907AA000) fault DFSC=0x07 even
  // though a fresh software walk shows a valid L2 block: winload converted that region
  // from an L3-table mapping to a 2 MB block and freed the L3 WITHOUT a full TLB
  // invalidate, leaving a stale walk-cache entry that points at the freed L3. Flush
  // that single VA (tlbi vaae1 - NOT the trapped vmalle1) and re-execute the load; the
  // retry re-walks fresh memory and succeeds. Bounded per-VA; on give-up, skip the
  // load (dest reg <- 0 => scanner treats the entry as empty) so we can never hang.
  //
  if ((Ctx != NULL) && mStar2LteScanActive) {
    UINT32  EcS   = (UINT32)((Ctx->ESR >> 26) & 0x3FU);
    UINT32  DfscS = (UINT32)(Ctx->ESR & 0x3FU);
    UINT64  FarS  = Ctx->FAR;

    if (((EcS == 0x24U) || (EcS == 0x25U)) && ((DfscS & 0x3CU) == 0x04U)) {
      if (FarS != mStar2LteScanLastFar) {
        mStar2LteScanLastFar = FarS;
        mStar2LteScanRetry   = 0;
        if (mStar2LteScanFaultLog < 6) {
          mStar2LteScanFaultLog++;
          BdsPramStr ("\n=SRT far=");
          BdsPramHex64 (FarS, 16);
          BdsPramStr (" esr=");
          BdsPramHex64 (Ctx->ESR, 16);
          BdsPramByte ('\n');
        }
      }

      mStar2LteScanRetry++;
      if (mStar2LteScanRetry <= 6) {
        UINT64  Page = FarS >> 12;
        if (mStar2LteTlbiProbe == 0) {
          mStar2LteTlbiProbe = 1;
          BdsPramStr ("\n=TLBIPRE p=");
          BdsPramHex64 (Page, 12);
          BdsPramByte ('\n');
        }
        __asm__ __volatile__ ("dsb ish; tlbi vaae1, %0; dsb ish; isb" :: "r" (Page) : "memory");
        if (mStar2LteTlbiProbe == 1) {
          mStar2LteTlbiProbe = 2;
          BdsPramStr ("\n=TLBIOK\n");
        }
        return;
      }

      //
      // Persistent fault: the VA is genuinely unmapped. Skip the faulting load by
      // zeroing its destination register (Rt = insn[4:0]) and stepping over it.
      //
      {
        UINT32  Insn = *(volatile UINT32 *)(UINTN)Ctx->ELR;
        UINT32  Rt   = Insn & 0x1FU;
        if (Rt <= 30U) {
          ((UINT64 *)&Ctx->X0)[Rt] = 0;
        }
        Ctx->ELR += 4U;
      }
      return;
    }
  }

  BdsPramStr ("\n=EXC ty=");
  BdsPramHex ((UINT32)InterruptType, 2);
  BdsPramStr (" vbar=");
  BdsPramHex64 (Star2LteReadVbarEl1 (), 16);
  BdsPramStr (" daif=");
  BdsPramHex ((UINT32)Star2LteReadDaif (), 4);
  BdsPramStr (" el=");
  BdsPramHex ((UINT32)Star2LteReadCurrentEl (), 2);
  if (Ctx != NULL) {
    BdsPramStr (" elr=");
    BdsPramHex64 (Ctx->ELR, 16);
    BdsPramStr (" esr=");
    BdsPramHex64 (Ctx->ESR, 16);
    BdsPramStr (" ec=");
    BdsPramHex ((UINT32)(Ctx->ESR >> 26), 2);
    BdsPramStr (" far=");
    BdsPramHex64 (Ctx->FAR, 16);
    BdsPramStr (" spsr=");
    BdsPramHex64 (Ctx->SPSR, 16);
    BdsPramStr (" sp=");
    BdsPramHex64 (Ctx->SP, 16);
    BdsPramStr (" lr=");
    BdsPramHex64 (Ctx->LR, 16);
    BdsPramStr (" x0=");
    BdsPramHex64 (Ctx->X0, 16);
    BdsPramStr (" x1=");
    BdsPramHex64 (Ctx->X1, 16);
  }
  //
  // Confirm the GTDT counter-frame patch applied (values stamped by
  // Star2LteAcpiPlatformDxe into the fixed scratch word). gtdp=47544450 means it
  // ran; cc=CntControlBase, cr=CntReadBase (10040100), fq=CNTFID0 (018cba80).
  //
  {
    volatile UINT32  *Scratch = (volatile UINT32 *)(UINTN)0xFED13F00ULL;
    BdsPramStr (" gtdp=");
    BdsPramHex (Scratch[0], 8);
    BdsPramStr (" cc=");
    BdsPramHex (Scratch[1], 8);
    BdsPramStr (" cr=");
    BdsPramHex (Scratch[2], 8);
    BdsPramStr (" fq=");
    BdsPramHex (Scratch[3], 8);
  }
  //
  // Anchor for offline fault resolution: gBS points to DxeCore's internal
  // mBootServices global, so (elr - (gBS - offset of mBootServices)) via
  // DxeCore.map names the exact faulting function.
  //
  BdsPramStr (" gbs=");
  BdsPramHex64 ((UINT64)(UINTN)gBS, 16);
  BdsPramByte ('\n');

  //
  // DECISIVE PROBE: walk TTBR0_EL1 for the faulting address so we can see the
  // actual L0..L3 descriptors. The pre-EBS storm fault is a level-3 translation
  // fault (esr DFSC=0x07) on a firmware-pool VA (~0xbbc59xxx) that nominally sits
  // inside the identity-mapped DRAM (0x80000000..0xBC800000). This shows whether
  // the containing 2 MB block was split to an L3 table with the page left invalid,
  // and (compared with the early-BDS =PTB0 baseline) pins when it became invalid.
  //
  if (Ctx != NULL) {
    Star2LteDumpPtWalk ("PTF", Ctx->FAR);
  }

  //
  // FIRST healable fault reaches here after the one-time =EXC + =PTF capture:
  // install the missing DRAM page and resume so the faulting instruction re-runs
  // against a valid mapping. Only translation faults (DFSC 0x04..0x07) inside
  // identity-mapped DRAM are healed; anything else dead-loops with full context.
  //
  if (Ctx != NULL) {
    UINT32  Ec3   = (UINT32)((Ctx->ESR >> 26) & 0x3FU);
    UINT32  Dfsc3 = (UINT32)(Ctx->ESR & 0x3FU);
    UINT64  Far3  = Ctx->FAR;

    if (((Ec3 == 0x24U) || (Ec3 == 0x25U) || (Ec3 == 0x20U) || (Ec3 == 0x21U)) &&
        ((Dfsc3 & 0x3CU) == 0x04U) &&
        (Far3 >= 0x0000000080000000ULL) && (Far3 < 0x00000000BC800000ULL) &&
        (mStar2LteHealCount < STAR2LTE_HEAL_MAX) &&
        Star2LteHealPage (Far3)) {
      mStar2LteHealCount++;
      BdsPramStr ("\n=PTHEAL0 far=");
      BdsPramHex64 (Far3, 16);
      BdsPramByte ('\n');
      __asm__ __volatile__ ("dsb sy; isb" ::: "memory");
      return;
    }
  }

  __asm__ __volatile__ ("dsb sy" ::: "memory");

  for (;;) {
    __asm__ __volatile__ ("wfe" ::: "memory");
  }
}

STATIC
VOID
Star2LteInstallCpuExceptionTrace (
  VOID
  )
{
  EFI_CPU_ARCH_PROTOCOL  *Cpu;
  EFI_STATUS             Status;
  EFI_STATUS             SyncStatus;
  EFI_STATUS             SErrorStatus;
  EFI_STATUS             FiqStatus;

  Cpu = NULL;
  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (EFI_ERROR (Status) || (Cpu == NULL)) {
    BdsPramStr ("\n=EXCINST cpu=");
    BdsPramStatusLite (Status);
    BdsPramByte ('\n');
    return;
  }

  SyncStatus = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, Star2LteCpuExceptionTrace);
  SErrorStatus = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_SERROR, Star2LteCpuExceptionTrace);
  FiqStatus = Cpu->RegisterInterruptHandler (Cpu, EXCEPT_AARCH64_FIQ, Star2LteCpuExceptionTrace);
  BdsPramStr ("\n=EXCINST sync=");
  BdsPramStatusLite (SyncStatus);
  BdsPramStr (" serr=");
  BdsPramStatusLite (SErrorStatus);
  BdsPramStr (" fiq=");
  BdsPramStatusLite (FiqStatus);
  BdsPramStr (" irq=skip");
  BdsPramByte ('\n');
  Star2LtePramCpuSnapshot ("VBAR0");
}

STATIC
VOID
BdsPramStr16Ascii (
  IN CONST CHAR16  *Str,
  IN UINTN         MaxChars
  )
{
  UINTN  Index;

  for (Index = 0; (Index < MaxChars) && (Str[Index] != L'\0'); Index++) {
    BdsPramByte (((Str[Index] >= 0x20) && (Str[Index] <= 0x7E)) ? (UINT8)Str[Index] : (UINT8)'?');
  }
}


#define STAR2LTE_BOOT_TRACE_LIMIT      260u
#define STAR2LTE_PROTOCOL_TRACE_LIMIT  96u
#define STAR2LTE_VARIABLE_TRACE_LIMIT  96u
#define STAR2LTE_MEMORY_TRACE_LIMIT    64u
//
// Star2Lte: once the trace has reached this many events (i.e. we are in the late
// pre-ExitBootServices window, just before/after winload's final GetMemoryMap probe at
// ~bt0ab), trace ALL AllocatePool/AllocatePages calls incl. successes. Early boot stays
// quiet so we don't flood; this exposes exactly which boot service winload calls (and
// whether it returns) right after the probe, to localize the halt.
//
#define STAR2LTE_LATE_ALLOC_TRACE_EVENT 160u

STATIC BOOLEAN                   mStar2LteTraceInstalled;
STATIC UINT32                    mStar2LteTraceEvents;
STATIC UINT32                    mStar2LteTraceProtocolEvents;
STATIC UINT32                    mStar2LteTraceVariableEvents;
STATIC UINT32                    mStar2LteTraceMemoryEvents;
STATIC EFI_IMAGE_LOAD            mOrigLoadImage;
STATIC EFI_IMAGE_START           mOrigStartImage;
STATIC EFI_GET_MEMORY_MAP        mOrigGetMemoryMap;
STATIC EFI_EXIT_BOOT_SERVICES    mOrigExitBootServices;
STATIC EFI_RAISE_TPL             mOrigRaiseTpl;
STATIC EFI_RESTORE_TPL           mOrigRestoreTpl;
STATIC EFI_ALLOCATE_PAGES        mOrigAllocatePages;
STATIC EFI_FREE_PAGES            mOrigFreePages;
STATIC EFI_ALLOCATE_POOL         mOrigAllocatePool;
STATIC EFI_FREE_POOL             mOrigFreePool;
STATIC EFI_LOCATE_PROTOCOL       mOrigLocateProtocol;
STATIC EFI_LOCATE_HANDLE         mOrigLocateHandle;
STATIC EFI_LOCATE_HANDLE_BUFFER  mOrigLocateHandleBuffer;
STATIC EFI_OPEN_PROTOCOL         mOrigOpenProtocol;
STATIC EFI_HANDLE_PROTOCOL       mOrigHandleProtocol;
STATIC EFI_CREATE_EVENT          mOrigCreateEvent;
STATIC EFI_CLOSE_EVENT           mOrigCloseEvent;
STATIC EFI_SIGNAL_EVENT          mOrigSignalEvent;
STATIC EFI_WAIT_FOR_EVENT        mOrigWaitForEvent;
STATIC EFI_SET_TIMER             mOrigSetTimer;
STATIC EFI_STALL                 mOrigStall;
STATIC EFI_CHECK_EVENT           mOrigCheckEvent;
STATIC EFI_COPY_MEM              mOrigCopyMem;
STATIC EFI_SET_MEM               mOrigSetMem;
STATIC EFI_GET_VARIABLE          mOrigGetVariable;
STATIC EFI_SET_VARIABLE          mOrigSetVariable;
STATIC EFI_GET_NEXT_VARIABLE_NAME mOrigGetNextVariableName;
STATIC EFI_GET_TIME              mOrigGetTime;
STATIC EFI_CALCULATE_CRC32       mOrigCalculateCrc32;
STATIC EFI_INPUT_READ_KEY        mOrigConInReadKeyStroke;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL *mTraceConIn;
STATIC BOOLEAN                   mStar2LteTraceSetVariableEnabled;
STATIC UINT32                    mStar2LteSyntheticTimeSeconds;
STATIC UINT32                    mStar2LteSetTimerSeq;
STATIC UINT32                    mStar2LteStallSeq;
STATIC UINT32                    mStar2LteCheckSeq;
STATIC UINT32                    mStar2LteKeySeq;
STATIC UINT32                    mStar2LteLastFbSig;
STATIC UINT32                    mStar2LteFbPresentSeq;
STATIC UINT32                    mStar2LteBlockReadSeq;
STATIC UINT32                    mStar2LteAllocSeq;
STATIC UINT32                    mStar2LteReadHudHoldDepth;
#if STAR2LTE_DISK_ACTIVITY_INDICATOR
STATIC UINT32                    mStar2LteDiskBlinkSeq;
#endif
STATIC BOOLEAN                   mStar2LteBlockReadUseFallback;
STATIC BOOLEAN                   mStar2LteWinloadReserveActive;
STATIC BOOLEAN                   mStar2LteWinloadReserve1Active;
STATIC BOOLEAN                   mStar2LteWinloadReserve2Active;
STATIC EFI_EVENT                 mStar2LteBootWatchdogEvent;
STATIC BOOLEAN                   mStar2LteBootWatchdogArmed;
STATIC UINT32                    mStar2LteWdTicks;

#define STAR2LTE_WINLOAD_RESERVE_BASE  0x00000000BBC08000ULL
#define STAR2LTE_WINLOAD_RESERVE_PAGES 0x63u
#define STAR2LTE_WINLOAD_RESERVE2_BASE  0x00000000BB752000ULL
#define STAR2LTE_WINLOAD_RESERVE2_PAGES 0x2Du

#define STAR2LTE_FB_BASE          0x00000000CC000000ULL
#define STAR2LTE_FB_WIDTH         1440u
#define STAR2LTE_FB_HEIGHT        2960u
#define STAR2LTE_BLOCK_TRACE_MAX  64u
#define STAR2LTE_BLOCK_READ_CHUNK          0x01000000u
#define STAR2LTE_BLOCK_READ_FALLBACK_CHUNK 0x00400000u
#define STAR2LTE_BLOCK_READ_MIN_CHUNK      0x00010000u
#define STAR2LTE_HUD_READ_UPDATE_MASK      0x0u

typedef struct {
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  EFI_BLOCK_READ         ReadBlocks;
  UINT32                 Index;
} STAR2LTE_BLOCK_TRACE;

STATIC STAR2LTE_BLOCK_TRACE  mStar2LteBlockTrace[STAR2LTE_BLOCK_TRACE_MAX];
STATIC UINT32                mStar2LteBlockTraceCount;
STATIC
VOID
Star2LtePresent (
  VOID
  );

#if STAR2LTE_LIVE_HUD
#define STAR2LTE_HUD_SCALE  3u
#define STAR2LTE_HUD_CELL_W 18u
#define STAR2LTE_HUD_TEXT_W (32u * STAR2LTE_HUD_CELL_W)
#define STAR2LTE_HUD_X      ((STAR2LTE_FB_WIDTH - STAR2LTE_HUD_TEXT_W) / 2u)
#define STAR2LTE_HUD_Y      144u
#define STAR2LTE_HUD_BG_X   (STAR2LTE_HUD_X - 24u)
#define STAR2LTE_HUD_BG_Y   (STAR2LTE_HUD_Y - 10u)
#define STAR2LTE_HUD_BG_W   (STAR2LTE_HUD_TEXT_W + 48u)
#define STAR2LTE_HUD_BG_H   42u
#define STAR2LTE_HUD_BG     0xCC000000u
#define STAR2LTE_HUD_FG     0xFFFFFFFFu
#define STAR2LTE_HUD_ACCENT 0xFF00A0FFu
#define STAR2LTE_HUD_CLEAR_COLOR 0xFF000000u

STATIC BOOLEAN  mStar2LteHudFramebufferCleared;

STATIC CONST UINT8  mStar2LteHudFont[37][7] = {
  { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
  { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E },
  { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
  { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
  { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
  { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
  { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
  { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
  { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
  { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C },
  { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
  { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
  { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
  { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
  { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
  { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
  { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 }, { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
  { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
};

STATIC
VOID
Star2LteHudRect (
  IN UINTN   X,
  IN UINTN   Y,
  IN UINTN   W,
  IN UINTN   H,
  IN UINT32  Color
  )
{
  volatile UINT32  *Fb;
  UINTN            Row;
  UINTN            Col;

  Fb = (volatile UINT32 *)(UINTN)STAR2LTE_FB_BASE;
  for (Row = 0; Row < H; Row++) {
    for (Col = 0; Col < W; Col++) {
      Fb[(Y + Row) * STAR2LTE_FB_WIDTH + X + Col] = Color;
    }
  }
}

STATIC
UINTN
Star2LteHudGlyphIndex (
  IN CHAR8  Ch
  )
{
  if ((Ch >= '0') && (Ch <= '9')) {
    return (UINTN)(Ch - '0');
  }
  if ((Ch >= 'a') && (Ch <= 'z')) {
    Ch = (CHAR8)(Ch - 'a' + 'A');
  }
  if ((Ch >= 'A') && (Ch <= 'Z')) {
    return (UINTN)(Ch - 'A' + 10);
  }
  return 36u;
}

STATIC
VOID
Star2LteHudChar (
  IN UINTN  X,
  IN UINTN  Y,
  IN CHAR8  Ch
  )
{
  UINTN  Glyph;
  UINTN  Row;
  UINTN  Col;
  UINT8  Bits;

  Glyph = Star2LteHudGlyphIndex (Ch);
  for (Row = 0; Row < 7u; Row++) {
    Bits = mStar2LteHudFont[Glyph][Row];
    for (Col = 0; Col < 5u; Col++) {
      if ((Bits & (0x10u >> Col)) != 0) {
        Star2LteHudRect (X + Col * STAR2LTE_HUD_SCALE, Y + Row * STAR2LTE_HUD_SCALE, STAR2LTE_HUD_SCALE, STAR2LTE_HUD_SCALE, STAR2LTE_HUD_FG);
      }
    }
  }
}

STATIC
UINTN
Star2LteHudHex (
  IN UINTN   X,
  IN UINTN   Y,
  IN UINT32  Value,
  IN UINTN   Digits
  )
{
  CONST CHAR8  *Hex;

  Hex = "0123456789ABCDEF";
  while (Digits-- > 0) {
    Star2LteHudChar (X, Y, Hex[(Value >> (Digits * 4u)) & 0xFu]);
    X += STAR2LTE_HUD_CELL_W;
  }
  return X;
}

STATIC
VOID
Star2LteHudUpdate (
  IN CONST CHAR8  *Tag,
  IN UINT32       A,
  IN UINT32       B,
  IN UINT32       C
  )
{
  UINTN  X;
  UINTN  Index;

  if (!mStar2LteHudFramebufferCleared) {
    mStar2LteHudFramebufferCleared = TRUE;
    Star2LteHudRect (0, 0, STAR2LTE_FB_WIDTH, STAR2LTE_FB_HEIGHT, STAR2LTE_HUD_CLEAR_COLOR);
  }

  Star2LteHudRect (STAR2LTE_HUD_BG_X, STAR2LTE_HUD_BG_Y, STAR2LTE_HUD_BG_W, STAR2LTE_HUD_BG_H, STAR2LTE_HUD_BG);
  Star2LteHudRect (STAR2LTE_HUD_BG_X, STAR2LTE_HUD_BG_Y, 10u, STAR2LTE_HUD_BG_H, STAR2LTE_HUD_ACCENT);
  X = STAR2LTE_HUD_X;
  for (Index = 0; Index < 5u; Index++) {
    Star2LteHudChar (X, STAR2LTE_HUD_Y, ((Tag != NULL) && (Tag[Index] != '\0')) ? Tag[Index] : ' ');
    X += STAR2LTE_HUD_CELL_W;
  }
  X = Star2LteHudHex (X + STAR2LTE_HUD_CELL_W, STAR2LTE_HUD_Y, A, 8u);
  X = Star2LteHudHex (X + STAR2LTE_HUD_CELL_W, STAR2LTE_HUD_Y, B, 8u);
  Star2LteHudHex (X + STAR2LTE_HUD_CELL_W, STAR2LTE_HUD_Y, C, 8u);
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  Star2LtePresent ();
}
#else
#define Star2LteHudUpdate(Tag, A, B, C)  do { } while (0)
#endif

STATIC
VOID
Star2LteSetRecoveryBootReason (
  VOID
  )
{
  *(volatile UINT32 *)(UINTN)EXYNOS9810_PMU_INFORM2 = SEC_POWER_RESET;
  *(volatile UINT32 *)(UINTN)EXYNOS9810_PMU_INFORM3 = SEC_REBOOT_REASON_RECOVERY;
  *(volatile UINT32 *)(UINTN)EXYNOS9810_PMU_SYSIP_DAT0 = EXYNOS_INFORM_RECOVERY;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
VOID
Star2LteArmMiscRecoveryMessage (
  VOID
  )
{
  EFI_HANDLE                   *Handles;
  UINTN                        Count;
  UINTN                        Index;
  EFI_PARTITION_INFO_PROTOCOL  *PartInfo;
  EFI_BLOCK_IO_PROTOCOL        *BlockIo;
  EFI_STATUS                   Status;
  UINT8                        *Message;
  UINTN                        BufferSize;
  BOOLEAN                      Found;

  Handles = NULL;
  Count   = 0;
  Found   = FALSE;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    BdsPramStr ("\n=MISC nf ");
    BdsPramHex ((UINT32)Status, 8);
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    PartInfo = NULL;
    Status   = gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&PartInfo);
    if (EFI_ERROR (Status) || (PartInfo == NULL) || (PartInfo->Type != PARTITION_TYPE_GPT)) {
      continue;
    }

    if ((StrnCmp (PartInfo->Info.Gpt.PartitionName, L"MISC", 36) != 0) &&
        (StrnCmp (PartInfo->Info.Gpt.PartitionName, L"misc", 36) != 0)) {
      continue;
    }
    Found = TRUE;

    BlockIo = NULL;
    Status  = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status) || (BlockIo == NULL) || (BlockIo->Media == NULL) || !BlockIo->Media->MediaPresent) {
      BdsPramStr ("\n=MISC bio ");
      BdsPramHex ((UINT32)Status, 8);
      break;
    }

    BufferSize = 2048;
    if (BlockIo->Media->BlockSize > BufferSize) {
      BufferSize = BlockIo->Media->BlockSize;
    }
    if ((BufferSize % BlockIo->Media->BlockSize) != 0) {
      BufferSize = ((BufferSize + BlockIo->Media->BlockSize - 1) / BlockIo->Media->BlockSize) * BlockIo->Media->BlockSize;
    }

    Message = AllocateZeroPool (BufferSize);
    if (Message == NULL) {
      BdsPramStr ("\n=MISC mem");
      break;
    }

    CopyMem (Message, "boot-recovery", sizeof ("boot-recovery"));
    CopyMem (Message + 32, "recovery\n", sizeof ("recovery\n"));
    Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, 0, BufferSize, Message);
    if (!EFI_ERROR (Status)) {
      BlockIo->FlushBlocks (BlockIo);
    }
    FreePool (Message);

    BdsPramStr ("\n=MISC wr ");
    BdsPramByte (EFI_ERROR (Status) ? (UINT8)'E' : (UINT8)'S');
    BdsPramHex ((UINT32)(Status & 0xFFFFu), 4);
    BdsPramStr (" bs=");
    BdsPramHex (BlockIo->Media->BlockSize, 4);
    break;
  }

  if (!Found) {
    BdsPramStr ("\n=MISC none");
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }
}

//
// Forward declaration: the big-cluster probe is defined far below but is invoked
// from the boot-attempt watchdog's divert branch, so that the boot which runs the
// probe is also the boot that parks in TWRP with its PRAM ring intact.
//
#if STAR2LTE_BIG_PROBE_DIVERT
STATIC
VOID
Star2LteBigClusterProbe (
  VOID
  );
#endif

STATIC
VOID
Star2LteRecoveryReset (
  IN UINT8  Reason
  )
{
  BdsPramStr ("\n=RECOV r=");
  BdsPramByte (Reason);
  BdsPramByte ('\n');
  Star2LteSetRecoveryBootReason ();
  *(volatile UINT32 *)(UINTN)EXYNOS9810_PMU_SWRESET = 0x1u;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

//
// Star2Lte: evaluate the persistent boot-attempt counter. Called once, as early in BDS as
// there is a BDS at all. Returns normally on every boot that is still within budget; the
// divert path does not return.
//
STATIC
VOID
Star2LteBootAttemptWatchdog (
  VOID
  )
{
 #if STAR2LTE_BOOT_ATTEMPT_WATCHDOG
  volatile UINT32  *Magic;
  volatile UINT32  *Count;
  UINT32           Attempts;

  Magic = (volatile UINT32 *)(UINTN)STAR2LTE_BOOT_ATTEMPT_MAGIC_ADDR;
  Count = (volatile UINT32 *)(UINTN)STAR2LTE_BOOT_ATTEMPT_COUNT_ADDR;

  if (*Magic != STAR2LTE_BOOT_ATTEMPT_MAGIC) {
    //
    // Cold boot, or the word has never been ours. Claim it and start counting; do NOT
    // trust whatever happened to be in the count word.
    //
    *Magic   = STAR2LTE_BOOT_ATTEMPT_MAGIC;
    Attempts = 0u;
  } else {
    Attempts = *Count;
    //
    // A value past the limit means the word was corrupted rather than written by us --
    // restart the count instead of diverting on the strength of a wild read.
    //
    if (Attempts > STAR2LTE_BOOT_ATTEMPT_LIMIT) {
      Attempts = 0u;
    }
  }

  Attempts++;
  *Count = Attempts;
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  BdsPramStr ("\n=BATT n=");
  BdsPramHex (Attempts, 2);
  BdsPramStr (" lim=");
  BdsPramHex (STAR2LTE_BOOT_ATTEMPT_LIMIT, 2);

  if (Attempts < STAR2LTE_BOOT_ATTEMPT_LIMIT) {
    return;
  }

  //
  // Budget exhausted: the Windows side has failed to acknowledge this many consecutive
  // boots. Clear the count BEFORE diverting -- recovery is where the host picks the phone
  // up, and a stale count must not send the next deliberate Windows boot back to TWRP.
  //
  *Count = 0u;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  BdsPramStr (" =BATTO\n");
 #if STAR2LTE_BIG_PROBE_DIVERT
  //
  // Run the experiment HERE so its records land in the ring of the very boot that
  // parks in TWRP. The probe returns; the divert below is the proven TWRP path.
  //
  Star2LteBigClusterProbe ();
 #endif
  Star2LteRecoveryReset ((UINT8)'B');
 #endif
}

STATIC
VOID
Star2LteArmEbsHardwareWatchdog (
  VOID
  )
{
#if STAR2LTE_EBS_HW_WATCHDOG
  volatile UINT32  *Wdt;

  Wdt = (volatile UINT32 *)(UINTN)EXYNOS9810_WDT_BASE;
  Star2LteSetRecoveryBootReason ();
  *(volatile UINT32 *)(UINTN)EXYNOS9810_PMU_WDT_DISABLE &= ~STAR2LTE_WDT_CLUSTER0_RESET_BIT;
  *(volatile UINT32 *)(UINTN)EXYNOS9810_PMU_WDT_MASK_RESET &= ~STAR2LTE_WDT_CLUSTER0_RESET_BIT;
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  Wdt[STAR2LTE_WDT_WTCON / sizeof (UINT32)] = 0u;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  Wdt[STAR2LTE_WDT_WTDAT / sizeof (UINT32)] = STAR2LTE_EBS_HW_WATCHDOG_COUNT;
  Wdt[STAR2LTE_WDT_WTCNT / sizeof (UINT32)] = STAR2LTE_EBS_HW_WATCHDOG_COUNT;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  Wdt[STAR2LTE_WDT_WTCON / sizeof (UINT32)] = STAR2LTE_WDT_ENABLE_RESET;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  Star2LteSetRecoveryBootReason ();

  BdsPramStr ("\n=HWWDT c=");
  BdsPramHex (STAR2LTE_EBS_HW_WATCHDOG_COUNT, 8);
#endif
}

STATIC
VOID
Star2LteBootWatchdogClearRecoveryReason (
  VOID
  )
{
  *(volatile UINT32 *)(UINTN)EXYNOS9810_PMU_INFORM3 = 0;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
VOID
EFIAPI
Star2LteBootWatchdogTimeout (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (!mStar2LteBootWatchdogArmed) {
    return;
  }

  mStar2LteWdTicks++;
  BdsPramStr ("\n=HB");
  BdsPramHex (mStar2LteWdTicks, 4);
  if (mStar2LteWdTicks < (STAR2LTE_BOOT_WATCHDOG_TIMEOUT_S / STAR2LTE_WD_TICK_S)) {
    return;
  }

  BdsPramStr (" =WDTO t=");
  BdsPramHex ((UINT32)STAR2LTE_BOOT_WATCHDOG_TIMEOUT_S, 8);
  BdsPramByte ('\n');
  Star2LteRecoveryReset ((UINT8)'W');
}

STATIC
VOID
Star2LteBootWatchdogArm (
  VOID
  )
{
#if STAR2LTE_BOOT_WATCHDOG
  EFI_CREATE_EVENT  CreateEvent;
  EFI_SET_TIMER     SetTimer;
  EFI_STATUS        EventStatus;
  EFI_STATUS        TimerStatus;
  EFI_STATUS        WatchdogStatus;

  if ((gBS == NULL) || mStar2LteBootWatchdogArmed) {
    return;
  }

  Star2LteBootWatchdogClearRecoveryReason ();
  Star2LteSetRecoveryBootReason ();

  EventStatus = EFI_SUCCESS;
  if (mStar2LteBootWatchdogEvent == NULL) {
    CreateEvent = (mOrigCreateEvent != NULL) ? mOrigCreateEvent : gBS->CreateEvent;
    EventStatus = CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY, Star2LteBootWatchdogTimeout, NULL, &mStar2LteBootWatchdogEvent);
  }

  TimerStatus = EventStatus;
  if (!EFI_ERROR (EventStatus)) {
    SetTimer = (mOrigSetTimer != NULL) ? mOrigSetTimer : gBS->SetTimer;
    TimerStatus = SetTimer (mStar2LteBootWatchdogEvent, TimerPeriodic, (UINT64)STAR2LTE_WD_TICK_S * 10000000ULL);
  }

  WatchdogStatus = gBS->SetWatchdogTimer (STAR2LTE_BOOT_WATCHDOG_TIMEOUT_S + STAR2LTE_BOOT_WATCHDOG_RESET_GRACE_S, 0x0000, 0x00, NULL);
  mStar2LteBootWatchdogArmed = (BOOLEAN)(!EFI_ERROR (TimerStatus) || !EFI_ERROR (WatchdogStatus));
  if (!mStar2LteBootWatchdogArmed) {
    Star2LteBootWatchdogClearRecoveryReason ();
  }

  BdsPramStr ("\n=WDOG ev=");
  BdsPramByte (EFI_ERROR (EventStatus) ? (UINT8)'E' : (UINT8)'S');
  BdsPramHex ((UINT32)(EventStatus & 0xFFFFu), 4);
  BdsPramStr (" tm=");
  BdsPramByte (EFI_ERROR (TimerStatus) ? (UINT8)'E' : (UINT8)'S');
  BdsPramHex ((UINT32)(TimerStatus & 0xFFFFu), 4);
  BdsPramStr (" hw=");
  BdsPramByte (EFI_ERROR (WatchdogStatus) ? (UINT8)'E' : (UINT8)'S');
  BdsPramHex ((UINT32)(WatchdogStatus & 0xFFFFu), 4);
  BdsPramStr (" t=");
  BdsPramHex ((UINT32)STAR2LTE_BOOT_WATCHDOG_TIMEOUT_S, 8);
  BdsPramByte ('\n');
#endif
}

STATIC
VOID
Star2LteBootWatchdogDisarm (
  VOID
  )
{
#if STAR2LTE_BOOT_WATCHDOG
  EFI_SET_TIMER  SetTimer;

  if ((gBS == NULL) || !mStar2LteBootWatchdogArmed) {
    return;
  }

  mStar2LteBootWatchdogArmed = FALSE;
  if (mStar2LteBootWatchdogEvent != NULL) {
    SetTimer = (mOrigSetTimer != NULL) ? mOrigSetTimer : gBS->SetTimer;
    SetTimer (mStar2LteBootWatchdogEvent, TimerCancel, 0);
  }
  gBS->SetWatchdogTimer (0, 0x0000, 0x00, NULL);
  Star2LteBootWatchdogClearRecoveryReason ();
  BdsPramStr ("\n=WDOFF\n");
#endif
}

STATIC
VOID
Star2LteBootWatchdogPet (
  VOID
  )
{
#if STAR2LTE_BOOT_WATCHDOG
  if ((gBS == NULL) || !mStar2LteBootWatchdogArmed) {
    return;
  }

  mStar2LteWdTicks = 0;
  gBS->SetWatchdogTimer (STAR2LTE_BOOT_WATCHDOG_TIMEOUT_S + STAR2LTE_BOOT_WATCHDOG_RESET_GRACE_S, 0x0000, 0x00, NULL);
#endif
}

#define STAR2LTE_STAGE_BS_CALL  0xA2720000u

STATIC UINT32  mStar2LteBsSeq;

STATIC CONST EFI_GUID  mEfiHiiStringProtocolGuid = {
  0x0FD96974, 0x23AA, 0x4CDC, { 0xB9, 0xCB, 0x98, 0xD1, 0x77, 0x50, 0x32, 0x2A }
};

STATIC CONST EFI_GUID  mEfiUnicodeCollationProtocolGuid = {
  0xA4C751FC, 0x23AE, 0x4C3E, { 0x92, 0xE9, 0x49, 0x64, 0xCF, 0x63, 0xF3, 0x49 }
};

STATIC
VOID
Star2LteTraceBsOverlay (
  IN CONST CHAR8  *Name,
  IN BOOLEAN      Done,
  IN EFI_STATUS   Status,
  IN UINT32       ArgA,
  IN UINT32       ArgB,
  IN UINT32       ArgC
  )
{
  volatile UINT32  *Diag;
  volatile UINT8   *NameBuf;
  UINT64           StageFn;
  UINTN            Index;

  Diag = (volatile UINT32 *)(UINTN)0xFED13000ULL;
  Diag[430] = ++mStar2LteBsSeq;
  Diag[431] = (UINT32)(Status & 0xFFFFu);
  Diag[432] = ArgA;
  Diag[433] = ArgB;
  Diag[434] = ArgC;
  NameBuf = (volatile UINT8 *)(UINTN)(0xFED13000ULL + 436u * sizeof (UINT32));
  for (Index = 0; Index < 32; Index++) {
    NameBuf[Index] = (Name != NULL) ? (UINT8)Name[Index] : 0;
    if ((Name == NULL) || (Name[Index] == '\0')) {
      break;
    }
  }
  if (Index == 32) {
    NameBuf[31] = '\0';
  }
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  StageFn = *(volatile UINT64 *)(UINTN)0x00000000FED13100ULL;
  if (StageFn != 0) {
    ((VOID (EFIAPI *)(UINT32))(UINTN)StageFn) (STAR2LTE_STAGE_BS_CALL | (Done ? 1u : 0u));
  }
}

//
// DIAG RETENTION FIX: Star2LteTraceResetLog() removed.
//
// It wrote the 'DBGC' magic and zeroed the cursor at 0xFED14000, and its only
// caller ran at the end of PlatformBootManagerAfterConsole -- i.e. AFTER the
// "=BMREG" and "=WINFS" storage diagnostics had been written. It therefore
// destroyed exactly the evidence needed to explain "no bootable device" on
// every boot. BdsPramByte performs the same magic/cursor initialization lazily
// when the magic is absent, so no explicit reset is required.
//

STATIC
VOID
Star2LteTraceStatus (
  IN EFI_STATUS  Status
  )
{
  BdsPramByte (EFI_ERROR (Status) ? (UINT8)'E' : (UINT8)'S');
  BdsPramHex ((UINT32)(Status & 0xFFFFu), 4);
}

STATIC
VOID
Star2LteTraceSig (
  IN UINT32  Signature
  )
{
  BdsPramByte ((UINT8)(Signature & 0xFFu));
  BdsPramByte ((UINT8)((Signature >> 8) & 0xFFu));
  BdsPramByte ((UINT8)((Signature >> 16) & 0xFFu));
  BdsPramByte ((UINT8)((Signature >> 24) & 0xFFu));
}

//
// SMP DIAG (cycle S1): dump the MADT *as actually published in the XSDT at
// runtime*, rather than as built from Madt.aslc.
//
// Madt.aslc and Dsdt.asl have both been audited line by line and are correct:
// 8 GICC entries, AcpiProcessorUid 0..7, every one EFI_ACPI_6_3_GIC_ENABLED,
// MPIDRs 0x000-0x003 / 0x100-0x103, matched by CPU0..CPU7 ACPI0007 devices.
// Windows nevertheless registers exactly one logical processor. The remaining
// question is therefore not what the source says but what the firmware hands
// over: a second/stale APIC table winning in the XSDT, a truncated or
// mis-checksummed table, or a subtable walk that terminates early would all
// produce exactly this symptom and are invisible from the host.
//
// Emitted TWICE per boot - once at the end of AfterConsole and once at
// =EBSENTER, the last firmware instant before winload takes the machine - so
// that at least one copy survives the 0x3F00 circular-ring wrap.
//
#define STAR2LTE_MADT_SIG   SIGNATURE_32 ('A', 'P', 'I', 'C')
#define STAR2LTE_PPTT_SIG   SIGNATURE_32 ('P', 'P', 'T', 'T')
#define STAR2LTE_ACPI_GICC  0x0Bu

//
// Byte-wise readers: ACPI subtable fields are not naturally aligned (GICC
// GICRBaseAddress sits at +60), and reading the table through raw offsets also
// means this dump cannot be fooled by a struct definition that disagrees with
// the bytes actually in memory - which is the entire point of the exercise.
//
STATIC
UINT32
Star2LteRdU32 (
  IN CONST UINT8  *P
  )
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

STATIC
UINT64
Star2LteRdU64 (
  IN CONST UINT8  *P
  )
{
  return (UINT64)Star2LteRdU32 (P) | ((UINT64)Star2LteRdU32 (P + 4) << 32);
}

STATIC
VOID
Star2LteTraceDumpOneMadt (
  IN CONST UINT8  *Madt
  )
{
  UINT32  Length;
  UINT32  Offset;
  UINT32  GiccIndex;
  UINT32  EnabledCount;
  UINT32  Index;
  UINT8   Sum;

  Length = Star2LteRdU32 (Madt + 4);
  BdsPramStr (" len=");
  BdsPramHex (Length, 4);
  BdsPramStr (" rev=");
  BdsPramHex ((UINT32)Madt[8], 2);

  if ((Length < 44u) || (Length > 0x2000u)) {
    BdsPramStr (" BADLEN\n");
    return;
  }

  //
  // A correct table sums to zero. InstallAcpiTable recomputes the checksum, so
  // a non-zero sum here means the table was mutated after installation.
  //
  Sum = 0;
  for (Index = 0; Index < Length; Index++) {
    Sum = (UINT8)(Sum + Madt[Index]);
  }
  BdsPramStr (" ck=");
  BdsPramHex ((UINT32)Sum, 2);
  BdsPramStr (" mflags=");
  BdsPramHex (Star2LteRdU32 (Madt + 40), 8);

  GiccIndex    = 0;
  EnabledCount = 0;
  Offset       = 44u;
  while ((Offset + 2u) <= Length) {
    UINT8  Type;
    UINT8  SubLen;

    Type   = Madt[Offset];
    SubLen = Madt[Offset + 1u];
    if ((SubLen < 2u) || ((Offset + (UINT32)SubLen) > Length)) {
      //
      // Windows stops parsing here too: everything past this point is invisible
      // to the OS no matter how correct Madt.aslc is.
      //
      BdsPramStr ("\n BADSUB off=");
      BdsPramHex (Offset, 4);
      BdsPramStr (" t=");
      BdsPramHex ((UINT32)Type, 2);
      BdsPramStr (" l=");
      BdsPramHex ((UINT32)SubLen, 2);
      break;
    }

    if ((Type == STAR2LTE_ACPI_GICC) && (SubLen >= 80u)) {
      UINT32  Flags;

      Flags = Star2LteRdU32 (Madt + Offset + 12u);
      if ((Flags & 0x1u) != 0) {
        EnabledCount++;
      }

      BdsPramStr ("\n g");
      BdsPramHex (GiccIndex, 1);
      BdsPramStr (" l=");
      BdsPramHex ((UINT32)SubLen, 2);
      BdsPramStr (" u=");
      BdsPramHex (Star2LteRdU32 (Madt + Offset + 8u), 8);
      BdsPramStr (" f=");
      BdsPramHex (Flags, 8);
      BdsPramStr (" m=");
      BdsPramHex64 (Star2LteRdU64 (Madt + Offset + 68u), 16);
      BdsPramStr (" pi=");
      BdsPramHex (Star2LteRdU32 (Madt + Offset + 20u), 4);
      BdsPramStr (" e=");
      BdsPramHex ((UINT32)Madt[Offset + 76u], 2);
      GiccIndex++;
    } else {
      BdsPramStr ("\n s t=");
      BdsPramHex ((UINT32)Type, 2);
      BdsPramStr (" l=");
      BdsPramHex ((UINT32)SubLen, 2);
    }

    Offset += (UINT32)SubLen;
  }

  BdsPramStr ("\n gicc=");
  BdsPramHex (GiccIndex, 2);
  BdsPramStr (" en=");
  BdsPramHex (EnabledCount, 2);
  BdsPramStr (" used=");
  BdsPramHex (Offset, 4);
  BdsPramByte ('\n');
}

STATIC
VOID
Star2LteTraceDumpMadt (
  IN CONST CHAR8  *Tag
  )
{
  UINTN        Index;
  CONST UINT8  *Rsdp;
  CONST UINT8  *Xsdt;
  UINT64       XsdtAddress;
  UINT32       XsdtLength;
  UINTN        EntryCount;
  UINTN        EntryIndex;
  UINT32       MadtCount;
  UINT32       PpttCount;

  BdsPramStr ("\n=MADTDUMP ");
  BdsPramStr (Tag);
  BdsPramStr (" mpidr=");
  BdsPramHex64 (Star2LteReadMpidrEl1 (), 16);

  Rsdp = NULL;
  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &gEfiAcpi20TableGuid) &&
        (gST->ConfigurationTable[Index].VendorTable != NULL))
    {
      Rsdp = (CONST UINT8 *)gST->ConfigurationTable[Index].VendorTable;
      break;
    }
  }

  if (Rsdp == NULL) {
    BdsPramStr (" NORSDP\n");
    return;
  }

  XsdtAddress = Star2LteRdU64 (Rsdp + 24);
  if (XsdtAddress == 0) {
    BdsPramStr (" NOXSDT\n");
    return;
  }

  Xsdt       = (CONST UINT8 *)(UINTN)XsdtAddress;
  XsdtLength = Star2LteRdU32 (Xsdt + 4);
  if ((XsdtLength < 36u) || (XsdtLength > 0x1000u)) {
    BdsPramStr (" BADXSDT len=");
    BdsPramHex (XsdtLength, 8);
    BdsPramByte ('\n');
    return;
  }

  EntryCount = (XsdtLength - 36u) / sizeof (UINT64);
  BdsPramStr (" ec=");
  BdsPramHex ((UINT32)EntryCount, 2);

  MadtCount = 0;
  PpttCount = 0;
  for (EntryIndex = 0; EntryIndex < EntryCount; EntryIndex++) {
    CONST UINT8  *Table;
    UINT64       TableAddress;
    UINT32       Signature;

    TableAddress = Star2LteRdU64 (Xsdt + 36u + (EntryIndex * sizeof (UINT64)));
    if (TableAddress == 0) {
      continue;
    }

    Table     = (CONST UINT8 *)(UINTN)TableAddress;
    Signature = Star2LteRdU32 (Table);
    if (Signature == STAR2LTE_MADT_SIG) {
      MadtCount++;
      BdsPramStr ("\n=MADT#");
      BdsPramHex (MadtCount, 1);
      BdsPramStr (" @");
      BdsPramHex64 (TableAddress, 16);
      Star2LteTraceDumpOneMadt (Table);
    } else if (Signature == STAR2LTE_PPTT_SIG) {
      PpttCount++;
      BdsPramStr ("\n=PPTT len=");
      BdsPramHex (Star2LteRdU32 (Table + 4), 4);
      BdsPramStr (" rev=");
      BdsPramHex ((UINT32)Table[8], 2);
    }
  }

  //
  // n>1 would by itself explain the whole single-core symptom: Windows takes
  // the first APIC it finds in the XSDT, which need not be ours.
  //
  BdsPramStr ("\n=MADTDUMP end n=");
  BdsPramHex (MadtCount, 2);
  BdsPramStr (" pptt=");
  BdsPramHex (PpttCount, 2);
  BdsPramByte ('\n');
}

//
// The Windows UFS miniport maps 0xFED14000 and rewinds the ramoops start/size
// fields to 0 at init, so every firmware byte in the shared PRAM ring is gone
// before TWRP can read it. Write the trace to a raw GPT partition instead:
// DQMDBG (sda17), which neither Windows nor TWRP touches.
//
VOID
Star2LteWriteEvidencePartition (
  IN CONST CHAR8  *Tag
  )
{
  EFI_HANDLE                   *Handles;
  UINTN                        Count;
  UINTN                        Index;
  EFI_PARTITION_INFO_PROTOCOL  *PartInfo;
  EFI_BLOCK_IO_PROTOCOL        *BlockIo;
  EFI_STATUS                   Status;
  UINT8                        *Message;
  UINTN                        BufferSize;
  UINT32                       WrittenLen;
  BOOLEAN                      Found;

  STATIC BOOLEAN  EvidWritten = FALSE;

  if (EvidWritten) {
    return;
  }

  Handles    = NULL;
  Count      = 0;
  Found      = FALSE;
  WrittenLen = 0;
  Status     = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    BdsPramStr ("\n=EVID nf ");
    BdsPramHex ((UINT32)Status, 8);
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    PartInfo = NULL;
    Status   = gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&PartInfo);
    if (EFI_ERROR (Status) || (PartInfo == NULL) || (PartInfo->Type != PARTITION_TYPE_GPT)) {
      continue;
    }

    if ((StrnCmp (PartInfo->Info.Gpt.PartitionName, L"DQMDBG", 36) != 0) &&
        (StrnCmp (PartInfo->Info.Gpt.PartitionName, L"dqmdbg", 36) != 0)) {
      continue;
    }
    Found = TRUE;

    BlockIo = NULL;
    Status  = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status) || (BlockIo == NULL) || (BlockIo->Media == NULL) || !BlockIo->Media->MediaPresent) {
      BdsPramStr ("\n=EVID bio ");
      BdsPramHex ((UINT32)Status, 8);
      break;
    }

    BufferSize = 65536;
    if (BlockIo->Media->BlockSize > BufferSize) {
      BufferSize = BlockIo->Media->BlockSize;
    }
    if ((BufferSize % BlockIo->Media->BlockSize) != 0) {
      BufferSize = ((BufferSize + BlockIo->Media->BlockSize - 1) / BlockIo->Media->BlockSize) * BlockIo->Media->BlockSize;
    }

    Message = AllocateZeroPool (BufferSize);
    if (Message == NULL) {
      BdsPramStr ("\n=EVID mem");
      break;
    }

    //
    // Magic first so a stale partition can never be mistaken for a fresh dump,
    // then tee every BdsPramByte into the buffer while the dump runs. Detach
    // before emitting the status line so the status is not captured itself.
    //
    CopyMem (Message, "STAR2LTE-EVID-V1\n", 17);
    mBdsTeeMax = (UINT32)(BufferSize - 1);
    mBdsTeeLen = 17;
    mBdsTeeBuf = Message;

    Star2LteTraceDumpMadt (Tag);

    mBdsTeeBuf = NULL;
    WrittenLen = mBdsTeeLen;

    Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, 0, BufferSize, Message);
    if (!EFI_ERROR (Status)) {
      BlockIo->FlushBlocks (BlockIo);
      EvidWritten = TRUE;
    }
    FreePool (Message);

    BdsPramStr ("\n=EVID wr ");
    BdsPramByte (EFI_ERROR (Status) ? (UINT8)'E' : (UINT8)'S');
    BdsPramHex ((UINT32)(Status & 0xFFFFu), 4);
    BdsPramStr (" len=");
    BdsPramHex (WrittenLen, 8);
    BdsPramStr (" bs=");
    BdsPramHex (BlockIo->Media->BlockSize, 4);
    break;
  }

  if (!Found) {
    BdsPramStr ("\n=EVID none");
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }
}

STATIC
CONST CHAR8 *
Star2LteTraceGuidName (
  IN CONST EFI_GUID  *Guid
  )
{
  if (Guid == NULL) {
    return "null";
  }

  if (CompareGuid (Guid, &gEfiLoadedImageProtocolGuid)) {
    return "ldimg";
  }
  if (CompareGuid (Guid, &gEfiDevicePathProtocolGuid)) {
    return "devpath";
  }
  if (CompareGuid (Guid, &gEfiSimpleFileSystemProtocolGuid)) {
    return "sfs";
  }
  if (CompareGuid (Guid, &gEfiBlockIoProtocolGuid)) {
    return "blkio";
  }
  if (CompareGuid (Guid, &gEfiGraphicsOutputProtocolGuid)) {
    return "gop";
  }
  if (CompareGuid (Guid, &gEfiSimpleTextOutProtocolGuid)) {
    return "txtout";
  }
  if (CompareGuid (Guid, &gEfiAcpiTableProtocolGuid)) {
    return "acpitab";
  }
  if (CompareGuid (Guid, &gEfiAcpiSdtProtocolGuid)) {
    return "acpisdt";
  }
  if (CompareGuid (Guid, &mEfiHiiStringProtocolGuid)) {
    return "hiistr";
  }
  if (CompareGuid (Guid, &mEfiUnicodeCollationProtocolGuid)) {
    return "unicoll";
  }
  if (CompareGuid (Guid, &gEdkiiUfsHostControllerProtocolGuid)) {
    return "ufshc";
  }
  if (CompareGuid (Guid, &gEfiExtScsiPassThruProtocolGuid)) {
    return "xscsi";
  }
  if (CompareGuid (Guid, &gEfiScsiIoProtocolGuid)) {
    return "scsiio";
  }

  return NULL;
}

STATIC
VOID
Star2LteTraceGuid (
  IN CONST EFI_GUID  *Guid
  )
{
  CONST CHAR8  *Name;

  Name = Star2LteTraceGuidName (Guid);
  if (Name != NULL) {
    BdsPramStr (Name);
  } else if (Guid != NULL) {
    BdsPramStr ("g");
    BdsPramHex (Guid->Data1, 8);
  } else {
    BdsPramStr ("null");
  }
}

STATIC
BOOLEAN
Star2LteTraceBegin (
  IN CONST CHAR8  *Tag
  )
{
  if (mStar2LteTraceEvents >= STAR2LTE_BOOT_TRACE_LIMIT) {
    return FALSE;
  }

  mStar2LteTraceEvents++;
  //
  // Star2Lte: write the persistent-RAM marker FIRST, then do the (finicky command-mode
  // DECON) HUD present. Previously the present ran before the marker, so if a present ever
  // HANGS, that event's marker is never written and the hang is indistinguishable from a
  // winload-internal halt. With the marker first, a present-hang leaves a trailing "btNNN tag"
  // with no detail fields (caller never returns), which pinpoints the present as the culprit.
  //
  BdsPramStr ("\nbt");
  BdsPramHex (mStar2LteTraceEvents, 3);
  BdsPramByte (' ');
  BdsPramStr (Tag);
  if ((mStar2LteReadHudHoldDepth == 0) &&
      !((Tag != NULL) && (Tag[0] == 'b') && (Tag[1] == 'i') && (Tag[2] == 'o') && (Tag[3] == '\0'))) {
    Star2LteHudUpdate (Tag, mStar2LteTraceEvents, mStar2LteBlockReadSeq, mStar2LteAllocSeq);
  }
  return TRUE;
}

STATIC
VOID
Star2LteTraceTableCrc (
  IN OUT EFI_TABLE_HEADER  *Header
  )
{
  if ((Header == NULL) || (mOrigCalculateCrc32 == NULL)) {
    return;
  }

  Header->CRC32 = 0;
  mOrigCalculateCrc32 (Header, Header->HeaderSize, &Header->CRC32);
}

STATIC
VOID
Star2LteTraceProtocolResult (
  IN CONST CHAR8     *Tag,
  IN CONST EFI_GUID  *Guid,
  IN EFI_STATUS      Status,
  IN UINTN           Count
  )
{
  if (!EFI_ERROR (Status) && (Guid != NULL) && CompareGuid (Guid, &mEfiHiiStringProtocolGuid)) {
    return;
  }
  if (!EFI_ERROR (Status) && (Guid != NULL) && CompareGuid (Guid, &mEfiUnicodeCollationProtocolGuid)) {
    return;
  }

  if (!EFI_ERROR (Status) && (mStar2LteTraceProtocolEvents >= STAR2LTE_PROTOCOL_TRACE_LIMIT)) {
    return;
  }

  mStar2LteTraceProtocolEvents++;
  if (Star2LteTraceBegin (Tag)) {
    BdsPramStr (" p=");
    Star2LteTraceGuid (Guid);
    BdsPramStr (" st=");
    Star2LteTraceStatus (Status);
    if (Count != MAX_UINTN) {
      BdsPramStr (" n=");
      BdsPramHex ((UINT32)Count, 4);
    }
  }
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceLoadImage (
  IN  BOOLEAN                   BootPolicy,
  IN  EFI_HANDLE                ParentImageHandle,
  IN  EFI_DEVICE_PATH_PROTOCOL  *DevicePath   OPTIONAL,
  IN  VOID                      *SourceBuffer OPTIONAL,
  IN  UINTN                     SourceSize,
  OUT EFI_HANDLE                *ImageHandle
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;

  Status = mOrigLoadImage (BootPolicy, ParentImageHandle, DevicePath, SourceBuffer, SourceSize, ImageHandle);
  if (Star2LteTraceBegin ("load")) {
    BdsPramStr (" bp=");
    BdsPramHex (BootPolicy ? 1u : 0u, 1);
    BdsPramStr (" sz=");
    BdsPramHex ((UINT32)SourceSize, 8);
    BdsPramStr (" st=");
    Star2LteTraceStatus (Status);
    BdsPramStr (" ra=");
    BdsPramHex64 (STAR2LTE_CALLER_RA (), 16);
    if (!EFI_ERROR (Status) && (ImageHandle != NULL) && (*ImageHandle != NULL)) {
      LoadedImage = NULL;
      if (!EFI_ERROR (((mOrigHandleProtocol != NULL) ? mOrigHandleProtocol : gBS->HandleProtocol) (*ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage)) && (LoadedImage != NULL)) {
        BdsPramStr (" ib=");
        BdsPramHex64 ((UINT64)(UINTN)LoadedImage->ImageBase, 16);
        BdsPramStr (" isz=");
        BdsPramHex64 ((UINT64)LoadedImage->ImageSize, 8);
      }
    }
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceFreePages (
  IN EFI_PHYSICAL_ADDRESS  Memory,
  IN UINTN                 Pages
  )
{
  EFI_STATUS  Status;

  Star2LteAfterFinalServiceTrace ("freep>", Memory, Pages);
  Status = mOrigFreePages (Memory, Pages);
  Star2LteAfterFinalServiceTrace ("freep<", Memory, (UINT64)Status);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceStartImage (
  IN  EFI_HANDLE  ImageHandle,
  OUT UINTN       *ExitDataSize,
  OUT CHAR16      **ExitData    OPTIONAL
  )
{
  STATIC BOOLEAN  MiscRecoveryArmed;
  EFI_STATUS      Status;

  if (Star2LteTraceBegin ("start")) {
    BdsPramStr (" h=");
    BdsPramHex ((UINT32)(UINTN)ImageHandle, 8);
  }

  if (!MiscRecoveryArmed) {
    MiscRecoveryArmed = TRUE;
    Star2LteArmMiscRecoveryMessage ();
    // v14: DISABLED (v13-only evidence-partition write).
    // Star2LteWriteEvidencePartition ("start");
  }

  Status = mOrigStartImage (ImageHandle, ExitDataSize, ExitData);
  Star2LteTraceBsOverlay ("START", TRUE, Status, (UINT32)(UINTN)ImageHandle, (ExitDataSize != NULL) ? (UINT32)*ExitDataSize : 0u, 0);

  if (Star2LteTraceBegin ("ret")) {
    BdsPramStr (" st=");
    Star2LteTraceStatus (Status);
    if (ExitDataSize != NULL) {
      BdsPramStr (" xsz=");
      BdsPramHex ((UINT32)*ExitDataSize, 8);
    }
    if ((ExitData != NULL) && (*ExitData != NULL)) {
      BdsPramStr (" xd=");
      BdsPramStr16Ascii (*ExitData, 48);
    }
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceFreePool (
  IN VOID  *Buffer
  )
{
  EFI_STATUS  Status;

  Star2LteAfterFinalServiceTrace ("freeo>", (UINT64)(UINTN)Buffer, 0u);
  Status = mOrigFreePool (Buffer);
  Star2LteAfterFinalServiceTrace ("freeo<", (UINT64)(UINTN)Buffer, (UINT64)Status);
  return Status;
}

STATIC
EFI_TPL
EFIAPI
Star2LteTraceRaiseTpl (
  IN EFI_TPL  NewTpl
  )
{
  EFI_TPL  OldTpl;

  Star2LteAfterFinalServiceTrace ("rtpl>", NewTpl, 0u);
  OldTpl = mOrigRaiseTpl (NewTpl);
  Star2LteAfterFinalServiceTrace ("rtpl<", OldTpl, 0u);
  return OldTpl;
}

STATIC
VOID
EFIAPI
Star2LteTraceRestoreTpl (
  IN EFI_TPL  OldTpl
  )
{
  Star2LteAfterFinalServiceTrace ("tpl>", OldTpl, 0u);
  mOrigRestoreTpl (OldTpl);
  Star2LteAfterFinalServiceTrace ("tpl<", OldTpl, 0u);
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceGetMemoryMap (
  IN OUT UINTN                  *MemoryMapSize,
  OUT    EFI_MEMORY_DESCRIPTOR  *MemoryMap,
  OUT    UINTN                  *MapKey,
  OUT    UINTN                  *DescriptorSize,
  OUT    UINT32                 *DescriptorVersion
  )
{
  EFI_STATUS  Status;
  UINTN       InSize;
  UINT64      CallerRa;

  InSize = (MemoryMapSize != NULL) ? *MemoryMapSize : 0;
  CallerRa = STAR2LTE_CALLER_RA ();
  //
  // Star2Lte: in the late (pre-ExitBootServices) window, emit a "gmm>" marker BEFORE the real
  // GetMemoryMap. The post-call "gmm" marker is written only after mOrigGetMemoryMap returns, so a
  // hang INSIDE the real map fetch would be invisible. With gmm> we see winload's final map attempt
  // (and its input size) even if that call never returns -> distinguishes "hang in GetMemoryMap"
  // from "halt before winload calls it".
  //
  if (!mStar2LteAfterFinalProbe && (mStar2LteTraceEvents >= STAR2LTE_LATE_ALLOC_TRACE_EVENT) && Star2LteTraceBegin ("gmm>")) {
    BdsPramStr (" in=");
    BdsPramHex ((UINT32)InSize, 8);
    BdsPramStr (" ra=");
    BdsPramHex64 (CallerRa, 16);
    Star2LteTraceFrameChain ();
    BdsPramStr (" vbar=");
    BdsPramHex64 (Star2LteReadVbarEl1 (), 16);
    BdsPramStr (" daif=");
    BdsPramHex ((UINT32)Star2LteReadDaif (), 4);
  }
  if (FALSE && Star2LteAfterFinalTraceBegin ("AFG>")) {
    BdsPramStr (" in=");
    BdsPramHex ((UINT32)InSize, 8);
    BdsPramStr (" buf=");
    BdsPramHex64 ((UINT64)(UINTN)MemoryMap, 16);
    BdsPramStr (" ra=");
    BdsPramHex64 (CallerRa, 16);
  }
  Status = mOrigGetMemoryMap (MemoryMapSize, MemoryMap, MapKey, DescriptorSize, DescriptorVersion);
  Star2LteBootWatchdogPet ();
  if (!mStar2LteAfterFinalProbe && (mStar2LteTraceMemoryEvents < STAR2LTE_MEMORY_TRACE_LIMIT)) {
    mStar2LteTraceMemoryEvents++;
    if (Star2LteTraceBegin ("gmm")) {
      BdsPramStr (" in=");
      BdsPramHex ((UINT32)InSize, 8);
      BdsPramStr (" out=");
      BdsPramHex ((MemoryMapSize != NULL) ? (UINT32)*MemoryMapSize : 0u, 8);
      BdsPramStr (" key=");
      BdsPramHex ((MapKey != NULL) ? (UINT32)*MapKey : 0u, 8);
      BdsPramStr (" st=");
      Star2LteTraceStatus (Status);
      if (mStar2LteTraceEvents >= STAR2LTE_LATE_ALLOC_TRACE_EVENT) {
        BdsPramStr (" ra=");
        BdsPramHex64 (CallerRa, 16);
      }
    }
  }
  if (FALSE && Star2LteAfterFinalTraceBegin ("AFG<")) {
    BdsPramStr (" out=");
    BdsPramHex ((MemoryMapSize != NULL) ? (UINT32)*MemoryMapSize : 0u, 8);
    BdsPramStr (" key=");
    BdsPramHex ((MapKey != NULL) ? (UINT32)*MapKey : 0u, 8);
    BdsPramStr (" ds=");
    BdsPramHex ((DescriptorSize != NULL) ? (UINT32)*DescriptorSize : 0u, 4);
    BdsPramStr (" st=");
    Star2LteTraceStatus (Status);
  }
  if (FALSE && mStar2LteAfterFinalProbe && !mStar2LteAfterFinalMapDumped && !EFI_ERROR (Status) &&
      (MemoryMap != NULL) && (MemoryMapSize != NULL) && (DescriptorSize != NULL) &&
      (*DescriptorSize >= sizeof (EFI_MEMORY_DESCRIPTOR)) && (*MemoryMapSize <= 0x4000u)) {
    UINTN  DescCount;
    UINTN  Index;

    mStar2LteAfterFinalMapDumped = TRUE;
    DescCount = *MemoryMapSize / *DescriptorSize;
    BdsPramStr ("\n=AFMS buf=");
    BdsPramHex64 ((UINT64)(UINTN)MemoryMap, 16);
    BdsPramStr (" sz=");
    BdsPramHex ((UINT32)*MemoryMapSize, 8);
    BdsPramStr (" ds=");
    BdsPramHex ((UINT32)*DescriptorSize, 4);
    BdsPramStr (" key=");
    BdsPramHex ((MapKey != NULL) ? (UINT32)*MapKey : 0u, 8);
    BdsPramStr (" n=");
    BdsPramHex ((UINT32)DescCount, 4);
    for (Index = 0; (Index < DescCount) && (Index < 8u); Index++) {
      Star2LteDumpAfterFinalDescriptor (Index, (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)MemoryMap + (Index * *DescriptorSize)));
    }
    if (DescCount > 12u) {
      for (Index = DescCount - 4u; Index < DescCount; Index++) {
        Star2LteDumpAfterFinalDescriptor (Index, (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)MemoryMap + (Index * *DescriptorSize)));
      }
    }
  }

  //
  // Star2Lte: the last visible operation is winload's zero-size final map probe. The older broad
  // dump showed the stack's candidate map buffer is still zero here; now switch to a tiny
  // after-final timeline so the PRAM survives long enough to catch the next boot-service call.
  //
  {
    STATIC BOOLEAN  mFinalDumped = FALSE;

    if (!mFinalDumped && (InSize == 0) && (Status == EFI_BUFFER_TOO_SMALL) &&
        (MemoryMapSize != NULL) && (*MemoryMapSize <= 0xC00u) &&
        (mStar2LteTraceEvents >= STAR2LTE_LATE_ALLOC_TRACE_EVENT) &&
        (mStar2LteLastFramePra != 0) && (mStar2LteLastFramePra < CallerRa)) {
      UINT64      ParentMap;
      UINT64      ParentMapSize;
      UINT64      ParentRet1;
      UINT64      ParentRet2;
      UINT64      ParentStackBase;
      volatile UINT64  *ParentStack;

        mFinalDumped                  = TRUE;
        mStar2LteAfterFinalProbe      = TRUE;
        mStar2LteAfterFinalMapDumped  = FALSE;
        mStar2LteAfterFinalTraceCount = 0;
        ParentMap                     = 0;
        ParentMapSize                 = 0;
        ParentRet1                    = 0;
        ParentRet2                    = 0;
        ParentStackBase               = mStar2LteLastFramePfp - 0x80u;
        ParentStack                   = NULL;
      if (Star2LteLooksLikeDramPtr (ParentStackBase) &&
          Star2LteLooksLikeDramPtr (ParentStackBase + (29u * sizeof (UINT64)))) {
        ParentStack   = (volatile UINT64 *)(UINTN)ParentStackBase;
        ParentRet1    = ParentStack[17];
        ParentMapSize = ParentStack[19];
        ParentMap     = ParentStack[24];
        ParentRet2    = ParentStack[29];
      }
      BdsPramStr ("\n=AF0 out=");
      BdsPramHex ((UINT32)*MemoryMapSize, 8);
      BdsPramStr (" key=");
      BdsPramHex ((MapKey != NULL) ? (UINT32)*MapKey : 0u, 8);
      BdsPramStr (" ra=");
      BdsPramHex64 (CallerRa, 16);
      BdsPramStr (" pra=");
      BdsPramHex64 (mStar2LteLastFramePra, 16);
      BdsPramStr (" map=");
      BdsPramHex64 (ParentMap, 16);
      if (ParentStack != NULL) {
        BdsPramStr (" sz=");
        BdsPramHex64 (ParentMapSize, 8);
        BdsPramStr (" r1=");
        BdsPramHex64 (ParentRet1, 16);
        BdsPramStr (" r2=");
        BdsPramHex64 (ParentRet2, 16);
      }
      //
      // Star2Lte (2026-08-08) BOOT RELIABILITY FIX.
      //
      // Everything below this comment used to run diagnostics in the live
      // winload handoff, and they made the boot INTERMITTENT: roughly half of
      // all boots stopped at the Windows logo before the kernel started (pmsg
      // carried the full firmware trace but zero UFSINIT, and the SYSTEM hive
      // was byte-unchanged). Retrying from recovery would then boot normally.
      //
      // The two functional patches are KEPT because the machine cannot boot
      // without them:
      //   Star2LtePatchNtosCntfrqInMemory - CNTFRQ_EL0 physically reads 0 here
      //                                     (=CFQ0 0), so ntoskrnl needs the
      //                                     substituted frequency.
      //   Star2LtePatchWinloadTimers      - the TLBI substitution the hyp can
      //                                     actually emulate (=STLBI0/1/2).
      //
      // Three diagnostics are REMOVED:
      //
      //   Star2LteStartSoftwareStepWindow - planted a BRK into winload's LIVE
      //     code (=SWA a=0x90e03204 o=540000e3) and single-stepped 96
      //     instructions through the exception vectors. This is the race:
      //     Star2LteKernelContextMsrName's own comment states that past
      //     winload's TTBR/VBAR writes "the TTBR switch remaps the code page
      //     holding our planted breakpoint and the VBAR write replaces the
      //     exception vectors the stepper relies on". Whether the stepper
      //     finishes before winload reaches that switch is a timing race, and
      //     losing it hangs the machine with a planted breakpoint in winload
      //     and no vectors to service it.
      //
      //   Star2LteArmEbsHardwareWatchdog - arms an Exynos WDT that resets the
      //     phone. Watchdogs were deliberately removed from this project; a
      //     reset mid-boot both corrupts the filesystem and destroys evidence.
      //     (Currently compiled out, but the call must not come back.)
      //
      //   The LVM ASID/TLBI probe - deliberately installed a NON-ZERO ASID into
      //     both TTBR0_EL1 and TTBR1_EL1 and issued raw `tlbi vmalle1` against
      //     the kernel's live TTBR1 base, purely to test which combination
      //     hangs. Writing TTBRs underneath a running winload has no place in a
      //     boot we want to be reliable.
      //
      // Keep this handoff minimal. Anything added here runs while winload is
      // mid-transition, so it can only ever reduce reliability.
      //
      Star2LtePatchNtosCntfrqInMemory ();
      Star2LtePatchWinloadTimers ();
      BdsPramStr ("\n=HANDOFF minimal");
      BdsPramByte ('\n');
    }
  }

  if (FALSE && mStar2LteAfterFinalProbe && (InSize == 0) && (Status == EFI_BUFFER_TOO_SMALL)) {
    BdsPramStr ("\n=AFRET ra=");
    BdsPramHex64 (CallerRa, 16);
    BdsPramStr (" pra=");
    BdsPramHex64 (mStar2LteLastFramePra, 16);
    BdsPramStr (" daif=");
    BdsPramHex ((UINT32)Star2LteReadDaif (), 4);
  }

  return Status;
}

STATIC
VOID
Star2LteDrawHandoffMarker (
  IN UINT32  Color,
  IN UINTN   Y
  )
{
#if STAR2LTE_PANEL_DIAG_TEXT
  volatile UINT32  *Fb;
  UINTN            Row;
  UINTN            Col;

  Fb = (volatile UINT32 *)(UINTN)STAR2LTE_FB_BASE;
  for (Row = 0; Row < 72u; Row++) {
    for (Col = 160u; Col < (STAR2LTE_FB_WIDTH - 160u); Col++) {
      Fb[((Y + Row) * STAR2LTE_FB_WIDTH) + Col] = Color;
    }
  }
  for (Row = 0; Row < 24u; Row++) {
    for (Col = 220u; Col < 1220u; Col += 80u) {
      Fb[((Y + 24u + Row) * STAR2LTE_FB_WIDTH) + Col] = 0x00FFFFFFu;
      Fb[((Y + 24u + Row) * STAR2LTE_FB_WIDTH) + Col + 1u] = 0x00FFFFFFu;
      Fb[((Y + 24u + Row) * STAR2LTE_FB_WIDTH) + Col + 2u] = 0x00FFFFFFu;
      Fb[((Y + 24u + Row) * STAR2LTE_FB_WIDTH) + Col + 3u] = 0x00FFFFFFu;
    }
  }
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  Star2LtePresent ();
#endif
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceExitBootServices (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       MapKey
  )
{
  EFI_STATUS  Status;
  BOOLEAN     WatchdogWasArmed;
  UINT64      CallerRa;

  CallerRa = STAR2LTE_CALLER_RA ();

  //
  // Star2Lte: winload enters its ExitBootServices critical section at TPL_HIGH (our periodic
  // heartbeat timer stops firing there). Log the CALL to PRAM immediately - before the overlay
  // and the DECON present, which can hang at high TPL - so we can tell whether winload actually
  // reaches ExitBootServices (=EBSENTER present) or halts in its final map prep before it (absent).
  //
  BdsPramStr ("\n=EBSENTER key=");
  BdsPramHex ((UINT32)MapKey, 8);
  BdsPramStr (" ra=");
  BdsPramHex64 (CallerRa, 16);
  BdsPramByte ('\n');

  //
  // SMP DIAG: last firmware instant before winload owns the machine. Dumping
  // the published MADT here guarantees it lands at the TAIL of the circular
  // ring, which is the part that survives the wrap and reaches pstore.
  //
  // v14: DISABLED. v12/v13 (which carried this call) both hung inside Windows
  // after =EBSOK. v11 (no runtime MADT dump at EBS) boots. The _MAT experiment
  // needs Windows to actually reach the desktop, so revert to v11 behaviour.
  //
  // Star2LteTraceDumpMadt ("ebs");

  //
  // Scan the Windows kernel PT (0x907AA000) HERE, at =EBSENTER, BEFORE forwarding
  // ExitBootServices. The kernel PT is already built by now (winload's =WTRACE/
  // =KCTXV/=AFP kernel-transition hooks fire before this call), and winload's
  // address space is STILL STABLE (pre-EBS). Scanning after EBS (=EBSOK) faulted:
  // ExitBootServices makes winload tear down / rebuild its map, so 0x907AA000 was
  // transiently unmapped (=EXC far=0x907AA000 DFSC=0x07 even though =PTF showed a
  // valid L2 block a moment later = a race). Doing it pre-EBS avoids that.
  //
  // Arm the HARDWARE watchdog (writes the Exynos WDT regs directly + sets the
  // recovery-to-TWRP boot reason) BEFORE the scan so the device SELF-RECOVERS even
  // if the scan hangs (e.g. the recovery tlbi is trapped by uH). Without this the
  // scan runs before the =EBSOK watchdog-arm => any scan hang is indefinite.
  //
  Star2LteArmEbsHardwareWatchdog ();
  Star2LteScanKernelPt ();

  Star2LteTraceBsOverlay ("EBS", FALSE, EFI_SUCCESS, (UINT32)MapKey, (UINT32)(UINTN)ImageHandle, 0);
  Star2LteDrawHandoffMarker (0x000000FFu, 96u);
  if (Star2LteTraceBegin ("ebs>")) {
    BdsPramStr (" key=");
    BdsPramHex ((UINT32)MapKey, 8);
  }

  WatchdogWasArmed = mStar2LteBootWatchdogArmed;
  if (WatchdogWasArmed) {
    Star2LteBootWatchdogDisarm ();
  }

  Status = mOrigExitBootServices (ImageHandle, MapKey);
  if (WatchdogWasArmed && EFI_ERROR (Status)) {
    Star2LteBootWatchdogArm ();
  }

  if (!EFI_ERROR (Status)) {
    BdsPramStr ("\n=EBSOK");
    BdsPramStr (" ra=");
    BdsPramHex64 (CallerRa, 16);
#if STAR2LTE_USB_DEBUG_MODE == STAR2LTE_USB_DEBUG_HID
    Star2LteUsbDebugQuiesce ();
#endif
    Star2LteArmEbsHardwareWatchdog ();
    return Status;
  }

  Star2LteTraceBsOverlay ("EBS", TRUE, Status, (UINT32)MapKey, (UINT32)(UINTN)ImageHandle, 0);
  Star2LteDrawHandoffMarker (EFI_ERROR (Status) ? 0x0000FFFFu : 0x0000FF00u, 192u);
  if (Star2LteTraceBegin ("ebs<")) {
    BdsPramStr (" st=");
    Star2LteTraceStatus (Status);
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceCloseEvent (
  IN EFI_EVENT  Event
  )
{
  EFI_STATUS  Status;

  Star2LteAfterFinalServiceTrace ("clse>", (UINT64)(UINTN)Event, 0u);
  Status = mOrigCloseEvent (Event);
  Star2LteAfterFinalServiceTrace ("clse<", (UINT64)(UINTN)Event, (UINT64)Status);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceSignalEvent (
  IN EFI_EVENT  Event
  )
{
  EFI_STATUS  Status;

  Star2LteAfterFinalServiceTrace ("sige>", (UINT64)(UINTN)Event, 0u);
  Status = mOrigSignalEvent (Event);
  Star2LteAfterFinalServiceTrace ("sige<", (UINT64)(UINTN)Event, (UINT64)Status);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceWaitForEvent (
  IN  UINTN      NumberOfEvents,
  IN  EFI_EVENT  *Event,
  OUT UINTN      *Index
  )
{
  EFI_STATUS  Status;

  Star2LteAfterFinalServiceTrace ("wait>", NumberOfEvents, (UINT64)(UINTN)Event);
  Status = mOrigWaitForEvent (NumberOfEvents, Event, Index);
  Star2LteAfterFinalServiceTrace ("wait<", (Index != NULL) ? *Index : 0u, (UINT64)Status);
  return Status;
}

STATIC
VOID
EFIAPI
Star2LteTraceCopyMem (
  IN VOID  *Destination,
  IN VOID  *Source,
  IN UINTN Length
  )
{
  Star2LteAfterFinalServiceTrace ("copy>", (UINT64)(UINTN)Destination, Length);
  mOrigCopyMem (Destination, Source, Length);
  Star2LteAfterFinalServiceTrace ("copy<", (UINT64)(UINTN)Destination, Length);
}

STATIC
VOID
EFIAPI
Star2LteTraceSetMem (
  IN VOID  *Buffer,
  IN UINTN Size,
  IN UINT8 Value
  )
{
  Star2LteAfterFinalServiceTrace ("setm>", (UINT64)(UINTN)Buffer, Size);
  mOrigSetMem (Buffer, Size, Value);
  Star2LteAfterFinalServiceTrace ("setm<", (UINT64)(UINTN)Buffer, Value);
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceCalculateCrc32 (
  IN  VOID    *Data,
  IN  UINTN   DataSize,
  OUT UINT32  *Crc32
  )
{
  EFI_STATUS  Status;

  Star2LteAfterFinalServiceTrace ("crc>", (UINT64)(UINTN)Data, DataSize);
  Status = mOrigCalculateCrc32 (Data, DataSize, Crc32);
  Star2LteAfterFinalServiceTrace ("crc<", (Crc32 != NULL) ? *Crc32 : 0u, (UINT64)Status);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceAllocatePages (
  IN     EFI_ALLOCATE_TYPE     Type,
  IN     EFI_MEMORY_TYPE       MemoryType,
  IN     UINTN                 Pages,
  IN OUT EFI_PHYSICAL_ADDRESS  *Memory
  )
{
  EFI_STATUS  Status;
  EFI_PHYSICAL_ADDRESS  Requested;
  BOOLEAN     InReserve1;
  BOOLEAN     InReserve2;
  UINT32      Seq;

  Seq    = ++mStar2LteAllocSeq;
  Requested = (Memory != NULL) ? *Memory : 0;
  Star2LteBootWatchdogPet ();
  InReserve1 = (BOOLEAN)((Requested >= STAR2LTE_WINLOAD_RESERVE_BASE) &&
                         ((Requested + EFI_PAGES_TO_SIZE (Pages)) <= (STAR2LTE_WINLOAD_RESERVE_BASE + EFI_PAGES_TO_SIZE (STAR2LTE_WINLOAD_RESERVE_PAGES))));
  InReserve2 = (BOOLEAN)((Requested >= STAR2LTE_WINLOAD_RESERVE2_BASE) &&
                         ((Requested + EFI_PAGES_TO_SIZE (Pages)) <= (STAR2LTE_WINLOAD_RESERVE2_BASE + EFI_PAGES_TO_SIZE (STAR2LTE_WINLOAD_RESERVE2_PAGES))));
  if ((Type == AllocateAddress) && (MemoryType == EfiLoaderCode) &&
      ((InReserve1 && mStar2LteWinloadReserve1Active) ||
       (InReserve2 && mStar2LteWinloadReserve2Active))) {
    if (Star2LteTraceBegin ("apgr")) {
      BdsPramStr (" req=");
      BdsPramHex64 (Requested, 12);
      BdsPramStr (" pg=");
      BdsPramHex ((UINT32)Pages, 8);
      BdsPramStr (" ra=");
      BdsPramHex64 (STAR2LTE_CALLER_RA (), 16);
      BdsPramStr (" act=");
      BdsPramHex (mStar2LteWinloadReserveActive ? 1u : 0u, 1);
    }
    return EFI_SUCCESS;
  }
  Status = mOrigAllocatePages (Type, MemoryType, Pages, Memory);
  if (Star2LteAfterFinalTraceBegin ("AFP")) {
    BdsPramStr (" q=");
    BdsPramHex (Seq, 8);
    BdsPramStr (" ty=");
    BdsPramHex ((UINT32)Type, 2);
    BdsPramStr (" mt=");
    BdsPramHex ((UINT32)MemoryType, 2);
    BdsPramStr (" pg=");
    BdsPramHex ((UINT32)Pages, 8);
    BdsPramStr (" req=");
    BdsPramHex64 (Requested, 12);
    BdsPramStr (" st=");
    Star2LteTraceStatus (Status);
    if ((Memory != NULL) && !EFI_ERROR (Status)) {
      BdsPramStr (" a=");
      BdsPramHex64 (*Memory, 12);
    }
    BdsPramStr (" ra=");
    BdsPramHex64 (STAR2LTE_CALLER_RA (), 16);
  }
  //
  // Star2Lte: winload's OS-load phase issues HUNDREDS OF THOUSANDS of allocations (q reached
  // ~0x61000 in one boot), mostly single 4 KiB pages, which flood the PRAM buffer and blind us
  // to whatever it does next (ExitBootServices / kernel handoff). In the late window trace only
  // LARGE allocations (>= 0x100 pages = 1 MiB, the actual module/kernel loads) so the trace
  // survives the storm and reaches the real frontier. Errors are always traced. ALSO emit a
  // HEARTBEAT every 0x4000th allocation (q & 0x3FFF == 0): if winload keeps doing single-page
  // allocations past the last big load (e.g. building kernel page tables), the heartbeat proves
  // it is still ALIVE and allocating rather than halted - the filtered single-page storm would
  // otherwise look identical to a halt.
  //
  if (mStar2LteTraceSetVariableEnabled && (EFI_ERROR (Status) || ((mStar2LteTraceEvents >= STAR2LTE_LATE_ALLOC_TRACE_EVENT) && ((Pages >= 0x100u) || ((Seq & 0x3FFFu) == 0)))) && (mStar2LteTraceEvents < STAR2LTE_BOOT_TRACE_LIMIT)) {
    if (Star2LteTraceBegin ("apg")) {
      BdsPramStr (" q=");
      BdsPramHex (Seq, 8);
      BdsPramStr (" ty=");
      BdsPramHex ((UINT32)Type, 2);
      BdsPramStr (" mt=");
      BdsPramHex ((UINT32)MemoryType, 2);
      BdsPramStr (" pg=");
      BdsPramHex ((UINT32)Pages, 8);
      BdsPramStr (" req=");
      BdsPramHex64 (Requested, 12);
      BdsPramStr (" ra=");
      BdsPramHex64 (STAR2LTE_CALLER_RA (), 16);
      BdsPramStr (" st=");
      Star2LteTraceStatus (Status);
      if ((Memory != NULL) && !EFI_ERROR (Status)) {
        BdsPramStr (" a=");
        BdsPramHex64 (*Memory, 12);
      }
    }
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceAllocatePool (
  IN  EFI_MEMORY_TYPE  PoolType,
  IN  UINTN            Size,
  OUT VOID             **Buffer
  )
{
  EFI_STATUS  Status;
  UINT32      Seq;

  Seq    = ++mStar2LteAllocSeq;
  Status = mOrigAllocatePool (PoolType, Size, Buffer);
  if (Star2LteAfterFinalTraceBegin ("AFL")) {
    BdsPramStr (" q=");
    BdsPramHex (Seq, 8);
    BdsPramStr (" mt=");
    BdsPramHex ((UINT32)PoolType, 2);
    BdsPramStr (" sz=");
    BdsPramHex ((UINT32)Size, 8);
    BdsPramStr (" st=");
    Star2LteTraceStatus (Status);
    if ((Buffer != NULL) && (*Buffer != NULL) && !EFI_ERROR (Status)) {
      BdsPramStr (" p=");
      BdsPramHex64 ((UINT64)(UINTN)*Buffer, 12);
    }
    BdsPramStr (" ra=");
    BdsPramHex64 (STAR2LTE_CALLER_RA (), 16);
  }
  if (mStar2LteTraceSetVariableEnabled && ((Seq <= 32u) || EFI_ERROR (Status) || ((mStar2LteTraceEvents >= STAR2LTE_LATE_ALLOC_TRACE_EVENT) && (Size >= 0x400u))) && (mStar2LteTraceEvents < STAR2LTE_BOOT_TRACE_LIMIT)) {
    if (Star2LteTraceBegin ("apl")) {
      BdsPramStr (" q=");
      BdsPramHex (Seq, 8);
      BdsPramStr (" mt=");
      BdsPramHex ((UINT32)PoolType, 2);
      BdsPramStr (" sz=");
      BdsPramHex ((UINT32)Size, 8);
      if (mStar2LteTraceEvents >= STAR2LTE_LATE_ALLOC_TRACE_EVENT) {
        BdsPramStr (" ra=");
        BdsPramHex64 (STAR2LTE_CALLER_RA (), 16);
      }
      BdsPramStr (" st=");
      Star2LteTraceStatus (Status);
      if ((Buffer != NULL) && (*Buffer != NULL) && !EFI_ERROR (Status)) {
        BdsPramStr (" p=");
        BdsPramHex ((UINT32)(UINTN)*Buffer, 8);
      }
    }
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceLocateProtocol (
  IN  EFI_GUID  *Protocol,
  IN  VOID      *Registration OPTIONAL,
  OUT VOID      **Interface
  )
{
  EFI_STATUS  Status;

  Star2LteAfterFinalServiceTrace ("locp>", (Protocol != NULL) ? Protocol->Data1 : 0u, (UINT64)(UINTN)Registration);
  Status = mOrigLocateProtocol (Protocol, Registration, Interface);
  Star2LteAfterFinalServiceTrace ("locp<", (UINT64)(UINTN)((Interface != NULL) ? *Interface : NULL), (UINT64)Status);
  if (EFI_ERROR (Status) || ((Protocol != NULL) &&
      (CompareGuid (Protocol, &gEfiAcpiTableProtocolGuid) || CompareGuid (Protocol, &gEfiGraphicsOutputProtocolGuid) || CompareGuid (Protocol, &gEfiLoadedImageProtocolGuid)))) {
    Star2LteTraceBsOverlay ("LOCP", TRUE, Status, Protocol != NULL ? Protocol->Data1 : 0u, (Interface != NULL) ? (UINT32)(UINTN)*Interface : 0u, 0);
  }
  Star2LteTraceProtocolResult ("locp", Protocol, Status, MAX_UINTN);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceLocateHandle (
  IN     EFI_LOCATE_SEARCH_TYPE  SearchType,
  IN     EFI_GUID                *Protocol  OPTIONAL,
  IN     VOID                    *SearchKey OPTIONAL,
  IN OUT UINTN                   *BufferSize,
  OUT    EFI_HANDLE              *Buffer
  )
{
  EFI_STATUS  Status;

  Star2LteAfterFinalServiceTrace ("lhnd>", (Protocol != NULL) ? Protocol->Data1 : 0u, (BufferSize != NULL) ? *BufferSize : 0u);
  Status = mOrigLocateHandle (SearchType, Protocol, SearchKey, BufferSize, Buffer);
  Star2LteAfterFinalServiceTrace ("lhnd<", (BufferSize != NULL) ? *BufferSize : 0u, (UINT64)Status);
  if ((Protocol != NULL) && CompareGuid (Protocol, &gEfiGraphicsOutputProtocolGuid)) {
    Star2LteTraceProtocolResult ("lhnd", Protocol, Status, (BufferSize != NULL) ? (UINT32)*BufferSize : 0);
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceLocateHandleBuffer (
  IN     EFI_LOCATE_SEARCH_TYPE  SearchType,
  IN     EFI_GUID                *Protocol  OPTIONAL,
  IN     VOID                    *SearchKey OPTIONAL,
  OUT    UINTN                   *NoHandles,
  OUT    EFI_HANDLE              **Buffer
  )
{
  EFI_STATUS  Status;

  Star2LteAfterFinalServiceTrace ("loch>", (Protocol != NULL) ? Protocol->Data1 : 0u, (UINT64)SearchType);
  Status = mOrigLocateHandleBuffer (SearchType, Protocol, SearchKey, NoHandles, Buffer);
  Star2LteAfterFinalServiceTrace ("loch<", (NoHandles != NULL) ? *NoHandles : 0u, (UINT64)Status);
  Star2LteTraceProtocolResult ("loch", Protocol, Status, (NoHandles != NULL) ? *NoHandles : 0);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceOpenProtocol (
  IN  EFI_HANDLE  Handle,
  IN  EFI_GUID    *Protocol,
  OUT VOID        **Interface OPTIONAL,
  IN  EFI_HANDLE  AgentHandle,
  IN  EFI_HANDLE  ControllerHandle,
  IN  UINT32      Attributes
  )
{
  EFI_STATUS  Status;

  Star2LteAfterFinalServiceTrace ("open>", (Protocol != NULL) ? Protocol->Data1 : 0u, Attributes);
  Status = mOrigOpenProtocol (Handle, Protocol, Interface, AgentHandle, ControllerHandle, Attributes);
  Star2LteAfterFinalServiceTrace ("open<", (UINT64)(UINTN)((Interface != NULL) ? *Interface : NULL), (UINT64)Status);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceHandleProtocol (
  IN  EFI_HANDLE  Handle,
  IN  EFI_GUID    *Protocol,
  OUT VOID        **Interface
  )
{
  EFI_STATUS  Status;

  Star2LteAfterFinalServiceTrace ("hand>", (Protocol != NULL) ? Protocol->Data1 : 0u, (UINT64)(UINTN)Handle);
  Status = mOrigHandleProtocol (Handle, Protocol, Interface);
  Star2LteAfterFinalServiceTrace ("hand<", (UINT64)(UINTN)((Interface != NULL) ? *Interface : NULL), (UINT64)Status);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceCreateEvent (
  IN  UINT32           Type,
  IN  EFI_TPL          NotifyTpl,
  IN  EFI_EVENT_NOTIFY NotifyFunction OPTIONAL,
  IN  VOID             *NotifyContext OPTIONAL,
  OUT EFI_EVENT        *Event
  )
{
  EFI_STATUS  Status;

  Star2LteAfterFinalServiceTrace ("cevt>", Type, NotifyTpl);
  Status = mOrigCreateEvent (Type, NotifyTpl, NotifyFunction, NotifyContext, Event);
  Star2LteAfterFinalServiceTrace ("cevt<", (UINT64)(UINTN)((Event != NULL) ? *Event : NULL), (UINT64)Status);
  if (mStar2LteTraceEvents < STAR2LTE_BOOT_TRACE_LIMIT) {
    if (Star2LteTraceBegin ("cevt")) {
      BdsPramStr (" t=");
      BdsPramHex (Type, 8);
      BdsPramStr (" st=");
      Star2LteTraceStatus (Status);
    }
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceSetTimer (
  IN EFI_EVENT        Event,
  IN EFI_TIMER_DELAY  Type,
  IN UINT64           TriggerTime
  )
{
  EFI_STATUS  Status;
  UINT32      Seq;

  Seq = ++mStar2LteSetTimerSeq;
  Star2LteAfterFinalServiceTrace ("stim>", (UINT64)(UINTN)Event, TriggerTime);
  Status = mOrigSetTimer (Event, Type, TriggerTime);
  Star2LteAfterFinalServiceTrace ("stim<", (UINT64)Type, (UINT64)Status);
  if (((Seq <= 8u) || EFI_ERROR (Status)) && (mStar2LteTraceEvents < STAR2LTE_BOOT_TRACE_LIMIT)) {
    if (Star2LteTraceBegin ("stim")) {
      BdsPramStr (" q=");
      BdsPramHex (Seq, 8);
      BdsPramStr (" ty=");
      BdsPramHex ((UINT32)Type, 2);
      BdsPramStr (" st=");
      Star2LteTraceStatus (Status);
    }
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceStall (
  IN UINTN  Microseconds
  )
{
  EFI_STATUS  Status;
  UINT32      Seq;

  Seq = ++mStar2LteStallSeq;
  Star2LteAfterFinalServiceTrace ("stall>", Microseconds, Seq);
  if (mStar2LteTraceSetVariableEnabled && ((Seq < 8u) || ((Seq & 0x3FFu) == 0)) && (mStar2LteTraceVariableEvents < STAR2LTE_VARIABLE_TRACE_LIMIT)) {
    mStar2LteTraceVariableEvents++;
    if (Star2LteTraceBegin ("stall>")) {
      BdsPramStr (" us=");
      BdsPramHex ((UINT32)Microseconds, 8);
      BdsPramStr (" q=");
      BdsPramHex (Seq, 8);
    }
  }
  Status = mOrigStall (Microseconds);
  Star2LteAfterFinalServiceTrace ("stall<", Seq, (UINT64)Status);
  if (mStar2LteTraceSetVariableEnabled && ((Seq < 8u) || ((Seq & 0x3FFu) == 0)) && (mStar2LteTraceVariableEvents < STAR2LTE_VARIABLE_TRACE_LIMIT)) {
    mStar2LteTraceVariableEvents++;
    if (Star2LteTraceBegin ("stall<")) {
      BdsPramStr (" st=");
      Star2LteTraceStatus (Status);
    }
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceCheckEvent (
  IN EFI_EVENT  Event
  )
{
  EFI_STATUS  Status;
  UINT32      Seq;

  Seq = ++mStar2LteCheckSeq;
  Star2LteAfterFinalServiceTrace ("chev>", (UINT64)(UINTN)Event, Seq);
  Status = mOrigCheckEvent (Event);
  Star2LteAfterFinalServiceTrace ("chev<", Seq, (UINT64)Status);
  if (mStar2LteTraceSetVariableEnabled && ((Seq < 8u) || ((Seq & 0x3FFu) == 0)) && (mStar2LteTraceVariableEvents < STAR2LTE_VARIABLE_TRACE_LIMIT)) {
    mStar2LteTraceVariableEvents++;
    if (Star2LteTraceBegin ("chev")) {
      BdsPramStr (" st=");
      Star2LteTraceStatus (Status);
      BdsPramStr (" q=");
      BdsPramHex (Seq, 8);
    }
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  EFI_STATUS  Status;
  UINT32      Seq;

  Seq = ++mStar2LteKeySeq;
  Star2LteAfterFinalServiceTrace ("key>", (UINT64)(UINTN)This, Seq);
  if (mOrigConInReadKeyStroke == NULL) {
    Status = EFI_UNSUPPORTED;
  } else {
    Status = mOrigConInReadKeyStroke (This, Key);
  }
  Star2LteAfterFinalServiceTrace ("key<", Seq, (UINT64)Status);

  if (mStar2LteTraceSetVariableEnabled && ((Seq < 8u) || ((Seq & 0x3FFu) == 0)) && (mStar2LteTraceEvents < STAR2LTE_BOOT_TRACE_LIMIT)) {
    if (Star2LteTraceBegin ("key")) {
      BdsPramStr (" st=");
      Star2LteTraceStatus (Status);
      BdsPramStr (" q=");
      BdsPramHex (Seq, 8);
      if (!EFI_ERROR (Status) && (Key != NULL)) {
        BdsPramStr (" sc=");
        BdsPramHex (Key->ScanCode, 4);
        BdsPramStr (" ch=");
        BdsPramHex (Key->UnicodeChar, 4);
      }
    }
  }
  return Status;
}

STATIC
VOID
Star2LteDiskActivityBlink (
  VOID
  )
{
#if STAR2LTE_DISK_ACTIVITY_INDICATOR
  volatile UINT32  *Fb;
  UINT32           Color;
  UINTN            Row;
  UINTN            Col;

  Fb    = (volatile UINT32 *)(UINTN)STAR2LTE_FB_BASE;
  Color = ((++mStar2LteDiskBlinkSeq & 1u) != 0) ? 0x0000FF00u : 0x000000FFu;
  for (Row = 0; Row < 48u; Row++) {
    for (Col = 0; Col < 48u; Col++) {
      Fb[((96u + Row) * STAR2LTE_FB_WIDTH) + 96u + Col] = Color;
    }
  }
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  Star2LtePresent ();
#endif
}

STATIC
EFI_BLOCK_READ
Star2LteFindOriginalBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  OUT UINT32                 *Index OPTIONAL
  )
{
  UINT32  Slot;

  for (Slot = 0; Slot < mStar2LteBlockTraceCount; Slot++) {
    if (mStar2LteBlockTrace[Slot].BlockIo == This) {
      if (Index != NULL) {
        *Index = mStar2LteBlockTrace[Slot].Index;
      }
      return mStar2LteBlockTrace[Slot].ReadBlocks;
    }
  }

  if (Index != NULL) {
    *Index = 0xFFFFFFFFu;
  }
  return NULL;
}

STATIC
EFI_STATUS
Star2LteReadBlocksChunked (
  IN     EFI_BLOCK_READ         ReadBlocks,
  IN     EFI_BLOCK_IO_PROTOCOL  *This,
  IN     UINT32                 MediaId,
  IN     EFI_LBA                Lba,
  IN     UINTN                  BufferSize,
  OUT    VOID                   *Buffer,
  IN     UINTN                  MaxChunkSize,
  IN OUT UINT32                 *Chunks
  )
{
  EFI_STATUS  Status;
  UINTN       Remaining;
  UINTN       ChunkSize;
  UINTN       BlockSize;
  UINT8       *ChunkBuffer;
  EFI_LBA     ChunkLba;
  BOOLEAN     HudHold;

  if ((ReadBlocks == NULL) || (This == NULL) || (This->Media == NULL) || (This->Media->BlockSize == 0) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status      = EFI_SUCCESS;
  Remaining   = BufferSize;
  BlockSize   = This->Media->BlockSize;
  ChunkBuffer = (UINT8 *)Buffer;
  ChunkLba    = Lba;
  HudHold     = (BOOLEAN)(This->Media->LogicalPartition && (BufferSize > STAR2LTE_BLOCK_READ_CHUNK));
  if (HudHold) {
    mStar2LteReadHudHoldDepth++;
  }
  while (Remaining > 0) {
    ChunkSize = Remaining;
    if (ChunkSize > MaxChunkSize) {
      ChunkSize = MaxChunkSize;
      ChunkSize -= (ChunkSize % BlockSize);
    }
    if (ChunkSize == 0) {
      ChunkSize = Remaining;
    }

    if ((Chunks == NULL) || (*Chunks == 0u) || ((*Chunks & STAR2LTE_HUD_READ_UPDATE_MASK) == 0u) || (Remaining <= ChunkSize)) {
      Star2LteHudUpdate ("READ", (UINT32)ChunkLba, (UINT32)Remaining, (Chunks != NULL) ? *Chunks : 0u);
    }

    Status = ReadBlocks (This, MediaId, ChunkLba, ChunkSize, ChunkBuffer);
    if (Chunks != NULL) {
      (*Chunks)++;
    }
    if (EFI_ERROR (Status)) {
      break;
    }
    Star2LteBootWatchdogPet ();

    Remaining   -= ChunkSize;
    ChunkBuffer += ChunkSize;
    ChunkLba    += ChunkSize / BlockSize;
  }

  if (HudHold && (mStar2LteReadHudHoldDepth > 0)) {
    mStar2LteReadHudHoldDepth--;
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceReadBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  OUT VOID                  *Buffer
  )
{
  EFI_BLOCK_READ  ReadBlocks;
  EFI_STATUS      Status;
  UINT32          Index;
  UINT32          Seq;
  UINT32          Chunks;
  BOOLEAN         Trace;
  BOOLEAN         Split;

  ReadBlocks = Star2LteFindOriginalBlockRead (This, &Index);
  if (ReadBlocks == NULL) {
    return EFI_UNSUPPORTED;
  }

  Seq    = ++mStar2LteBlockReadSeq;
  if ((Seq == 1u) || ((Seq & 0x3Fu) == 0)) {
    Star2LteDiskActivityBlink ();
  }
  Chunks = 1;
  Split  = (BOOLEAN)(BufferSize > STAR2LTE_BLOCK_READ_CHUNK);
  if (Split && (This != NULL) && (This->Media != NULL) && (This->Media->BlockSize != 0) && (Buffer != NULL)) {
    Chunks = 0;
    Status = Star2LteReadBlocksChunked (ReadBlocks, This, MediaId, Lba, BufferSize, Buffer, mStar2LteBlockReadUseFallback ? STAR2LTE_BLOCK_READ_FALLBACK_CHUNK : STAR2LTE_BLOCK_READ_CHUNK, &Chunks);
    if (EFI_ERROR (Status) && !mStar2LteBlockReadUseFallback) {
      mStar2LteBlockReadUseFallback = TRUE;
      Status = Star2LteReadBlocksChunked (ReadBlocks, This, MediaId, Lba, BufferSize, Buffer, STAR2LTE_BLOCK_READ_FALLBACK_CHUNK, &Chunks);
    }
    if (EFI_ERROR (Status)) {
      Status = Star2LteReadBlocksChunked (ReadBlocks, This, MediaId, Lba, BufferSize, Buffer, STAR2LTE_BLOCK_READ_MIN_CHUNK, &Chunks);
    }
  } else {
    Split  = FALSE;
    Status = ReadBlocks (This, MediaId, Lba, BufferSize, Buffer);
  }

  if (Split && (This != NULL) && (This->Media != NULL) && This->Media->LogicalPartition) {
    Star2LteHudUpdate (EFI_ERROR (Status) ? "RDER" : "RDOK", (UINT32)Lba, (UINT32)BufferSize, Chunks);
  }

  Trace  = (BOOLEAN)(mStar2LteTraceSetVariableEnabled && (EFI_ERROR (Status) || (Split && (This != NULL) && (This->Media != NULL) && This->Media->LogicalPartition) || (Seq <= 12u) || ((Seq & 0xFFu) == 0)));
  if (Trace && (mStar2LteTraceEvents < STAR2LTE_BOOT_TRACE_LIMIT)) {
    if (Star2LteTraceBegin ("bio")) {
      BdsPramStr (" q=");
      BdsPramHex (Seq, 8);
      BdsPramStr (" i=");
      BdsPramHex (Index, 2);
      BdsPramStr (" p=");
      BdsPramHex ((This != NULL) && (This->Media != NULL) && This->Media->LogicalPartition ? 1u : 0u, 1);
      BdsPramStr (" lba=");
      BdsPramHex ((UINT32)Lba, 8);
      BdsPramStr (" sz=");
      BdsPramHex ((UINT32)BufferSize, 8);
      if (Split) {
        BdsPramStr (" split=");
        BdsPramHex (Chunks, 2);
      }
      if ((This != NULL) && (This->Media != NULL)) {
        BdsPramStr (" last=");
        BdsPramHex ((UINT32)This->Media->LastBlock, 8);
      }
      BdsPramStr (" st=");
      Star2LteTraceStatus (Status);
    }
  }

  return Status;
}

STATIC
VOID
Star2LteInstallBlockIoTrace (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_HANDLE             *Handles;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  UINTN                  HandleCount;
  UINTN                  HandleIndex;
  UINT32                 Slot;

  Handles     = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (HandleIndex = 0; (HandleIndex < HandleCount) && (mStar2LteBlockTraceCount < STAR2LTE_BLOCK_TRACE_MAX); HandleIndex++) {
    Status = gBS->HandleProtocol (Handles[HandleIndex], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status) || (BlockIo == NULL) || (BlockIo->ReadBlocks == Star2LteTraceReadBlocks)) {
      continue;
    }

    for (Slot = 0; Slot < mStar2LteBlockTraceCount; Slot++) {
      if (mStar2LteBlockTrace[Slot].BlockIo == BlockIo) {
        break;
      }
    }
    if (Slot < mStar2LteBlockTraceCount) {
      continue;
    }

    mStar2LteBlockTrace[mStar2LteBlockTraceCount].BlockIo    = BlockIo;
    mStar2LteBlockTrace[mStar2LteBlockTraceCount].ReadBlocks = BlockIo->ReadBlocks;
    mStar2LteBlockTrace[mStar2LteBlockTraceCount].Index      = mStar2LteBlockTraceCount;
    mStar2LteBlockTraceCount++;
    BlockIo->ReadBlocks = Star2LteTraceReadBlocks;
  }

  FreePool (Handles);
  BdsPramStr ("\n=BIOTRACE n=");
  BdsPramHex (mStar2LteBlockTraceCount, 2);
  BdsPramByte ('\n');
}

STATIC
VOID
Star2LteReserveWinloadPages (
  VOID
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  EFI_STATUS            Status;

  if (mOrigAllocatePages == NULL) {
    return;
  }

  Base = STAR2LTE_WINLOAD_RESERVE_BASE;
  Status = mOrigAllocatePages (AllocateAddress, EfiLoaderCode, STAR2LTE_WINLOAD_RESERVE_PAGES, &Base);
  mStar2LteWinloadReserve1Active = (BOOLEAN)(!EFI_ERROR (Status));
  BdsPramStr ("\n=WRES st=");
  Star2LteTraceStatus (Status);
  BdsPramStr (" a=");
  BdsPramHex64 (Base, 12);
  BdsPramByte ('\n');

  Base = STAR2LTE_WINLOAD_RESERVE2_BASE;
  Status = mOrigAllocatePages (AllocateAddress, EfiLoaderCode, STAR2LTE_WINLOAD_RESERVE2_PAGES, &Base);
  mStar2LteWinloadReserve2Active = (BOOLEAN)(!EFI_ERROR (Status));
  mStar2LteWinloadReserveActive = (BOOLEAN)(mStar2LteWinloadReserve1Active && mStar2LteWinloadReserve2Active);
  BdsPramStr ("=WRE2 st=");
  Star2LteTraceStatus (Status);
  BdsPramStr (" a=");
  BdsPramHex64 (Base, 12);
  BdsPramByte ('\n');
}

STATIC
UINT32
Star2LteFrameBufferSignature (
  VOID
  )
{
  volatile UINT32  *Fb;
  UINT32           Sig;
  UINTN            X;
  UINTN            Y;

  Fb  = (volatile UINT32 *)(UINTN)STAR2LTE_FB_BASE;
  Sig = 0x811C9DC5u;
  for (Y = STAR2LTE_FB_HEIGHT / 16u; Y < STAR2LTE_FB_HEIGHT; Y += STAR2LTE_FB_HEIGHT / 8u) {
    for (X = STAR2LTE_FB_WIDTH / 16u; X < STAR2LTE_FB_WIDTH; X += STAR2LTE_FB_WIDTH / 8u) {
      Sig = ((Sig << 5) | (Sig >> 27)) ^ Fb[(Y * STAR2LTE_FB_WIDTH) + X];
    }
  }
  Sig ^= Fb[((STAR2LTE_FB_HEIGHT / 2u) * STAR2LTE_FB_WIDTH) + (STAR2LTE_FB_WIDTH / 2u)];
  return Sig;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceGetVariable (
  IN     CHAR16    *VariableName,
  IN     EFI_GUID  *VendorGuid,
  OUT    UINT32    *Attributes OPTIONAL,
  IN OUT UINTN     *DataSize,
  OUT    VOID      *Data OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINT8       Value;

  Star2LteAfterFinalServiceTrace ("gvar>", (VariableName != NULL) ? VariableName[0] : 0u, (DataSize != NULL) ? *DataSize : 0u);
  if ((VariableName != NULL) &&
      (VendorGuid != NULL) &&
      CompareGuid (VendorGuid, &gEfiGlobalVariableGuid) &&
      (StrCmp (VariableName, L"OsIndicationsSupported") == 0)) {
    if (Attributes != NULL) {
      *Attributes = 0;
    }
    if (DataSize != NULL) {
      *DataSize = 0;
    }
    Status = EFI_NOT_FOUND;
    if (mStar2LteTraceVariableEvents < STAR2LTE_VARIABLE_TRACE_LIMIT) {
      mStar2LteTraceVariableEvents++;
      if (Star2LteTraceBegin ("gvar")) {
        BdsPramStr (" osind st=");
        Star2LteTraceStatus (Status);
      }
    }
    Star2LteAfterFinalServiceTrace ("gvar<", (DataSize != NULL) ? *DataSize : 0u, (UINT64)Status);
    return Status;
  }

  if ((VariableName != NULL) &&
      (VendorGuid != NULL) &&
      CompareGuid (VendorGuid, &gEfiGlobalVariableGuid) &&
      ((StrCmp (VariableName, L"SecureBoot") == 0) ||
       (StrCmp (VariableName, L"SetupMode") == 0) ||
       (StrCmp (VariableName, L"AuditMode") == 0) ||
       (StrCmp (VariableName, L"DeployedMode") == 0))) {
    Value = (StrCmp (VariableName, L"SetupMode") == 0) ? 1u : 0u;
    if (Attributes != NULL) {
      *Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
    }
    if (DataSize == NULL) {
      Status = EFI_INVALID_PARAMETER;
    } else if ((*DataSize < sizeof (Value)) || (Data == NULL)) {
      *DataSize = sizeof (Value);
      Status = EFI_BUFFER_TOO_SMALL;
    } else {
      *(UINT8 *)Data = Value;
      *DataSize = sizeof (Value);
      Status = EFI_SUCCESS;
    }
    if (mStar2LteTraceVariableEvents < STAR2LTE_VARIABLE_TRACE_LIMIT) {
      mStar2LteTraceVariableEvents++;
      if (Star2LteTraceBegin ("gvar")) {
        BdsPramStr (" sbshim st=");
        Star2LteTraceStatus (Status);
      }
    }
    Star2LteAfterFinalServiceTrace ("gvar<", (DataSize != NULL) ? *DataSize : 0u, (UINT64)Status);
    return Status;
  }

  if ((VariableName != NULL) &&
      ((StrCmp (VariableName, L"CurrentPolicy") == 0) ||
       (StrCmp (VariableName, L"CurrentActivePolicy") == 0))) {
    if (Attributes != NULL) {
      *Attributes = 0;
    }
    if (DataSize != NULL) {
      *DataSize = 0;
    }
    Status = EFI_NOT_FOUND;
    if (mStar2LteTraceVariableEvents < STAR2LTE_VARIABLE_TRACE_LIMIT) {
      mStar2LteTraceVariableEvents++;
      if (Star2LteTraceBegin ("gvar")) {
        BdsPramStr (" masked-policy st=");
        Star2LteTraceStatus (Status);
      }
    }
    Star2LteAfterFinalServiceTrace ("gvar<", (DataSize != NULL) ? *DataSize : 0u, (UINT64)Status);
    return Status;
  }

  Status = mOrigGetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
  Star2LteAfterFinalServiceTrace ("gvar<", (DataSize != NULL) ? *DataSize : 0u, (UINT64)Status);
  if ((VariableName != NULL) &&
      ((StrCmp (VariableName, L"SetupMode") == 0) ||
       (StrCmp (VariableName, L"SecureBoot") == 0) ||
       (StrCmp (VariableName, L"CurrentPolicy") == 0) ||
       (StrCmp (VariableName, L"CurrentActivePolicy") == 0))) {
    Star2LteTraceBsOverlay ("GVAR", TRUE, Status, (DataSize != NULL) ? (UINT32)*DataSize : 0u, (VariableName[0] << 16) | VariableName[1], (VariableName[2] << 16) | VariableName[3]);
  }
  if ((VariableName != NULL) && (StrCmp (VariableName, L"PlatformLang") == 0)) {
    return Status;
  }
  if ((VariableName != NULL) &&
      ((StrCmp (VariableName, L"RtcEpochSeconds") == 0) ||
       (StrCmp (VariableName, L"RtcTimeZone") == 0) ||
       (StrCmp (VariableName, L"RtcDaylight") == 0))) {
    return Status;
  }

  if (EFI_ERROR (Status) && (mStar2LteTraceVariableEvents < STAR2LTE_VARIABLE_TRACE_LIMIT)) {
    mStar2LteTraceVariableEvents++;
    if (Star2LteTraceBegin ("gvar")) {
      BdsPramStr (" ");
      if (VariableName != NULL) {
        BdsPramStr16Ascii (VariableName, 28);
      } else {
        BdsPramStr ("null");
      }
      BdsPramStr (" vg=");
      if ((VendorGuid != NULL) && CompareGuid (VendorGuid, &gEfiGlobalVariableGuid)) {
        BdsPramStr ("global");
      } else {
        Star2LteTraceGuid (VendorGuid);
      }
      BdsPramStr (" sz=");
      BdsPramHex ((DataSize != NULL) ? (UINT32)*DataSize : 0u, 8);
      BdsPramStr (" st=");
      Star2LteTraceStatus (Status);
    }
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceSetVariable (
  IN CHAR16    *VariableName,
  IN EFI_GUID  *VendorGuid,
  IN UINT32    Attributes,
  IN UINTN     DataSize,
  IN VOID      *Data
  )
{
  EFI_STATUS  Status;
  BOOLEAN     Trace;

  Star2LteAfterFinalServiceTrace ("svar>", (VariableName != NULL) ? VariableName[0] : 0u, DataSize);
  Trace = (BOOLEAN)(mStar2LteTraceSetVariableEnabled && (VariableName != NULL));
  if (Trace && (mStar2LteTraceVariableEvents < STAR2LTE_VARIABLE_TRACE_LIMIT)) {
    mStar2LteTraceVariableEvents++;
    if (Star2LteTraceBegin ("svar>")) {
      BdsPramByte (' ');
      BdsPramStr16Ascii (VariableName, 28);
      BdsPramStr (" sz=");
      BdsPramHex ((UINT32)DataSize, 8);
      BdsPramStr (" at=");
      BdsPramHex (Attributes, 8);
    }
  }

  Status = mOrigSetVariable (VariableName, VendorGuid, Attributes, DataSize, Data);
  Star2LteAfterFinalServiceTrace ("svar<", DataSize, (UINT64)Status);

  if (Trace && (mStar2LteTraceVariableEvents < STAR2LTE_VARIABLE_TRACE_LIMIT)) {
    mStar2LteTraceVariableEvents++;
    if (Star2LteTraceBegin ("svar<")) {
      BdsPramByte (' ');
      BdsPramStr16Ascii (VariableName, 28);
      BdsPramStr (" st=");
      Star2LteTraceStatus (Status);
    }
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceGetNextVariableName (
  IN OUT UINTN     *VariableNameSize,
  IN OUT CHAR16    *VariableName,
  IN OUT EFI_GUID  *VendorGuid
  )
{
  EFI_STATUS  Status;

  Star2LteAfterFinalServiceTrace ("gnvn>", (VariableNameSize != NULL) ? *VariableNameSize : 0u, 0u);
  if (mStar2LteTraceSetVariableEnabled && (mStar2LteTraceVariableEvents < STAR2LTE_VARIABLE_TRACE_LIMIT)) {
    mStar2LteTraceVariableEvents++;
    if (Star2LteTraceBegin ("gnvn>")) {
      BdsPramStr (" sz=");
      BdsPramHex ((VariableNameSize != NULL) ? (UINT32)*VariableNameSize : 0, 8);
    }
  }
  Status = mOrigGetNextVariableName (VariableNameSize, VariableName, VendorGuid);
  Star2LteAfterFinalServiceTrace ("gnvn<", (VariableNameSize != NULL) ? *VariableNameSize : 0u, (UINT64)Status);
  if (mStar2LteTraceSetVariableEnabled && (mStar2LteTraceVariableEvents < STAR2LTE_VARIABLE_TRACE_LIMIT)) {
    mStar2LteTraceVariableEvents++;
    if (Star2LteTraceBegin ("gnvn<")) {
      BdsPramStr (" st=");
      Star2LteTraceStatus (Status);
      BdsPramStr (" sz=");
      BdsPramHex ((VariableNameSize != NULL) ? (UINT32)*VariableNameSize : 0, 8);
    }
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Star2LteTraceGetTime (
  OUT EFI_TIME              *Time,
  OUT EFI_TIME_CAPABILITIES *Capabilities OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINT32      FbSig;
  UINT32      Tick;

  Star2LteAfterFinalServiceTrace ("time>", (UINT64)(UINTN)Time, (UINT64)(UINTN)Capabilities);
  if (mStar2LteTraceSetVariableEnabled) {
    Tick = ++mStar2LteSyntheticTimeSeconds;
    FbSig = 0;
    if ((Tick < 8u) || ((Tick & 0x3Fu) == 0)) {
      FbSig = Star2LteFrameBufferSignature ();
      if (mStar2LteLastFbSig == 0) {
        mStar2LteLastFbSig = FbSig;
      } else if (FbSig != mStar2LteLastFbSig) {
        mStar2LteLastFbSig = FbSig;
        mStar2LteFbPresentSeq++;
        if ((mStar2LteFbPresentSeq < 16u) || ((mStar2LteFbPresentSeq & 0xFu) == 0)) {
          if (Star2LteTraceBegin ("fbchg")) {
            BdsPramStr (" q=");
            BdsPramHex (mStar2LteFbPresentSeq, 8);
            BdsPramStr (" t=");
            BdsPramHex (Tick, 8);
            BdsPramStr (" sig=");
            BdsPramHex (FbSig, 8);
          }
        }
        Star2LtePresent ();
      }
    }
    if (((Tick < 8u) || ((Tick & 0x3FFu) == 0)) && (mStar2LteTraceVariableEvents < STAR2LTE_VARIABLE_TRACE_LIMIT)) {
      mStar2LteTraceVariableEvents++;
      if (Star2LteTraceBegin ("time>")) {
        BdsPramStr (" n=");
        BdsPramHex (Tick, 8);
        if (FbSig != 0) {
          BdsPramStr (" fb=");
          BdsPramHex (FbSig, 8);
        }
      }
    }

    if (Time == NULL) {
      Status = EFI_INVALID_PARAMETER;
    } else {
      ZeroMem (Time, sizeof (*Time));
      Time->Year       = 2026;
      Time->Month      = 6;
      Time->Day        = 29;
      Time->Hour       = (UINT8)((Tick / 3600u) % 24u);
      Time->Minute     = (UINT8)((Tick / 60u) % 60u);
      Time->Second     = (UINT8)(Tick % 60u);
      Time->Nanosecond = (Tick % 1000u) * 1000000u;
      Time->TimeZone   = EFI_UNSPECIFIED_TIMEZONE;
      Time->Daylight   = 0;

      if (Capabilities != NULL) {
        Capabilities->Resolution = 1;
        Capabilities->Accuracy   = 0;
        Capabilities->SetsToZero = FALSE;
      }
      Status = EFI_SUCCESS;
    }

    if (((Tick < 8u) || ((Tick & 0x3FFu) == 0)) && (mStar2LteTraceVariableEvents < STAR2LTE_VARIABLE_TRACE_LIMIT)) {
      mStar2LteTraceVariableEvents++;
      if (Star2LteTraceBegin ("time<")) {
        BdsPramStr (" st=");
        Star2LteTraceStatus (Status);
        BdsPramStr (" n=");
        BdsPramHex (Tick, 8);
      }
    }
    Star2LteAfterFinalServiceTrace ("time<", Tick, (UINT64)Status);
    return Status;
  }

  Status = mOrigGetTime (Time, Capabilities);
  Star2LteAfterFinalServiceTrace ("time<", 0u, (UINT64)Status);
  return Status;
}

STATIC
VOID
Star2LteTraceDumpConfigTables (
  VOID
  )
{
  UINTN  Index;

  BdsPramStr ("\n=FWTS A21 n=");
  BdsPramHex ((UINT32)gST->NumberOfTableEntries, 4);
  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    EFI_CONFIGURATION_TABLE  *Config;

    Config = &gST->ConfigurationTable[Index];
    BdsPramStr (" ");
    if (CompareGuid (&Config->VendorGuid, &gEfiAcpi20TableGuid)) {
      BdsPramStr ("acpi20=");
    } else if (CompareGuid (&Config->VendorGuid, &gEfiAcpi10TableGuid)) {
      BdsPramStr ("acpi10=");
    } else if (CompareGuid (&Config->VendorGuid, &gEfiSmbios3TableGuid)) {
      BdsPramStr ("smb3=");
    } else if (CompareGuid (&Config->VendorGuid, &gEfiSmbiosTableGuid)) {
      BdsPramStr ("smb=");
    } else {
      continue;
    }
    BdsPramHex ((UINT32)(UINTN)Config->VendorTable, 8);
  }

  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    EFI_CONFIGURATION_TABLE  *Config;

    Config = &gST->ConfigurationTable[Index];
    if (CompareGuid (&Config->VendorGuid, &gEfiAcpi20TableGuid) && (Config->VendorTable != NULL)) {
      UINT8   *Rsdp;
      UINT64  XsdtAddress;

      Rsdp        = (UINT8 *)Config->VendorTable;
      XsdtAddress = *(UINT64 *)(VOID *)(Rsdp + 24);
      BdsPramStr ("\nrsdp rev=");
      BdsPramHex (Rsdp[15], 2);
      BdsPramStr (" xsdt=");
      BdsPramHex64 (XsdtAddress, 16);
      if (XsdtAddress != 0) {
        EFI_ACPI_DESCRIPTION_HEADER  *Xsdt;
        UINTN                        EntryCount;
        UINTN                        EntryIndex;

        Xsdt       = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)XsdtAddress;
        EntryCount = (Xsdt->Length - sizeof (EFI_ACPI_DESCRIPTION_HEADER)) / sizeof (UINT64);
        BdsPramStr (" sig=");
        Star2LteTraceSig (Xsdt->Signature);
        BdsPramStr (" ec=");
        BdsPramHex ((UINT32)EntryCount, 2);
        for (EntryIndex = 0; (EntryIndex < EntryCount) && (EntryIndex < 12); EntryIndex++) {
          UINT64                       TableAddress;
          EFI_ACPI_DESCRIPTION_HEADER  *Table;

          TableAddress = *(UINT64 *)(VOID *)((UINT8 *)Xsdt + sizeof (EFI_ACPI_DESCRIPTION_HEADER) + EntryIndex * sizeof (UINT64));
          Table        = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)TableAddress;
          BdsPramStr (" t");
          BdsPramHex ((UINT32)EntryIndex, 1);
          BdsPramByte ('=');
          Star2LteTraceSig (Table->Signature);
          if (Table->Signature == SIGNATURE_32 ('F', 'A', 'C', 'P')) {
            EFI_ACPI_6_3_FIXED_ACPI_DESCRIPTION_TABLE  *Fadt;

            Fadt = (EFI_ACPI_6_3_FIXED_ACPI_DESCRIPTION_TABLE *)Table;
            BdsPramStr (" dsdt=");
            BdsPramHex (Fadt->Dsdt, 8);
            BdsPramStr (" xdsdt=");
            BdsPramHex64 (Fadt->XDsdt, 16);
          }
        }
      }
      break;
    }
  }

  //
  // SMP DIAG: early snapshot, taken before the boot-service hooks start filling
  // the ring. If the =EBSENTER copy is lost to the wrap this one may survive.
  //
  // The Windows UFS miniport maps the same PRAM page and rewinds the ramoops
  // cursor, so anything left in the ring is destroyed before TWRP can read it.
  // Tee the dump into a raw GPT partition (DQMDBG) instead; the writer invokes
  // Star2LteTraceDumpMadt internally so the PRAM copy is still emitted.
  //
  // v14: DISABLED (v13-only). See the =EBSENTER note above.
  //
  // Star2LteWriteEvidencePartition ("bds");
  BdsPramByte ('\n');
}

STATIC
VOID
Star2LteTraceInstallBootHooks (
  VOID
  )
{
  if (mStar2LteTraceInstalled) {
    return;
  }

  //
  // DIAG RETENTION FIX: do NOT reset the PRAM trace ring here.
  //
  // This function runs at the END of PlatformBootManagerAfterConsole (after
  // RegisterWindowsBootOption's "=BMREG" and ReportStorageOnScreen's "=WINFS").
  // Resetting the ring at this point destroyed both storage diagnostics on
  // every single run, so the only markers that ever survived to TWRP were the
  // post-reset ones ('g'/'h'/'i'/'j' = UnableToBoot, 'R' = auto-recovery).
  // That made the actual cause of "no bootable device" permanently invisible.
  //
  // BdsPramByte self-initializes the ring when the 'DBGC' magic is absent
  // (cold boot), so dropping this call is safe: the ring simply appends across
  // the boot-attempt cycle and stops when full (cap 0x3F00, no wrap).
  //
  // Star2LteTraceResetLog ();
  Star2LteTraceDumpConfigTables ();
  mStar2LteLastFbSig = Star2LteFrameBufferSignature ();

  mOrigLoadImage          = gBS->LoadImage;
  mOrigStartImage         = gBS->StartImage;
  mOrigGetMemoryMap       = gBS->GetMemoryMap;
  mOrigExitBootServices   = gBS->ExitBootServices;
  mOrigRaiseTpl           = gBS->RaiseTPL;
  mOrigRestoreTpl         = gBS->RestoreTPL;
  mOrigAllocatePages      = gBS->AllocatePages;
  mOrigFreePages          = gBS->FreePages;
  mOrigAllocatePool       = gBS->AllocatePool;
  mOrigFreePool           = gBS->FreePool;
  mOrigLocateProtocol     = gBS->LocateProtocol;
  mOrigLocateHandle       = gBS->LocateHandle;
  mOrigLocateHandleBuffer = gBS->LocateHandleBuffer;
  mOrigOpenProtocol       = gBS->OpenProtocol;
  mOrigHandleProtocol     = gBS->HandleProtocol;
  mOrigCreateEvent        = gBS->CreateEvent;
  mOrigCloseEvent         = gBS->CloseEvent;
  mOrigSignalEvent        = gBS->SignalEvent;
  mOrigWaitForEvent       = gBS->WaitForEvent;
  mOrigSetTimer           = gBS->SetTimer;
  mOrigStall              = gBS->Stall;
  mOrigCheckEvent         = gBS->CheckEvent;
  mOrigCopyMem            = gBS->CopyMem;
  mOrigSetMem             = gBS->SetMem;
  mOrigCalculateCrc32     = gBS->CalculateCrc32;
  mOrigGetVariable        = (gRT != NULL) ? gRT->GetVariable : NULL;
  mOrigSetVariable        = (gRT != NULL) ? gRT->SetVariable : NULL;
  mOrigGetNextVariableName = (gRT != NULL) ? gRT->GetNextVariableName : NULL;
  mOrigGetTime            = (gRT != NULL) ? gRT->GetTime : NULL;

  gBS->LoadImage          = Star2LteTraceLoadImage;
  gBS->StartImage         = Star2LteTraceStartImage;
  gBS->GetMemoryMap       = Star2LteTraceGetMemoryMap;
  gBS->ExitBootServices   = Star2LteTraceExitBootServices;
  gBS->RaiseTPL           = Star2LteTraceRaiseTpl;
  gBS->RestoreTPL         = Star2LteTraceRestoreTpl;
  gBS->AllocatePages      = Star2LteTraceAllocatePages;
  gBS->FreePages          = Star2LteTraceFreePages;
  gBS->AllocatePool       = Star2LteTraceAllocatePool;
  gBS->FreePool           = Star2LteTraceFreePool;
  gBS->LocateProtocol     = Star2LteTraceLocateProtocol;
  gBS->LocateHandle       = Star2LteTraceLocateHandle;
  gBS->LocateHandleBuffer = Star2LteTraceLocateHandleBuffer;
  gBS->OpenProtocol       = Star2LteTraceOpenProtocol;
  gBS->HandleProtocol     = Star2LteTraceHandleProtocol;
  gBS->CreateEvent        = Star2LteTraceCreateEvent;
  gBS->CloseEvent         = Star2LteTraceCloseEvent;
  gBS->SignalEvent        = Star2LteTraceSignalEvent;
  gBS->WaitForEvent       = Star2LteTraceWaitForEvent;
  gBS->SetTimer           = Star2LteTraceSetTimer;
  gBS->Stall              = Star2LteTraceStall;
  gBS->CheckEvent         = Star2LteTraceCheckEvent;
  gBS->CopyMem            = Star2LteTraceCopyMem;
  gBS->SetMem             = Star2LteTraceSetMem;
  gBS->CalculateCrc32     = Star2LteTraceCalculateCrc32;
  if ((gRT != NULL) && (mOrigGetVariable != NULL)) {
    gRT->GetVariable = Star2LteTraceGetVariable;
  }
  if ((gRT != NULL) && (mOrigSetVariable != NULL)) {
    gRT->SetVariable = Star2LteTraceSetVariable;
  }
  if ((gRT != NULL) && (mOrigGetNextVariableName != NULL)) {
    gRT->GetNextVariableName = Star2LteTraceGetNextVariableName;
  }
  if ((gRT != NULL) && (mOrigGetTime != NULL)) {
    gRT->GetTime = Star2LteTraceGetTime;
  }
  if ((gST != NULL) && (gST->ConIn != NULL) && (gST->ConIn->ReadKeyStroke != NULL)) {
    mTraceConIn                = gST->ConIn;
    mOrigConInReadKeyStroke    = gST->ConIn->ReadKeyStroke;
    gST->ConIn->ReadKeyStroke  = Star2LteTraceReadKeyStroke;
  }

  Star2LteTraceTableCrc (&gBS->Hdr);
  if (gRT != NULL) {
    Star2LteTraceTableCrc (&gRT->Hdr);
  }
  Star2LteTraceTableCrc (&gST->Hdr);

  mStar2LteTraceInstalled = TRUE;
  BdsPramStr ("\n=BTRACE A21 installed\n");
}

//
// DECON command-mode present (DEVICE-PROVEN in the M1 milestone, m1_display.c).
// The S9+ panel is COMMAND-MODE: pixels written into the scanout buffer
// (0xCC000000) are NOT shown until DECON is told to push one fresh frame to the
// panel GRAM. sboot leaves DECON configured; we only (1) clean the FB to the
// Point of Coherency (DECON's DMA reads DRAM and does NOT snoop the CPU D-cache)
// and (2) issue the shadow-update + a fresh SW-trigger 0->1 edge that scans out
// one frame. WITHOUT this the EDK2 framebuffer/GraphicsConsole is invisible even
// though every write succeeds. Register map device-exact (dpu_9810 regs-decon.h).
//
#define DECON_BASE               0x0000000016030000ULL
#define DECON_SHADOW_UPDATE_REQ  0x0060u
#define DECON_HW_SW_TRIG         0x0070u
#define SHADOW_UPDATE_REQ_GLOBAL (1u << 31)
#define SHADOW_UPDATE_REQ_WINS   0x3Fu          // all 6 decon windows
#define HW_SW_TRIG_SW_TRIG_EN    (1u << 8)
#define HW_SW_TRIG_HW_TRIG_EN    (1u << 0)      // pulse bit that fires the frame
#define HW_SW_TRIG_HW_TRIG_MASK  (1u << 4)      // 1 = panel TE hardware trigger BLOCKED (sboot default)

STATIC
VOID
Star2LtePresent (
  VOID
  )
{
  volatile UINT32  *Trig = (volatile UINT32 *)(UINTN)(DECON_BASE + DECON_HW_SW_TRIG);
  volatile UINT32  *Shad = (volatile UINT32 *)(UINTN)(DECON_BASE + DECON_SHADOW_UPDATE_REQ);
  UINT32           Base;

  //
  // 1. Order the framebuffer writes before we trigger DECON. The FB is
  //    Device-mapped (MMAP_DEV) so the pixels are already in DRAM; DECON's DMA
  //    reads DRAM directly. (No cache clean needed for Device memory, and we
  //    avoid cache-data ops on Device memory on this core.)
  //
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  //
  // 2. Snapshot the trigger, force HW_TRIG_EN low so each present is a fresh edge.
  //
  Base = *Trig & ~HW_SW_TRIG_HW_TRIG_EN;

  //
  // 3. Request a shadow-register update (global + all windows).
  //
  *Shad = SHADOW_UPDATE_REQ_GLOBAL | SHADOW_UPDATE_REQ_WINS;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  gBS->Stall (1000);

  //
  // 4. Two-write SW trigger: the enable->assert 0->1 edge that pushes one frame.
  //
  *Trig = Base | HW_SW_TRIG_SW_TRIG_EN;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  gBS->Stall (1000);
  *Trig = Base | HW_SW_TRIG_SW_TRIG_EN | HW_SW_TRIG_HW_TRIG_EN;
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  //
  // 5. Let one frame latch. Then, instead of restoring sboot's masked state,
  //    leave the command-mode panel in TE-driven HARDWARE auto-refresh: unmask
  //    the panel's TE hardware trigger (clear HW_TRIG_MASK_DECON) and keep
  //    HW_TRIG_EN set, with SW_TRIG_EN cleared. DECON then re-scans the
  //    framebuffer (0xCC000000) on every panel TE (~60 Hz) with NO CPU
  //    involvement, so the display keeps updating AFTER firmware stops running.
  //    After the winload->ntoskrnl handoff the Windows kernel writes its own
  //    output (boot logo / bugcheck BSOD) to the inherited GOP framebuffer, and
  //    with auto-refresh live that output now becomes visible on the panel
  //    instead of the last frozen firmware frame. (If the panel TE does not
  //    actually drive DECON this is harmless -- the panel simply holds the last
  //    SW-triggered frame, exactly as before this change.)
  //
  gBS->Stall (33000);
  *Trig = (Base & ~(HW_SW_TRIG_HW_TRIG_MASK | HW_SW_TRIG_SW_TRIG_EN)) | HW_SW_TRIG_HW_TRIG_EN;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

//
// Push whatever is currently in the framebuffer scanout (e.g. the SEC-phase
// progress text drawn by Star2LteFbText) to the command-mode panel via the DECON
// SW-trigger. Used at BDS so the early "Starting UEFI..." lines become visible
// before the graphics console takes over and clears the screen.
//
STATIC
VOID
BdsPresentPanel (
  VOID
  )
{
#if STAR2LTE_PANEL_DIAG_TEXT
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  Star2LtePresent ();
#endif
}

/**
  Connect every driver to every controller so UFS, USB, and the console
  enumerate. Simple and slow, but correct for bring-up.
**/
#if !STAR2LTE_FAST_STORAGE_ENUM
STATIC
VOID
ConnectAllControllers (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       HandleCount;
  EFI_HANDLE  *HandleBuffer;
  UINTN       Index;

  HandleCount  = 0;
  HandleBuffer = NULL;

  Status = gBS->LocateHandleBuffer (
                  AllHandles,
                  NULL,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    gBS->ConnectController (HandleBuffer[Index], NULL, NULL, FALSE);
  }

  if (HandleBuffer != NULL) {
    FreePool (HandleBuffer);
  }
}
#endif

/**
  Register a console device (in + out) so text appears on the framebuffer and
  serial as soon as possible.
**/
STATIC
VOID
RegisterConsoles (
  VOID
  )
{
  EFI_STATUS                     Status;
  UINTN                          HandleCount;
  EFI_HANDLE                     *HandleBuffer;
  UINTN                          Index;
  EFI_DEVICE_PATH_PROTOCOL       *DevicePath;

  //
  // Any handle exposing Graphics Output becomes a console-out device.
  //
  HandleBuffer = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < HandleCount; Index++) {
      Status = gBS->HandleProtocol (
                      HandleBuffer[Index],
                      &gEfiDevicePathProtocolGuid,
                      (VOID **)&DevicePath
                      );
      if (!EFI_ERROR (Status)) {
        EfiBootManagerUpdateConsoleVariable (ConOut, DevicePath, NULL);
      }
    }
    FreePool (HandleBuffer);
  }
}

/**
  Build a device path for a boot file on a given filesystem handle.
  Caller frees the result.
**/
STATIC
EFI_DEVICE_PATH_PROTOCOL *
BuildBootFilePath (
  IN EFI_HANDLE    FsHandle,
  IN CONST CHAR16  *FilePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *FsPath;

  FsPath = DevicePathFromHandle (FsHandle);
  if (FsPath == NULL) {
    return NULL;
  }

  return FileDevicePath (FsHandle, FilePath);
}

/**
  Scan all simple-file-system handles for \EFI\Microsoft\Boot\bootmgfw.efi and,
  if found, register a "Windows Boot Manager" boot option at the front of
  BootOrder.
**/
STATIC BOOLEAN  mWindowsBootmgrFound = FALSE;

STATIC
VOID
Star2LtePublishSecureBootOffVariables (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Attributes;
  UINT8       Zero;
  UINT8       One;

  Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
  Zero       = 0;
  One        = 1;

  BdsPramStr ("\n=SBVAR ");
  Status = gRT->SetVariable (L"SecureBoot", &gEfiGlobalVariableGuid, Attributes, sizeof (Zero), &Zero);
  BdsPramStr ("sec=");
  BdsPramHex ((UINT32)(Status & 0xFFu), 2);
  Status = gRT->SetVariable (L"SetupMode", &gEfiGlobalVariableGuid, Attributes, sizeof (One), &One);
  BdsPramStr (" setup=");
  BdsPramHex ((UINT32)(Status & 0xFFu), 2);
  Status = gRT->SetVariable (L"AuditMode", &gEfiGlobalVariableGuid, Attributes, sizeof (Zero), &Zero);
  BdsPramStr (" audit=");
  BdsPramHex ((UINT32)(Status & 0xFFu), 2);
  Status = gRT->SetVariable (L"DeployedMode", &gEfiGlobalVariableGuid, Attributes, sizeof (Zero), &Zero);
  BdsPramStr (" depl=");
  BdsPramHex ((UINT32)(Status & 0xFFu), 2);
  BdsPramByte ('\n');
}

STATIC
VOID
Star2LteDeleteBootOptions (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;
  UINTN                         OptionCount;
  UINTN                         Index;

  OptionCount = 0;
  Options = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);
  if (Options == NULL) {
    return;
  }

  BdsPramStr ("\n=BMCLR n=");
  BdsPramHex ((UINT32)OptionCount, 2);
  for (Index = 0; Index < OptionCount; Index++) {
    EFI_STATUS  DeleteStatus;

    DeleteStatus = EfiBootManagerDeleteLoadOptionVariable (Options[Index].OptionNumber, LoadOptionTypeBoot);
    BdsPramStr (" b");
    BdsPramHex ((UINT32)Options[Index].OptionNumber, 4);
    BdsPramByte ('=');
    BdsPramHex ((UINT32)(DeleteStatus & 0xFFu), 2);
  }
  BdsPramByte ('\n');

  EfiBootManagerFreeLoadOptions (Options, OptionCount);
}

//
// Decide whether a partition should have Fat connected to it, and report WHY.
//
// This used to be `LastBlock == 0x00112FFF` -- a hardcoded partition geometry.
// That constant is NOT self-evidently wrong: the Windows media (sda18) is
// 4,613,734,400 bytes, which is 1,126,400 blocks of 4096 -> LastBlock
// 0x00112FFF exactly. Its FAT32 BPB was measured host-side from a raw capture
// and reports BytesPerSector = 4096 and TotalSectors32 = 1,126,400, so if UFS
// reports a 4 KiB logical block the hardcoded test MATCHES the correct
// partition. If UFS instead reports 512-byte blocks the same partition has
// LastBlock 0x00897FFF and the test matches some other ~550 MiB partition.
// Which of those is true has never been measured on this device, and the
// hardcoded form cannot tell us -- it silently does the wrong thing either way.
//
// Replaced with a real FAT BPB probe (0x55AA plus the "FAT" filesystem-type
// string at the FAT12/16 offset 0x36 or the FAT32 offset 0x52), which is
// geometry- and block-size-independent. Reads exactly one block, so it is
// correct at 512 and at 4096.
//
// Why is reported so a failure is attributable rather than merely negative:
//   0 = FAT detected      1 = ReadBlocks failed (UFS read path)
//   2 = no 0x55AA sig     3 = signature present but no "FAT" type string
//   4 = protocol/geometry rejected before any read was attempted
//
// When Why == 1 the EFI_STATUS is reported too, because "the read failed" and
// "the read failed with EFI_DEVICE_ERROR" are different findings.
//
// Replayed host-side against the real bytes of this device's sda18 (captured
// raw in the Gate 3 preflight): sig 0x55AA at 510/511 present, "FAT32" at
// 0x52 present, so this probe returns Why=0 on the actual target partition.
// EDK2 FatOpenDevice's own gates were replayed on the same bytes and all pass,
// so the volume is mountable and any failure is upstream of Fat.
//
STATIC
BOOLEAN
Star2LteBlockIoLooksLikeFat (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  OUT UINT8                  *Why         OPTIONAL,
  OUT EFI_STATUS             *ReadStatus  OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINT32      BlockSize;
  UINTN       Pages;
  UINT8       *Buffer;
  BOOLEAN     IsFat;
  UINT8       Reason;

  Reason = 4;
  Status = EFI_NOT_STARTED;
  if ((BlockIo == NULL) || (BlockIo->Media == NULL) || (BlockIo->ReadBlocks == NULL)) {
    goto Done;
  }

  BlockSize = BlockIo->Media->BlockSize;
  if ((BlockSize < 512) || (BlockSize > SIZE_64KB)) {
    goto Done;
  }

  Pages  = EFI_SIZE_TO_PAGES (BlockSize);
  Buffer = AllocateAlignedPages (Pages, BlockIo->Media->IoAlign);
  if (Buffer == NULL) {
    goto Done;
  }

  IsFat  = FALSE;
  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0, BlockSize, Buffer);
  if (EFI_ERROR (Status)) {
    Reason = 1;
  } else if ((Buffer[510] != 0x55) || (Buffer[511] != 0xAA)) {
    Reason = 2;
  } else if ((CompareMem (&Buffer[0x36], "FAT", 3) != 0) &&
             (CompareMem (&Buffer[0x52], "FAT", 3) != 0))
  {
    Reason = 3;
  } else {
    Reason = 0;
    IsFat  = TRUE;
  }

  FreeAlignedPages (Buffer, Pages);

  if (Why != NULL) {
    *Why = Reason;
  }

  if (ReadStatus != NULL) {
    *ReadStatus = Status;
  }

  return IsFat;

Done:
  if (Why != NULL) {
    *Why = Reason;
  }

  if (ReadStatus != NULL) {
    *ReadStatus = Status;
  }

  return FALSE;
}

//
// =UFSDIAG -- mirror the storage stack's diagnostic block into the PRAM ring.
//
// Star2LteUfsHcDxe, UfsPassThruDxe, ScsiDiskDxe, DiskIoDxe and PartitionDxe have
// accumulated roughly a hundred debug cycles' worth of instrumentation that
// writes UINT32 words to the reserved page at 0xFED13000. That page is NOT
// surfaced to Linux through pstore, so in practice every one of those words has
// been write-only: the sole reader was on-panel text, which can show a handful.
// The PRAM ring at 0xFED14000 IS surfaced, as pmsg-ramoops-0. Mirroring the
// block into the ring makes the whole existing instrument readable for the first
// time, at the cost of about 1.5 KB of a 16 KB ring.
//
// Zero words are skipped. Star2LteUfsHcDxe's entry point zeroes the low block,
// so a zero means "never written" and carries no information.
//
// The UFS/SCSI window is emitted before the GPT/BDS window because it is the
// more decisive of the two, and a shared budget bounds the worst case so this
// dump can never wrap the ring and destroy the =FATSCAN line that precedes it.
//
#define STAR2LTE_STORAGE_DIAG_BASE   0xFED13000ULL
#define STAR2LTE_STORAGE_DIAG_BUDGET 120u

STATIC
VOID
Star2LteDumpStorageDiag (
  VOID
  )
{
  //
  // {first, last-exclusive} word windows, most decisive first.
  //   300..416 - UfsPassThruHci command/sense/read traces, ScsiDisk step tags
  //     0..80  - Star2LteUfsHcDxe entry block and PartitionDxe/Gpt sub-checks
  //
  STATIC CONST UINT16  Windows[][2] = { { 300, 416 }, { 0, 80 } };

  CONST volatile UINT32  *Diag;
  UINTN                   Window;
  UINTN                   Index;
  UINTN                   Emitted;
  UINTN                   Skipped;

  Diag    = (CONST volatile UINT32 *)(UINTN)STAR2LTE_STORAGE_DIAG_BASE;
  Emitted = 0;
  Skipped = 0;

  BdsPramStr ("\n=UFSDIAG");

  for (Window = 0; Window < ARRAY_SIZE (Windows); Window++) {
    for (Index = Windows[Window][0]; Index < Windows[Window][1]; Index++) {
      UINT32  Value;

      Value = Diag[Index];
      if (Value == 0) {
        continue;
      }

      if (Emitted >= STAR2LTE_STORAGE_DIAG_BUDGET) {
        Skipped++;
        continue;
      }

      BdsPramByte (' ');
      BdsPramHex ((UINT32)Index, 3);
      BdsPramByte (':');
      BdsPramHex (Value, 8);
      Emitted++;
    }
  }

  //
  // Report both counts. A non-zero "ov" means the budget truncated the dump,
  // which must never be mistaken for "those words were zero".
  //
  BdsPramStr (" nz=");
  BdsPramHex ((UINT32)Emitted, 3);
  BdsPramStr (" ov=");
  BdsPramHex ((UINT32)Skipped, 3);

  //
  // Live host-controller register sample, ordered LAST on purpose: if any read
  // were to stall it cannot cost anything already appended to the ring above.
  // These addresses are already read directly from this library elsewhere
  // (0x11120000 at the A5 probe, 0x1112116C at the AXIDMA sample), so they are
  // known-mapped at BDS time rather than assumed to be.
  //
  // Standard UFSHCI block at 0x11120000: 20=IS 24=IE 30=HCS 34=HCE 38=UECPA
  // 3C=UECDL 58=UTRLDBR 5C=UTRLCLR 60=UTRLRSR 64=UTRLCNR. Reads only -- IS,
  // UTRLCLR and UTRLCNR are write-1-to-clear latches and must not be written
  // from here.
  //
  // "nx" is the Exynos vendor HCI_UTRL_NEXUS_TYPE (VS 0x11121100 + 0x40).
  // Star2LteUfsHcDxe writes 0xFFFFFFFF to it once at init, while UfsPassThruHci
  // ORs in one bit per SCSI command and never clears any -- two mechanisms that
  // only agree if the init write does NOT survive, which is exactly what the
  // A50 comment claims and what nothing has ever measured.
  //
  {
    STATIC CONST UINT16  Regs[] = {
      0x0020, 0x0024, 0x0030, 0x0034, 0x0038, 0x003C, 0x0058, 0x005C, 0x0060, 0x0064
    };

    BdsPramStr (" hci");
    for (Index = 0; Index < ARRAY_SIZE (Regs); Index++) {
      BdsPramByte (' ');
      BdsPramHex ((UINT32)Regs[Index], 3);
      BdsPramByte (':');
      BdsPramHex (*(volatile UINT32 *)(UINTN)(0x0000000011120000ULL + Regs[Index]), 8);
    }

    BdsPramStr (" nx:");
    BdsPramHex (*(volatile UINT32 *)(UINTN)0x0000000011121140ULL, 8);
  }

  BdsPramByte ('\n');
}

STATIC
VOID
Star2LteConnectUfsScsiStack (
  OUT UINTN       *UfsHcCount      OPTIONAL,
  OUT UINTN       *ExtScsiCount    OPTIONAL,
  OUT UINTN       *ScsiIoCount     OPTIONAL,
  OUT UINTN       *BlockIoCount    OPTIONAL,
  OUT EFI_STATUS  *UfsConnect      OPTIONAL,
  OUT EFI_STATUS  *ExtScsiConnect  OPTIONAL,
  OUT EFI_STATUS  *ScsiIoConnect   OPTIONAL
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  LastUfsConnect;
  EFI_STATUS  LastExtConnect;
  EFI_STATUS  LastScsiIoConnect;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  UINTN       Index;

  if (UfsHcCount != NULL) {
    *UfsHcCount = 0;
  }

  if (ExtScsiCount != NULL) {
    *ExtScsiCount = 0;
  }

  if (ScsiIoCount != NULL) {
    *ScsiIoCount = 0;
  }

  if (BlockIoCount != NULL) {
    *BlockIoCount = 0;
  }

  LastUfsConnect = EFI_NOT_FOUND;
  LastExtConnect = EFI_NOT_FOUND;
  LastScsiIoConnect = EFI_NOT_FOUND;

  Handles = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEdkiiUfsHostControllerProtocolGuid, NULL,
                  &HandleCount, &Handles
                  );
  if (!EFI_ERROR (Status)) {
    if (UfsHcCount != NULL) {
      *UfsHcCount = HandleCount;
    }

    for (Index = 0; Index < HandleCount; Index++) {
      LastUfsConnect = gBS->ConnectController (Handles[Index], NULL, NULL, FALSE);
    }

    FreePool (Handles);
  }

  Handles = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEfiExtScsiPassThruProtocolGuid, NULL,
                  &HandleCount, &Handles
                  );
  if (!EFI_ERROR (Status)) {
    if (ExtScsiCount != NULL) {
      *ExtScsiCount = HandleCount;
    }

    for (Index = 0; Index < HandleCount; Index++) {
      LastExtConnect = gBS->ConnectController (Handles[Index], NULL, NULL, FALSE);
    }

    FreePool (Handles);
  }

  Handles = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEfiScsiIoProtocolGuid, NULL,
                  &HandleCount, &Handles
                  );
  if (!EFI_ERROR (Status)) {
    if (ScsiIoCount != NULL) {
      *ScsiIoCount = HandleCount;
    }

    for (Index = 0; Index < HandleCount; Index++) {
      LastScsiIoConnect = gBS->ConnectController (Handles[Index], NULL, NULL, FALSE);
    }

    FreePool (Handles);
  }

  Handles = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEfiBlockIoProtocolGuid, NULL,
                  &HandleCount, &Handles
                  );
  if (!EFI_ERROR (Status)) {
    UINTN       FatConnected;
    UINTN       PartitionCount;
    UINT8       FatWhy;
    EFI_STATUS  FatRead;

    if (BlockIoCount != NULL) {
      *BlockIoCount = HandleCount;
    }

    FatConnected   = 0;
    PartitionCount = 0;
    FatWhy         = 0xFF;
    FatRead        = EFI_NOT_STARTED;
    BdsPramStr ("\n=FATSCAN n=");
    BdsPramHex ((UINT32)HandleCount, 2);

    for (Index = 0; Index < HandleCount; Index++) {
      EFI_BLOCK_IO_PROTOCOL  *BlockIo;

      Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
      if (EFI_ERROR (Status) || (BlockIo->Media == NULL) ||
          !BlockIo->Media->LogicalPartition)
      {
        continue;
      }

      PartitionCount++;
      BdsPramStr (" p");
      BdsPramHex ((UINT32)Index, 2);
      BdsPramByte ('=');
      BdsPramHex ((UINT32)BlockIo->Media->LastBlock, 8);
      BdsPramByte ('b');
      BdsPramHex ((UINT32)BlockIo->Media->BlockSize, 4);
      BdsPramByte ('r');

      if (Star2LteBlockIoLooksLikeFat (BlockIo, &FatWhy, &FatRead)) {
        BdsPramHex (FatWhy, 1);
        BdsPramByte ('F');
        gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
        FatConnected++;
      } else {
        BdsPramHex (FatWhy, 1);
        if (FatWhy == 1) {
          //
          // The read itself failed. "Which error" is the whole finding here,
          // so carry the EFI_STATUS rather than just the fact of failure.
          //
          BdsPramByte ('s');
          BdsPramHex ((UINT32)(FatRead & 0xFF), 2);
        }
      }
    }

    BdsPramStr (" lp=");
    BdsPramHex ((UINT32)PartitionCount, 2);

    //
    // FAIL-SAFE: if the BPB probe matched nothing, connect every BlockIo
    // handle recursively -- including whole-disk handles, so PartitionDxe
    // still gets a chance when no logical partition exists at all -- and let
    // Fat itself decide. A defect in the probe, or a missing partition
    // enumeration, must never be able to strand BDS with zero filesystems;
    // that is precisely the failure mode the hardcoded-geometry test produced.
    //
    if (FatConnected == 0) {
      BdsPramStr (" FBALL");
      for (Index = 0; Index < HandleCount; Index++) {
        gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
      }
    }

    BdsPramStr (" fc=");
    BdsPramHex ((UINT32)FatConnected, 2);
    BdsPramByte ('\n');

    FreePool (Handles);
  }

  if (UfsConnect != NULL) {
    *UfsConnect = LastUfsConnect;
  }

  if (ExtScsiConnect != NULL) {
    *ExtScsiConnect = LastExtConnect;
  }

  if (ScsiIoConnect != NULL) {
    *ScsiIoConnect = LastScsiIoConnect;
  }
}

STATIC
VOID
RegisterWindowsBootOption (
  VOID
  )
{
  EFI_STATUS                        Status;
  UINTN                             HandleCount;
  EFI_HANDLE                        *HandleBuffer;
  UINTN                             Index;
  UINTN                             PathIndex;
  EFI_DEVICE_PATH_PROTOCOL          *BootPath;
  EFI_BOOT_MANAGER_LOAD_OPTION      Option;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL   *Fs;
  EFI_FILE_PROTOCOL                 *Root;
  EFI_FILE_PROTOCOL                 *File;
  CHAR16                            *BootFilePath;
  STATIC CHAR16                     *BootFilePaths[] = {
    WINDOWS_BOOTMGR_PATH,
    REMOVABLE_BOOT_PATH,
    WINDOWS_BOOTMGR_ABS,
    REMOVABLE_BOOT_ABS
  };

  HandleBuffer = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "BDS: no filesystems yet (UFS not up?).\n"));
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    HandleBuffer[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Fs
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    BootFilePath = NULL;
    for (PathIndex = 0; PathIndex < (sizeof (BootFilePaths) / sizeof (BootFilePaths[0])); PathIndex++) {
      Status = Root->Open (Root, &File, BootFilePaths[PathIndex], EFI_FILE_MODE_READ, 0);
      if (!EFI_ERROR (Status)) {
        File->Close (File);
        BootFilePath = BootFilePaths[PathIndex];
        break;
      }
    }
    Root->Close (Root);
    if (BootFilePath == NULL) {
      continue;
    }

    BootPath = BuildBootFilePath (HandleBuffer[Index], BootFilePath);
    if (BootPath == NULL) {
      continue;
    }

    Star2LteDeleteBootOptions ();

    Status = EfiBootManagerInitializeLoadOption (
               &Option,
               LoadOptionNumberUnassigned,
               LoadOptionTypeBoot,
               LOAD_OPTION_ACTIVE,
               WINDOWS_BOOT_OPTION,
               BootPath,
               NULL,
               0
               );
    if (!EFI_ERROR (Status)) {
      // Insert at position 0 so Windows is the default.
      Status = EfiBootManagerAddLoadOptionVariable (&Option, 0);
      BdsPramStr ("\n=BMREG fp=");
      BdsPramStr16Ascii (BootFilePath, 36);
      BdsPramStr (" add=");
      BdsPramHex ((UINT32)(Status & 0xFFu), 2);
      BdsPramStr (" opt=");
      BdsPramHex ((UINT32)Option.OptionNumber, 4);
      BdsPramByte ('\n');
      EfiBootManagerFreeLoadOption (&Option);
      if (!EFI_ERROR (Status)) {
        mWindowsBootmgrFound = TRUE;
        DEBUG ((DEBUG_INFO, "BDS: registered Windows Boot Manager.\n"));
      }
    }

    FreePool (BootPath);
    break;  // first filesystem with a boot file wins
  }

  FreePool (HandleBuffer);
}

STATIC
BOOLEAN
Star2LteBootRegisteredBootmgr (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;
  UINTN                         OptionCount;
  UINTN                         Index;
  BOOLEAN                       Booted;

  OptionCount = 0;
  Booted      = FALSE;
  Options     = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);
  BdsPramStr ("\n=BMBOOT n=");
  BdsPramHex ((UINT32)OptionCount, 2);
  if (Options == NULL) {
    BdsPramStr (" none\n");
    return FALSE;
  }

  for (Index = 0; Index < OptionCount; Index++) {
    if ((Options[Index].Description != NULL) && (StrCmp (Options[Index].Description, WINDOWS_BOOT_OPTION) == 0)) {
      BdsPramStr (" opt=");
      BdsPramHex ((UINT32)Options[Index].OptionNumber, 4);
      BdsPramByte ('\n');
      Star2LteBootWatchdogArm ();
      EfiBootManagerBoot (&Options[Index]);
      Star2LteBootWatchdogDisarm ();
      BdsPramStr ("\n=BMBOOTRET st=");
      BdsPramByte (EFI_ERROR (Options[Index].Status) ? (UINT8)'E' : (UINT8)'S');
      BdsPramHex ((UINT32)(Options[Index].Status & 0xFFFFu), 4);
      BdsPramByte ('\n');
      Booted = TRUE;
      break;
    }
  }

  EfiBootManagerFreeLoadOptions (Options, OptionCount);
  return Booted;
}

STATIC
VOID
Star2LteDirectBootRootBootmgr (
  VOID
  )
{
  EFI_STATUS                       Status;
  UINTN                            HandleCount;
  EFI_HANDLE                       *HandleBuffer;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_DEVICE_PATH_PROTOCOL         *BootPath;
  EFI_DEVICE_PATH_PROTOCOL         *FullPath;
  VOID                             *FileBuffer;
  UINTN                            FileSize;
  EFI_HANDLE                       ImageHandle;
  EFI_LOADED_IMAGE_PROTOCOL        *LoadedImage;
  UINTN                            ExitDataSize;
  CHAR16                           *ExitData;
  UINTN                            PathIndex;
  STATIC CHAR16                    *BootFilePaths[] = {
    WINDOWS_BOOTMGR_PATH,
    REMOVABLE_BOOT_PATH,
    WINDOWS_BOOTMGR_ABS,
    REMOVABLE_BOOT_ABS
  };

  HandleBuffer = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  BdsPramStr ("\n=DBOOT sfs=");
  BdsPramByte (EFI_ERROR (Status) ? (UINT8)'E' : (UINT8)'S');
  BdsPramHex ((UINT32)(Status & 0xFFFFu), 4);
  BdsPramStr (" n=");
  BdsPramHex ((UINT32)(EFI_ERROR (Status) ? 0 : HandleCount), 2);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Fs = NULL;
    Status = gBS->HandleProtocol (HandleBuffer[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Root = NULL;
    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    File = NULL;
    for (PathIndex = 0; PathIndex < ARRAY_SIZE (BootFilePaths); PathIndex++) {
      Status = Root->Open (Root, &File, BootFilePaths[PathIndex], EFI_FILE_MODE_READ, 0);
      if (!EFI_ERROR (Status)) {
        break;
      }
    }
    if (EFI_ERROR (Status) || (PathIndex >= ARRAY_SIZE (BootFilePaths))) {
      Root->Close (Root);
      continue;
    }
    File->Close (File);
    Root->Close (Root);

    BootPath = BuildBootFilePath (HandleBuffer[Index], BootFilePaths[PathIndex]);
    if (BootPath == NULL) {
      continue;
    }

    ImageHandle = NULL;
    FullPath    = NULL;
    FileSize    = 0;
    BdsPramStr (" path=");
    BdsPramStr16Ascii (BootFilePaths[PathIndex], 36);
    FileBuffer = EfiBootManagerGetLoadOptionBuffer (BootPath, &FullPath, &FileSize);
    BdsPramStr (" buf=");
    BdsPramHex ((FileBuffer != NULL) ? 1u : 0u, 1);
    BdsPramStr (" sz=");
    BdsPramHex ((UINT32)FileSize, 8);
    Status = gBS->LoadImage (TRUE, gImageHandle, (FullPath != NULL) ? FullPath : BootPath, FileBuffer, FileSize, &ImageHandle);
    BdsPramStr (" load=");
    BdsPramByte (EFI_ERROR (Status) ? (UINT8)'E' : (UINT8)'S');
    BdsPramHex ((UINT32)(Status & 0xFFFFu), 4);
    if (FileBuffer != NULL) {
      FreePool (FileBuffer);
    }
    if (FullPath != NULL) {
      FreePool (FullPath);
    }
    FreePool (BootPath);
    if (EFI_ERROR (Status)) {
      break;
    }

    LoadedImage = NULL;
    Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
    BdsPramStr (" li=");
    BdsPramByte (EFI_ERROR (Status) ? (UINT8)'E' : (UINT8)'S');
    BdsPramHex ((UINT32)(Status & 0xFFFFu), 4);
    if (!EFI_ERROR (Status) && (LoadedImage != NULL)) {
      LoadedImage->ParentHandle = NULL;
    }

    Star2LteBootWatchdogArm ();
    ExitDataSize = 0;
    ExitData     = NULL;
    Status = gBS->StartImage (ImageHandle, &ExitDataSize, &ExitData);
    Star2LteBootWatchdogDisarm ();
    BdsPramStr (" startret=");
    BdsPramByte (EFI_ERROR (Status) ? (UINT8)'E' : (UINT8)'S');
    BdsPramHex ((UINT32)(Status & 0xFFFFu), 4);
    BdsPramStr (" xsz=");
    BdsPramHex ((UINT32)ExitDataSize, 8);
    if (ExitData != NULL) {
      BdsPramStr (" xd=");
      BdsPramStr16Ascii (ExitData, 48);
    }
    break;
  }

  FreePool (HandleBuffer);
  BdsPramByte ('\n');
}

//
// DIAG: report the storage enumeration ON THE PANEL (the pram trail is clobbered
// by the secure-world reset dump, so the screen is our reliable channel). Counts
// the BlockIo handles (raw controllers + logical partitions) and the
// SimpleFileSystem handles, and whether bootmgfw.efi was located — this tells us
// whether the Star2Lte UFS host -> UfsPassThru -> ScsiDisk -> Partition -> Fat
// chain actually produced the Windows ESP.
//
STATIC
VOID
ReportStorageOnScreen (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       BlockIoCount;
  UINTN       PartitionCount;
  UINTN       FsCount;
  UINTN       UfsHcCount;
  UINTN       ExtScsiCount;
  UINTN       ScsiIoCount;
  EFI_STATUS  UfsConnect;
  EFI_STATUS  ExtScsiConnect;
  EFI_STATUS  ScsiIoConnect;
  UINTN       Index;
  UINTN       HandleCount;
  EFI_HANDLE  *Handles;
  CHAR16      Line[1024];

  if (gST->ConOut == NULL) {
    return;
  }

  Star2LteConnectUfsScsiStack (&UfsHcCount, &ExtScsiCount, &ScsiIoCount, NULL, &UfsConnect, &ExtScsiConnect, &ScsiIoConnect);

  //
  // BlockIo handles: total + how many are logical partitions (have media that
  // is the "logical" kind). We approximate partitions by re-locating via the
  // BlockIo media LogicalPartition flag.
  //
  BlockIoCount   = 0;
  PartitionCount = 0;
  Handles        = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEfiBlockIoProtocolGuid, NULL,
                  &HandleCount, &Handles
                  );
  if (!EFI_ERROR (Status)) {
    BlockIoCount = HandleCount;
    for (Index = 0; Index < HandleCount; Index++) {
      EFI_BLOCK_IO_PROTOCOL  *BlockIo;
      Status = gBS->HandleProtocol (
                      Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo
                      );
      if (!EFI_ERROR (Status) && (BlockIo->Media != NULL) &&
          BlockIo->Media->LogicalPartition) {
        PartitionCount++;
      }
    }
    FreePool (Handles);
  }

  //
  // SimpleFileSystem handles (mountable volumes — the ESP shows up here).
  //
  FsCount = 0;
  Handles = NULL;
  Status  = gBS->LocateHandleBuffer (
                   ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL,
                   &HandleCount, &Handles
                   );
  if (!EFI_ERROR (Status)) {
    FsCount = HandleCount;
    FreePool (Handles);
  }

  BdsPramStr ("\n=WINFS A20 blk=");
  BdsPramHex ((UINT32)BlockIoCount, 2);
  BdsPramStr (" part=");
  BdsPramHex ((UINT32)PartitionCount, 2);
  BdsPramStr (" fs=");
  BdsPramHex ((UINT32)FsCount, 2);
  //
  // These were computed by Star2LteConnectUfsScsiStack() and then discarded,
  // which is why every previous capture could say "no filesystem" but not say
  // WHERE the UfsHc -> ExtScsiPassThru -> ScsiIo -> BlockIo chain broke.
  //
  BdsPramStr (" uh=");
  BdsPramHex ((UINT32)UfsHcCount, 2);
  BdsPramStr (" ex=");
  BdsPramHex ((UINT32)ExtScsiCount, 2);
  BdsPramStr (" sio=");
  BdsPramHex ((UINT32)ScsiIoCount, 2);
  BdsPramStr (" uc=");
  BdsPramHex ((UINT32)(UfsConnect & 0xFFu), 2);
  BdsPramStr (" ec=");
  BdsPramHex ((UINT32)(ExtScsiConnect & 0xFFu), 2);
  BdsPramStr (" sc=");
  BdsPramHex ((UINT32)(ScsiIoConnect & 0xFFu), 2);
  BdsPramByte ('\n');


#if !STAR2LTE_FAST_STORAGE_ENUM
  {
    EFI_HANDLE             *BiH;
    UINTN                  BiN;
    UINTN                  Bi;
    EFI_BLOCK_IO_PROTOCOL  *Bio;
    EFI_STATUS             Bs;
    UINT8                  *Sec;

    BiH = NULL;
    BiN = 0;
    if (!EFI_ERROR (gBS->LocateHandleBuffer (
                           ByProtocol, &gEfiBlockIoProtocolGuid, NULL,
                           &BiN, &BiH))) {
      for (Bi = 0; Bi < BiN; Bi++) {
        Bs = gBS->HandleProtocol (BiH[Bi], &gEfiBlockIoProtocolGuid, (VOID **)&Bio);
        if (EFI_ERROR (Bs) || (Bio->Media == NULL) || !Bio->Media->LogicalPartition) {
          continue;
        }

        Sec = AllocateZeroPool (Bio->Media->BlockSize);
        if (Sec == NULL) {
          continue;
        }

        Bs = Bio->ReadBlocks (Bio, Bio->Media->MediaId, 0, Bio->Media->BlockSize, Sec);
        BdsPramStr ("p");
        BdsPramHex ((UINT32)Bi, 2);
        BdsPramStr (" rd=");
        BdsPramHex ((UINT32)Bs, 2);
        BdsPramStr (" sig=");
        BdsPramHex (((UINT32)Sec[510] << 8) | (UINT32)Sec[511], 4);
        BdsPramStr (" t=");
        BdsPramHex ((UINT32)Sec[82], 2);
        BdsPramHex ((UINT32)Sec[83], 2);
        BdsPramHex ((UINT32)Sec[84], 2);
        BdsPramByte ('\n');
        FreePool (Sec);
      }

      FreePool (BiH);
    }
  }
#endif

  UnicodeSPrint (
    Line, sizeof (Line),
    L"  Storage: BlockIo=%u (partitions=%u)  FileSystems=%u\r\n",
    (UINT32)BlockIoCount, (UINT32)PartitionCount, (UINT32)FsCount
    );
  gST->ConOut->OutputString (gST->ConOut, Line);

  gST->ConOut->OutputString (
    gST->ConOut,
    mWindowsBootmgrFound
      ? L"  Windows bootmgfw.efi: FOUND — launching Windows...\r\n"
      : L"  Windows bootmgfw.efi: not found (no UFS ESP yet).\r\n"
    );

  //
  // CYCLE A8: live PMA RX regs (packed by the UFS driver into D[70]/D[71]) so the
  // working-kernel dump diff is visible on the panel even without a serial jig.
  // rest byte order = [0x004 0x0F0 0x0C4 0x0E8], want ~ 00 7F D9 77 (kernel steady).
  //
  {
    volatile UINT32  *Dpma = (volatile UINT32 *)(UINTN)0xFED13000ULL;
    CHAR16           PmaLine[120];
    UnicodeSPrint (
      PmaLine, sizeof (PmaLine),
      L"  PMA rest[004 0F0 0C4 0E8]=%08x  adapt[010l 010h 014l 014h]=%08x\r\n",
      Dpma[70], Dpma[71]
      );
    gST->ConOut->OutputString (gST->ConOut, PmaLine);
  }

  //
  // Raw UFS host controller register dump (UFSHCI standard offsets, read
  // directly at EXYNOS_UFS_HCI_BASE = 0x11120000). This bypasses every driver
  // and tells us WHERE the chain broke:
  //   CAP/VER  — if 0x00000000 or 0xFFFFFFFF, MMIO/clock dead (or RKP blocks).
  //              A sane CAP (NUTRS in low bits) + VER (~0x0210/0x0300) = host alive.
  //   HCE.bit0 — host controller enabled (UfsPassThru sets this in its init).
  //   HCS.DP   — device present (UFS link up). DP=0 => link down => sboot tore it
  //              down or we must run PHY/UniPro startup ourselves.
  //   HCS.UCRDY— UIC command ready.
  //
  {
    volatile UINT32  *Ufs = (volatile UINT32 *)(UINTN)0x11120000ULL;
    UINT32           Cap  = Ufs[0x00 / 4];
    UINT32           Ver  = Ufs[0x08 / 4];
    UINT32           Is   = Ufs[0x20 / 4];
    UINT32           Hcs  = Ufs[0x30 / 4];
    UINT32           Hce  = Ufs[0x34 / 4];

    UnicodeSPrint (
      Line, sizeof (Line),
      L"  UFS HC @11120000: CAP=%08x VER=%08x HCE=%08x\r\n"
      L"  UFS HCS=%08x  IS=%08x  DevPresent=%u LinkRdy=%u\r\n",
      Cap, Ver, Hce, Hcs, Is,
      (UINT32)((Hcs & BIT0) != 0),   // HCS.DP  = device present
      (UINT32)((Hcs & BIT3) != 0)    // HCS.UCRDY = UIC ready
      );
    gST->ConOut->OutputString (gST->ConOut, Line);
  }

  //
  UnicodeSPrint (
    Line, sizeof (Line),
    L"  UfsHcHandles=%u  ExtScsiPassThru=%u  ScsiIo=%u  Connect=%r/%r/%r\r\n",
    (UINT32)UfsHcCount, (UINT32)ExtScsiCount, (UINT32)ScsiIoCount, UfsConnect, ExtScsiConnect, ScsiIoConnect
    );
  gST->ConOut->OutputString (gST->ConOut, Line);

#if !STAR2LTE_FAST_STORAGE_ENUM
  //
  // *** STAR2LTE PartReadProbe: read each partition's FIRST block directly to isolate why a
  // FAT volume did not mount (FileSystems=0): readOK = # partitions our UFS driver could read,
  // fatSig = # of those with a 55AA boot signature, fatReadSt = the FAT partition's read status,
  // bytesPerSec = its BPB sector size. read-fail => UFS-driver issue; readOK+fatSig but no mount
  // => Fat-driver reject.
  //
  {
    UINTN                  ProbeRead;
    UINTN                  ProbeFat;
    EFI_STATUS             ProbeSt;
    UINT32                 ProbeBps;
    UINT32                 ProbeClk;
    EFI_STATUS             DiskLoSt;
    EFI_STATUS             DiskHiSt;
    UINT8                  *ProbeBuf;
    EFI_BLOCK_IO_PROTOCOL  *Bio;
    volatile UINT32        *D;

    D         = (volatile UINT32 *)(UINTN)0xFED13000ULL;
    //
    // *** CYCLE A5: HCS (UFSHCI 0x11120030) at the VERY START of the probe, before any probe read ->
    // D[268] (panel hcs0). bit1 UTRLRDY: 0 => transfer list ALREADY HALTED during/at end of enumeration
    // (the halt is NOT our probe); 1 => list ALIVE here => the probe reads themselves trigger the halt.
    //
    D[268]    = *(volatile UINT32 *)(UINTN)0x0000000011120030ULL;
    ProbeRead = 0;
    ProbeFat  = 0;
    ProbeSt   = EFI_NOT_FOUND;
    ProbeBps  = 0;
    ProbeClk  = 0;
    DiskLoSt  = EFI_NOT_FOUND;
    DiskHiSt  = EFI_NOT_FOUND;
    ProbeBuf  = AllocatePages (1);
    Handles   = NULL;

    if ((ProbeBuf != NULL) &&
        !EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &Handles)))
    {
      for (Index = 0; Index < HandleCount; Index++) {
        if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Bio)) ||
            (Bio->Media == NULL))
        {
          continue;
        }

        if (!Bio->Media->LogicalPartition) {
          //
          // *** CYCLE 91: whole-disk LOW vs HIGH LBA read - decisive about the Device-Error failure:
          // loLBA OK + hiLBA fail => LBA-dependent (high-LBA reads wedge); loLBA fail => controller dead
          // by BDS (cumulative wedge); both OK (but partition reads fail) => partition-BlockIo path bug.
          //
          if ((DiskLoSt == EFI_NOT_FOUND) && Bio->Media->MediaPresent && (Bio->Media->LastBlock > 0x20000ULL)) {
            //
            // *** CYCLE A10 LBA-SWEEP -> persistent RAM (pmsg-ramoops-0). *** PHY is EXONERATED
            // (A9 PMADUMP byte-identical to the working kernel), so map WHERE the read fails: read a
            // spread of LBAs (LBA0 twice for determinism, then low + deep), recording each EFI_STATUS
            // low byte + HCS-after (bit1 UTRLRDY: 0 => transfer list WEDGED) + first data byte (0xEE = no
            // data written). Fails by LBA/region => device/media; wedge-after-N / non-deterministic => host.
            //
            {
              STATIC CONST UINT64  SweepLba[] = { 0, 0, 1, 8, 34, 2048, 0x10000, 0x100000, 0x1000000 };
              UINTN       SwIdx;
              EFI_STATUS  SwSt;
              UINT32      SwHcs;

              // CYCLE A11: mirror UfsPassThru's captured stuck-read failure state (D[] @0xFED13000,
              // written during enumeration) to pram FIRST, before the sweep's own BlockIo reads can
              // overwrite those slots. This surfaces the full transport state (resp UPIU, OCS, IS, HCS,
              // UEC counters) in pmsg-ramoops-0 so we can see WHAT halted the UTP list - no photo needed.
              {
                volatile UINT32      *Dg = (volatile UINT32 *)(UINTN)0xFED13000ULL;
                STATIC CONST UINT16  DgSlot[] = { 12, 19, 21, 22, 23, 24, 30, 32, 33, 34, 36, 81, 268 };
                UINTN                DgI;
                BdsPramStr ("\n=DIAG A11 setst=D12 Qcmd=D19 resp=D21 ocs=D22 IS=D23 HCS=D24 uPA=D30 uDL=D32 uTR=D33 recov=D34 rePMC=D36 hcs0=D268=\n");
                for (DgI = 0; DgI < ARRAY_SIZE (DgSlot); DgI++) {
                  BdsPramStr ("D");
                  BdsPramHex (DgSlot[DgI], 3);
                  BdsPramStr ("=");
                  BdsPramHex (Dg[DgSlot[DgI]], 8);
                  BdsPramByte ((UINT8)'\n');
                }
              }
              BdsPramStr ("\n=LBASWEEP A10 (ERR low byte 07=DEVERR; hcs bit1=UTRLRDY)=\n");
              for (SwIdx = 0; SwIdx < ARRAY_SIZE (SweepLba); SwIdx++) {
                ProbeBuf[0] = 0xEE;
                SwSt  = Bio->ReadBlocks (Bio, Bio->Media->MediaId, SweepLba[SwIdx], Bio->Media->BlockSize, ProbeBuf);
                SwHcs = *(volatile UINT32 *)(UINTN)0x0000000011120030ULL;
                BdsPramStr ("lba=");
                BdsPramHex ((UINT32)SweepLba[SwIdx], 8);
                BdsPramStr (EFI_ERROR (SwSt) ? " ERR=" : " OK_=");
                BdsPramHex ((UINT32)(SwSt & 0xFFu), 2);
                BdsPramStr (" hcs=");
                BdsPramHex (SwHcs, 8);
                BdsPramStr (" b0=");
                BdsPramHex (ProbeBuf[0], 2);
                BdsPramByte ((UINT8)'\n');
              }
            }
            //
            // *** CYCLE A12: kernel-style UTRL RESTART test. *** A11 proved the list is halted (UTRLRDY=0)
            // by probe time with NO fatal error. The kernel's ufshcd_make_hba_operational restarts a halted
            // list by writing the RUN_STOP bit to REG_UTP_TRANSFER_REQ_LIST_RUN_STOP (0x60) (and 0x70 for the
            // task list) after clearing IS. Try exactly that, then re-read LBA0: if UTRLRDY recovers (HCS bit1)
            // and the read succeeds, restart+retry IS the fix to graft into the recovery path.
            //
            {
              volatile UINT32  *Hci  = (volatile UINT32 *)(UINTN)0x0000000011120000ULL;
              UINT32           IsB   = Hci[0x20u / 4u];
              UINT32           RsrB  = Hci[0x60u / 4u];
              EFI_STATUS       ReRd;

              BdsPramStr ("\n=UTRLRESTART A12=\nbefore IS=");
              BdsPramHex (IsB, 8);
              BdsPramStr (" RSR=");
              BdsPramHex (RsrB, 8);
              BdsPramStr (" HCS=");
              BdsPramHex (Hci[0x30u / 4u], 8);
              Hci[0x20u / 4u] = IsB;      // clear IS (write-1-to-clear)
              Hci[0x70u / 4u] = 0x1u;     // UTMRLRSR run
              Hci[0x60u / 4u] = 0x1u;     // UTRLRSR run -> restart the transfer list
              __asm__ __volatile__ ("dsb sy" ::: "memory");
              gBS->Stall (2000);
              BdsPramStr ("\nafter  HCS=");
              BdsPramHex (Hci[0x30u / 4u], 8);
              BdsPramStr (" RSR=");
              BdsPramHex (Hci[0x60u / 4u], 8);
              ProbeBuf[0] = 0xEE;
              ReRd = Bio->ReadBlocks (Bio, Bio->Media->MediaId, 0, Bio->Media->BlockSize, ProbeBuf);
              BdsPramStr ("\nreread LBA0 ");
              BdsPramStr (EFI_ERROR (ReRd) ? "ERR=" : "OK_=");
              BdsPramHex ((UINT32)(ReRd & 0xFFu), 2);
              BdsPramStr (" hcs=");
              BdsPramHex (Hci[0x30u / 4u], 8);
              BdsPramStr (" b0=");
              BdsPramHex (ProbeBuf[0], 2);
              BdsPramByte ((UINT8)'\n');
            }
            DiskLoSt = Bio->ReadBlocks (Bio, Bio->Media->MediaId, 0, Bio->Media->BlockSize, ProbeBuf);
            DiskHiSt = Bio->ReadBlocks (Bio, Bio->Media->MediaId, 0x10000, Bio->Media->BlockSize, ProbeBuf);
          }

          continue;
        }

        Status = Bio->ReadBlocks (Bio, Bio->Media->MediaId, 0, Bio->Media->BlockSize, ProbeBuf);
        if (!EFI_ERROR (Status)) {
          ProbeRead++;
          if ((ProbeBuf[510] == 0x55) && (ProbeBuf[511] == 0xAA)) {
            ProbeFat++;
            if (ProbeSt != EFI_SUCCESS) {
              ProbeSt  = EFI_SUCCESS;
              ProbeBps = (UINT32)(ProbeBuf[11] | (ProbeBuf[12] << 8));
            }

          }
        } else if (ProbeSt == EFI_NOT_FOUND) {
          ProbeSt  = Status;
          ProbeClk = *(volatile UINT32 *)(UINTN)0x111211B0ULL; // CLKSTOP_CTRL (VS_BASE+0xB0) at the read failure
        }
      }

      FreePool (Handles);
    }

    if (ProbeBuf != NULL) {
      FreePages (ProbeBuf, 1);
    }

    UnicodeSPrint (
      Line, sizeof (Line),
      L"  PartReadProbe: readOK=%u/%u fatSig=%u rdSt=%r bps=%u clkAtFail=%08x\r\n"
      L"  WholeDisk: loLBA=%r  hiLBA(0x10000)=%r\r\n"
      L"  tag=%08x  uPA=%08x setst=%08x hsS=%08x\r\n"
      L"  hcs0=%08x tact2=%08x resp=%08x OCS=%08x\r\n",
      (UINT32)ProbeRead, (UINT32)PartitionCount, (UINT32)ProbeFat, ProbeSt, ProbeBps, ProbeClk,
      DiskLoSt, DiskHiSt,
      D[10],
      D[259], D[12], D[262],
      D[268], D[269], D[21], D[22]
      );
    gST->ConOut->OutputString (gST->ConOut, Line);

    //
    // A77: HC-driver first-media-read probes (Star2LteUfsHcDxe UfsHcMap/Unmap), now that the PWM probe is
    // removed so these one-shots capture the REAL BDS partition reads.
    //   rd0 D[39] = (MBR sig @0x1FE)<<16 | len. AA55xxxx => real MBR data LANDED at BDS (=> partitions=0 is a
    //               GPT/parse issue, NOT a DMA issue); 0000xxxx => read completed but NO data (DMA: never lands).
    //   x46 D[47] / g48 D[48] = GPT MBR detail / 'PART' (GPT header LBA1 read OK).  fall D[24] = WC-bounce
    //   bypass count (>0 => bounce-reuse bug).  why D[25] = last bypass reason | len.  bnc D[27] = bounce addr.
    //
    UnicodeSPrint (
      Line, sizeof (Line),
      L"  A77 rd0=%08x g48=%08x  rord=%08x ax=%08x tx=%08x rx=%08x\r\n"
      L"      (rd0 0000xxxx => data never lands; verify rord=A ax=98000003 tx=8000000C rx=C vs kernel)\r\n",
      D[39], D[48], D[43], D[44], D[45], D[46]
      );
    gST->ConOut->OutputString (gST->ConOut, Line);
  }
  #endif

  //
  // DIAG: read the UFS HC call counters our Star2LteUfsHcDxe wrote at
  // 0xFED13000. This shows how far UfsPassThru.Start() drove our host controller
  // before failing (ConnectController swallows the real Start error):
  //   GetBar>0  -> Start reached our HC (binding OK).
  //   Alloc=0   -> Start failed in controller-init/LINK STARTUP (before rings)
  //                => UfsControllerInit's HCE reset tore down sboot's link and
  //                   re-link failed => need Exynos PHY/UniPro vendor init.
  //   Alloc>0   -> rings allocated (link re-came-up); HCS6 shows link state then;
  //                Map/Wr/Rd>0 = DMA attempted => failure is DMA/transport.
  //
  {
    volatile UINT32  *D = (volatile UINT32 *)(UINTN)0xFED13000ULL;
    STATIC CONST CHAR16  *AllocKind[] = { L"UC", L"WC", L"cached" };
    UINT32           Bar   = D[0];
    UINT32           Alloc = D[1];
    UINT32           Map   = D[2];
    UINT32           Rd    = D[3];
    UINT32           Wr    = D[4];
    UINT32           Kind  = D[5];
    UINT32           Hcs6  = D[6];   // transfer doorbell rings (xfer count): ~5-6=stuck at 1st query, 20+=enum advancing
    UINT32           Ufsp7 = D[7];   // OCS of slots3..0 packed (byte each); 0F=unprocessed 00=ok 1-C=err
    UINT32           Reord = D[8];   // HCI_DATA_REORDER readback (expect 0xA)
    UINT32           Gpio9 = D[9];   // HCI_GPIO_OUT readback after device reset deassert (expect 0x1)
    UINT32           ClkPre = D[10]; // [31:24]=firmware CYCLE tag (0x58=PRDT entry size 12->4 to match EDK2's 16B entries, clean FMP passthrough); [23:0]=mclk period (ns) into the 3 PRD attrs (expect 0A=10)
    UINT32           ClkPost = D[11];// CYCLE35 tact: [23:16]=HwCap MIN_TACTIVATE, [15:8]=device PA_TActivate, [7:0]=host PA_TActivate (HwCap=00 => DME_GET 0x8F failed = TActivate never truly set)
    UINT32           ClkSt  = D[12]; // CYCLE35 setst: [31:24]=PA_PWRMode after PMC (0x11=HS ok), [23]=UPMS seen, [15:0]=HCS
    UINT32           Phase  = D[13]; // phase-trail bitmask (highest set bit = furthest phase)
    UINT32           PhyIso = D[14]; // CYCLE64 (panel iocoh): UFS DMA IO-coherency sysreg @0x11010700 read back AFTER RMW set of bits 8|9 (0x311 => write stuck, coherent DMA now ON; 0x11 => write silently ignored / TZ-write-locked)
    UINT32           Force  = D[15]; // CYCLE61 (panel iccB): bActiveICCLevel BEFORE our write (0x0F => device self-set max after reset = ICC exonerated)
    UINT32           VsIs   = D[16]; // CYCLE62 (panel ssu): SCSI START UNIT result [0]=TargetStatus(00=GOOD) [1]=HostAdapterStatus [2]=sense key [24]=EFI_ERROR (00000000 => SSU GOOD, LU active)
    UINT32           IsDb   = D[17]; // CYCLE60 (panel cdb0): LIVE READ CDB[0..3] @ UCD+16 = SCSI opcode + flags + LBA[31:16] (READ(10) => low byte 0x28)
    UINT32           HcsDb  = D[18]; // CYCLE60 (panel cdbL): LIVE READ CDB[6..9] @ UCD+22 = group|len_hi<<8|len_lo<<16|control<<24 (1-block => 0x00010000; 0 => malformed len-0)
    UINT32           QCmd   = D[19]; // waited-slot CMD UPIU dword0 (byte0=txtype: 0x16 query/0x00 NOP)
    UINT32           QResp  = D[20]; // CYCLE53: cmd UPIU Expected Data Transfer Length (big-endian; 4096 => 0x00100000); 0 => malformed (no data requested)
    UINT32           QRsp2  = D[21]; // CYCLE53: device RESPONSE UPIU dword0; 0 => device SILENT (no response); byte0~0x21 => device answered w/ status/sense
    UINT32           QOcs   = D[22]; // [7:0]OCS [15:8]txtype [16]doorbell-still-set
    UINT32           IsLive = D[23]; // LIVE IS at stuck poll (bit0 UTRCS stuck => completion-edge theory)
    UINT32           HcsLive = D[24];// LIVE HCS at stuck poll (DevPresent/LinkRdy held during stall?)
    UINT32           ClkStop = D[25];// LIVE CLKSTOP_CTRL at stuck (set bit=clock GATED: b0 UNIPRO_PCLK..b4 REFCLKOUT)
    UINT32           FhcsLv  = D[26];// LIVE FORCE_HCS at stuck (expect 0 = all clocks forced on)
    UINT32           AcgLv   = D[27];// LIVE UFS_ACG_DISABLE at stuck (bit0=HWACG disabled)
    UINT32           Iacr    = D[28];// LIVE UTRIACR(0x4C) at stuck (expect 0 = aggregation disabled)
    UINT32           IeLv    = D[29];// LIVE IE(0x24) at stuck (expect bit0=1 = transfer-compl intr enabled)
    UINT32           UecPa   = D[30];// LIVE UECPA bits accumulated during stall (PHY-adapter error)
    UINT32           UecPaC  = D[31];// UECPA non-zero poll count (high => CONTINUOUS PHY line-reset)
    UINT32           UecDl   = D[32];// LIVE UECDL accumulated (data-link error)
    UINT32           UecTr   = D[33];// LIVE UECT accumulated (transport error)
    UINT32           Recov   = D[34];// CYCLE44: # of kernel-style HS re-PMC LINERESET recoveries fired (>=1 = recovery ran)
    UINT32           Reiss   = D[35];// CYCLE45: mReissueMask - slots UTRLCLR+re-rung (READ command re-issued on restored HS)
    UINT32           RePmc   = D[36];// CYCLE44: last re-PMC result [23:16]=AFC lock [8]=UPMS [7:0]=PA_PWRMode (0x11 = HS restored)
    UINT32           UeClrA  = D[37];// CYCLE47: mReissueDoneMask - shrunk(512B) re-issue completed? [23:16]=OCS(00=success) [15:0]=slots
    UINT32           UtrlC   = D[38];// UTRLCLR [15:0]=last cleared bit-mask, [31:16]=clear-pass count
    UINT32           Rd0     = D[39];// CYCLE65 (panel rd0): direct init READ(10) LBA0 result [7:0]TargetStatus(00=GOOD) [15:8]HostAdapterStatus [23:16]senseKey [24]EFI_ERROR
    UINT32           RdSns   = D[40];// CYCLE65 (panel rdsns): [7:0]ASC [15:8]ASCQ [23:16]senseRespCode(0x70) [31:24]EFI_STATUS low byte (00=GOOD,12=TIMEOUT,07=DEV_ERR)
    UINT32           Rdb     = D[264];// CYCLE A2 (panel h8e): hibern8 ENTER status [7:0]=IS bits seen (0x40 UHES / 0x10 UPMS) [31:8]=poll spins (0=completed immediately, 60000=timed out)
    UINT32           RdbSns  = D[265];// CYCLE A2 (panel h8x): hibern8 EXIT status [7:0]=IS bits seen (0x20 UHXS / 0x10 UPMS) [31:8]=poll spins; both enter+exit completing => M-PHY RX re-adapted before EDK2's reads
    UINT32           RSecB   = D[43];// CYCLE77 (panel des0): RING-time live READ PRDT DW0 = data buffer addr LOW (= bounce buffer; 0 => addr never written into PRDT = PRDT-build bug)
    UINT32           RCtl    = D[44];// CYCLE77 (panel edtl): RING-time READ cmd-UPIU EDTL @Ucd+12 (expect 0x00100000 = 4096 BE; 0 => EDK2 requested no data)
    UINT32           RSecA   = D[45];// CYCLE77 (panel des3): RING-time live READ PRDT DW3 = [31:30]DAS [29:28]FAS [25:0]LENGTH; expect 0x00000FFF (DAS/FAS=0 bypass, len 4095); top nibble != 0 => FMP engages = bug
    UINT32           WSec    = D[47];// CYCLE87 (panel x46): LBA0 read = MBR sig @0x1FE [31:16] (AA55=valid) | protective-MBR entry type @0x1C2 [15:8] (EE=GPT) | read length/512 [7:0] (08=4KB block)
    UINT32           Gpt48   = D[48];// CYCLE87 (panel g48): 2nd dword of first "EFI "-starting read = PART(54524150) => GPT header (LBA1) read OK; 0 => PartitionDxe never got a valid GPT header
    UINT32           GMask   = D[49];// CYCLE89 (panel gMsk): [23:8]=BlockSize, [3:0]=GPT primary-header sub-check mask (b0 sig, b1 hdrCRC, b2 MyLBA==Lba, b3 entrySize); 0x10000F=all pass, missing bit=failing check; 0=PartitionValidGptTable(Lba1) never ran
    UINT32           GMyLba  = D[50];// CYCLE89 (panel gLBA): GPT primary header MyLBA field (expect 1)
    UINT32           GNumEnt = D[51];// CYCLE89 (panel gNE): [31:16]=NumberOfPartitionEntries [15:0]=SizeOfPartitionEntry (expect 0x00800080 = 128 x 128B)
    UINT32           GeCrc   = D[52];// CYCLE89 (panel geC): expected PartitionEntryArrayCRC32 from the header
    UINT32           GcCrc   = D[53];// CYCLE89 (panel gcC): COMPUTED entry-array CRC32 (==geC => entry array valid => GPT should mount; !=geC => entry data wrong; 0 => not reached)
    UINT32           GStg    = D[54];// CYCLE89 (panel gSt): entry-array stage 0x10 reached / 0x30 read+CRC done (0 => a header check failed first, read gMsk)
    UINT32           GReal   = D[55];// CYCLE8A (panel gREAL): # GPT entries with non-zero type GUID. 0 (with geC==gcC) => entry-iterate read returned zeros = read non-determinism vs the CRC read
    UINT32           GSkip   = D[56];// CYCLE8A (panel gSKIP): [31:24]OutOfRange [23:16]OsSpecific(Attr bit1) [15:8]Overlap [7:0]child handles INSTALLED OK
    UINT32           GAttr   = D[57];// CYCLE8A (panel gATTR): first real entry Attributes low32 (bit1=0x2 => No-Block-IO/OsSpecific => PartitionDxe skips it)
    UINT32           GStart  = D[58];// CYCLE8A (panel gST): first real entry StartingLBA low32
    UINT32           GEnd    = D[59];// CYCLE8A (panel gEN): first real entry EndingLBA low32
    UINT32           GLastU  = D[60];// CYCLE8A (panel gLU): header LastUsableLBA low32 (entry EndingLBA > this => OutOfRange skip)

    UnicodeSPrint (
      Line, sizeof (Line),
      L"  UfsHc calls: GetBar=%u Alloc=%u Map=%u Rd=%u Wr=%u buf=%s\r\n"
      L"  xfers=%08x  s3210OCS=%08x  REORDER=%08x  swrst=%08x\r\n"
      L"  CLKPRD use=%08x tact=%08x setst=%08x\r\n"
      L"  PHASE=%08x  iocoh=%08x  iccB=%08x  ssu=%08x\r\n"
      L"  rd0=%08x  rdsns=%08x\r\n"
      L"  h8e=%08x  h8x=%08x\r\n"
      L"  des0=%08x  edtl=%08x  des3=%08x  x46=%08x  g48=%08x\r\n"
      L"  gMsk=%08x  gLBA=%08x  gNE=%08x  geC=%08x  gcC=%08x  gSt=%08x\r\n"
      L"  gREAL=%08x gSKIP=%08x gATTR=%08x gST=%08x gEN=%08x gLU=%08x\r\n"
      L"  cdb0=%08x  cdbL=%08x\r\n"
      L"  Wcmd=%08x  EDTL=%08x  resp=%08x  Wstat=%08x\r\n"
      L"  IS@stuck=%08x  HCS@stuck=%08x\r\n"
      L"  clkstop=%08x  fhcs=%08x  acg=%08x  iacr=%08x  ie=%08x\r\n"
      L"  uPA=%08x  paCnt=%08x  uDL=%08x  uTR=%08x\r\n"
      L"  recov=%08x  reiss=%08x  utrlC=%08x  rePMC=%08x  done=%08x\r\n",
      Bar, Alloc, Map, Rd, Wr,
      AllocKind[(Kind < 3) ? Kind : 2],
      Hcs6, Ufsp7, Reord, Gpio9,
      ClkPre, ClkPost, ClkSt,
      Phase, PhyIso, Force, VsIs,
      Rd0, RdSns,
      Rdb, RdbSns,
      RSecB, RCtl, RSecA, WSec, Gpt48,
      GMask, GMyLba, GNumEnt, GeCrc, GcCrc, GStg,
      GReal, GSkip, GAttr, GStart, GEnd, GLastU,
      IsDb, HcsDb,
      QCmd, QResp, QRsp2, QOcs,
      IsLive, HcsLive,
      ClkStop, FhcsLv, AcgLv, Iacr, IeLv,
      UecPa, UecPaC, UecDl, UecTr,
      Recov, Reiss, UtrlC, RePmc, UeClrA
      );
    //
    // *** STAR2LTE (cycle 8F verbosity): the verbose UFS-bringup/GPT diagnostic above is no
    // longer PRINTED to the panel - that whole investigation is solved. The compact Storage /
    // bootmgfw / PartReadProbe lines are the live panel now. (String still composed to avoid a
    // large churny edit; harmless.)
    //
  }
}

//
// ----------------------------------------------------------------------------
// PSCI CPU_ON self-test (v8)
// ----------------------------------------------------------------------------
//
// Every ntoskrnl word patch in this image is inert against the deployed kernel
// (0/14 RVAs hold their expected original word, and the PSCI MemProtect pattern
// does not occur at all), so the firmware-side SMP suspects are exhausted. The
// one thing never measured is whether Samsung's EL3 monitor will actually bring
// a secondary up when asked from UEFI context. This does exactly that, for all
// seven secondaries in a single boot, and reports over PRAM.
//
// HVC is fatal here (uH sets HCR_EL2.HCD, so hvc #0 raises an undef at EL1) and
// FADT ArmBootArch reads 0x0001 = PSCI_COMPLIANT without PSCI_USE_HVC, so this
// is SMC-only by construction.
//
//
// DISABLED 2026-08-02 (boot-v10 -> v11). The selftest has served its purpose: the
// v9 on-screen panel measured CPU_ON ok=7/7 with markers 3/7, which exonerates the
// secure monitor and moves the single-core fault into Windows' ACPI ingestion.
// Keeping it enabled is now actively harmful for two reasons:
//   1. The started secondaries park in WFI and are still ON when Windows later
//      issues its own CPU_ON, which returns ALREADY_ON (-4) -- the selftest would
//      cause the exact symptom it was built to diagnose.
//   2. Replacing that park with PSCI_CPU_OFF (tried in v10) hung the platform at
//      the Samsung logo, i.e. inside BeforeConsole where the selftest runs.
// Set back to 1 only to re-measure PSCI, and only with the WFI park restored.
//
// RE-RUN 2026-08-03 as boot-v20, with the WFI park restored and paired with
// STAR2LTE_PSCI_HANDOFF_PROBE so no started core survived into Windows. It
// finally explained the v9 "3/7": the three markers are the A55s, and CPU_ON
// for MPIDR 0x100 never returns and takes the SoC down with it. Disabled again
// - running it now would only re-hang the platform on the big cluster.
//
#ifndef STAR2LTE_PSCI_SELFTEST
#define STAR2LTE_PSCI_SELFTEST  0
#endif

#if STAR2LTE_PSCI_SELFTEST

#define STAR2LTE_PSCI_VERSION        0x84000000u
#define STAR2LTE_PSCI_CPU_ON         0xC4000003u
#define STAR2LTE_PSCI_AFFINITY_INFO  0xC4000004u

#define STAR2LTE_PSCI_TARGET_COUNT   7u
#define STAR2LTE_PSCI_STUB_STRIDE    0x40u
#define STAR2LTE_PSCI_MARK_OFFSET    0x800u

//
// MPIDR affinity values straight out of the v7 MADT: little cluster 0x0..0x3
// (efficiency 1) and big cluster 0x100..0x103 (efficiency 0). The boot core
// measures MPIDR 0x81000000 => Aff1.Aff0 = 0.0, so 0x0 is us and everything
// below is a secondary.
//
STATIC CONST UINT32  mStar2LtePsciTargets[STAR2LTE_PSCI_TARGET_COUNT] = {
  0x00000001u, 0x00000002u, 0x00000003u,
  0x00000100u, 0x00000101u, 0x00000102u, 0x00000103u
};

//
// Results, recorded in BeforeConsole and rendered on the panel from
// AfterConsole. The PRAM ring at 0xFED14000 does NOT survive a full Windows
// boot (Windows reuses that DRAM), so once the device actually reaches Windows
// the ring is empty and the trace above reports nothing. The text console is
// the only channel that still works in that case.
//
// PSCI SUCCESS is 0, so a zeroed static is indistinguishable from success:
// mStar2LtePsciRan plus the 0xDEADDEAD pre-fill are the controls that keep a
// "0" from being vacuous.
//
STATIC BOOLEAN  mStar2LtePsciRan = FALSE;
STATIC UINT32   mStar2LtePsciVer = 0;
STATIC UINT32   mStar2LtePsciOn[STAR2LTE_PSCI_TARGET_COUNT];
STATIC UINT32   mStar2LtePsciMark[STAR2LTE_PSCI_TARGET_COUNT];

//
// AFFINITY_INFO sampled AFTER CPU_ON. 0 = ON, 1 = OFF, 2 = ON_PENDING. This is
// the discriminator for the v9 result (on=7/7 but mk=3/7): if the four cluster-1
// cores read ON here they powered up and simply never reached our stub, and if
// they read OFF then EL3 returned SUCCESS for a core it never actually started.
// Pre-filled with 0xDEAD so an unwritten slot cannot masquerade as "ON".
//
STATIC UINT32   mStar2LtePsciAff[STAR2LTE_PSCI_TARGET_COUNT];

/**
  Emit a self-contained AArch64 stub for one secondary.

  The secondary enters with the MMU and caches OFF, so it cannot use any of our
  page tables and cannot see anything still sitting in our caches. The marker
  address is therefore baked in as an immediate and the store goes straight to
  DRAM. Register x0 holds the PSCI context id on entry, which we record as a
  second, independent witness that this really is the CPU we asked for.

  @param[out]  Code      Buffer receiving the stub (>= 11 instructions).
  @param[in]   MarkAddr  Physical address of this CPU's 8-byte marker slot.
**/
STATIC
VOID
Star2LtePsciEmitStub (
  OUT UINT32  *Code,
  IN  UINT64  MarkAddr
  )
{
  UINTN  i;

  i = 0;
  Code[i++] = 0xD2800000u | (0u << 21) | (((UINT32)(MarkAddr >>  0) & 0xFFFFu) << 5) | 1u;  // MOVZ X1, #a0
  Code[i++] = 0xF2800000u | (1u << 21) | (((UINT32)(MarkAddr >> 16) & 0xFFFFu) << 5) | 1u;  // MOVK X1, #a1, LSL #16
  Code[i++] = 0xF2800000u | (2u << 21) | (((UINT32)(MarkAddr >> 32) & 0xFFFFu) << 5) | 1u;  // MOVK X1, #a2, LSL #32
  Code[i++] = 0xF2800000u | (3u << 21) | (((UINT32)(MarkAddr >> 48) & 0xFFFFu) << 5) | 1u;  // MOVK X1, #a3, LSL #48
  Code[i++] = 0x52800000u | (0u << 21) | (0xBEEFu << 5) | 2u;                               // MOVZ W2, #0xBEEF
  Code[i++] = 0x72800000u | (1u << 21) | (0x5A5Au << 5) | 2u;                               // MOVK W2, #0x5A5A, LSL #16
  Code[i++] = 0xB9000000u | (0u << 10) | (1u << 5) | 2u;                                    // STR  W2, [X1]
  Code[i++] = 0xB9000000u | (1u << 10) | (1u << 5) | 0u;                                    // STR  W0, [X1, #4]
  Code[i++] = 0xD5033F9Fu;                                                                  // DSB  SY
  //
  // Park in WFI and loop back to it. This is the v9 park, which measured
  // CPU_ON ok=7/7 without disturbing the platform.
  //
  // Do NOT hand the core back with PSCI_CPU_OFF here. v10 tried exactly that
  // (MOVZ W0,#2; MOVK W0,#0x8400,LSL#16; SMC #0 - encodings verified correct)
  // and the platform hung at the Samsung logo: Samsung's EL3 does not survive
  // CPU_OFF from a core it started this way. The old fallback made it worse,
  // because B .-4 after an SMC branches back onto the SMC and spin-hammers the
  // monitor. B .-4 after WFI is the correct idiom - it re-enters the wait.
  //
  // A core parked here is still ON, so any build that runs this selftest MUST
  // reset before Windows is entered; otherwise Windows' own CPU_ON returns
  // ALREADY_ON (-4) and the selftest manufactures the very defect it measures.
  //
  Code[i++] = 0xD503207Fu;                                                                  // WFI
  Code[i++] = 0x17FFFFFFu;                                                                  // B    .-4  (back to WFI)
}

/**
  Ask the secure monitor to start every secondary core and report what happened.

  Three independent instruments per CPU, so no single zero can be vacuous:
  AFFINITY_INFO before (expect 1 = OFF), the CPU_ON return code itself, and a
  marker the secondary writes with its own store (expect 5A5ABEEF plus the
  context id we handed it), cross-checked by AFFINITY_INFO after (expect 0 = ON).
**/
STATIC
VOID
Star2LtePsciSelfTest (
  VOID
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Page;
  UINTN                 Idx;
  UINTN                 Fn;
  UINTN                 A1;
  UINTN                 A2;
  UINTN                 A3;
  UINTN                 Val;
  UINT64                StubAddr;
  UINT64                MarkAddr;
  volatile UINT32       *Mark;

  BdsPramStr ("\n=PSCIST");

  for (Idx = 0; Idx < STAR2LTE_PSCI_TARGET_COUNT; Idx++) {
    mStar2LtePsciOn[Idx]   = 0xDEADDEADu;
    mStar2LtePsciMark[Idx] = 0xDEADDEADu;
    mStar2LtePsciAff[Idx]  = 0xDEADDEADu;
  }

  //
  // PSCI_VERSION first. This is the positive control for the whole test: if SMC
  // from UEFI never reaches the monitor, every CPU_ON failure below is an
  // artifact of the conduit rather than a statement about bring-up.
  //
  Val = ArmCallSmc0 ((UINTN)STAR2LTE_PSCI_VERSION, NULL, NULL, NULL);
  mStar2LtePsciVer = (UINT32)Val;
  mStar2LtePsciRan = TRUE;
  BdsPramStr ("\n=PSCIVER ");
  BdsPramHex64 ((UINT64)Val, 16);

  Page   = 0;
  Status = gBS->AllocatePages (AllocateAnyPages, EfiReservedMemoryType, 1, &Page);
  BdsPramStr ("\n=PSCIPG ");
  BdsPramHex64 ((UINT64)Page, 16);
  BdsPramStatusLite (Status);
  if (EFI_ERROR (Status) || (Page == 0)) {
    return;
  }

  ZeroMem ((VOID *)(UINTN)Page, EFI_PAGE_SIZE);

  for (Idx = 0; Idx < STAR2LTE_PSCI_TARGET_COUNT; Idx++) {
    Star2LtePsciEmitStub (
      (UINT32 *)(UINTN)((UINT64)Page + (Idx * STAR2LTE_PSCI_STUB_STRIDE)),
      (UINT64)Page + STAR2LTE_PSCI_MARK_OFFSET + (Idx * 8u)
      );
  }

  //
  // Publish every stub to the point of coherency before any secondary, running
  // with caches off, can fetch it.
  //
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Page, EFI_PAGE_SIZE);
  InvalidateInstructionCacheRange ((VOID *)(UINTN)Page, EFI_PAGE_SIZE);

  for (Idx = 0; Idx < STAR2LTE_PSCI_TARGET_COUNT; Idx++) {
    StubAddr = (UINT64)Page + (Idx * STAR2LTE_PSCI_STUB_STRIDE);
    MarkAddr = (UINT64)Page + STAR2LTE_PSCI_MARK_OFFSET + (Idx * 8u);
    Mark     = (volatile UINT32 *)(UINTN)MarkAddr;

    BdsPramStr ("\n=CPU ");
    BdsPramHex (mStar2LtePsciTargets[Idx], 8);

    Fn  = (UINTN)STAR2LTE_PSCI_AFFINITY_INFO;
    A1  = (UINTN)mStar2LtePsciTargets[Idx];
    A2  = 0;
    A3  = 0;
    Val = ArmCallSmc2 (Fn, &A1, &A2, &A3);
    BdsPramStr (" AFF0=");
    BdsPramHex64 ((UINT64)Val, 16);

    Fn  = (UINTN)STAR2LTE_PSCI_CPU_ON;
    A1  = (UINTN)mStar2LtePsciTargets[Idx];
    A2  = (UINTN)StubAddr;
    A3  = (UINTN)(0xC0DE0000u | mStar2LtePsciTargets[Idx]);
    Val = ArmCallSmc3 (Fn, &A1, &A2, &A3);
    mStar2LtePsciOn[Idx] = (UINT32)Val;
    BdsPramStr (" ON=");
    BdsPramHex64 ((UINT64)Val, 16);

    gBS->Stall (20000);

    //
    // The secondary stored with caches off, so drop our stale cached copy of the
    // marker before reading it back.
    //
    WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)MarkAddr, 8);
    mStar2LtePsciMark[Idx] = Mark[0];
    BdsPramStr (" MK=");
    BdsPramHex (Mark[0], 8);
    BdsPramStr (" CTX=");
    BdsPramHex (Mark[1], 8);

    Fn  = (UINTN)STAR2LTE_PSCI_AFFINITY_INFO;
    A1  = (UINTN)mStar2LtePsciTargets[Idx];
    A2  = 0;
    A3  = 0;
    Val = ArmCallSmc2 (Fn, &A1, &A2, &A3);
    mStar2LtePsciAff[Idx] = (UINT32)Val;
    BdsPramStr (" AFF1=");
    BdsPramHex64 ((UINT64)Val, 16);
  }

  BdsPramStr ("\n=PSCIEND");
}

/**
  Render the recorded PSCI CPU_ON results on the UEFI text console.

  This is the discriminator for the single-core symptom. Read it as:
    ver != 0 .......... SMC reaches the secure monitor (positive control). If
                        this is 0/garbage the conduit is broken and every
                        CPU_ON result below is meaningless.
    on=7/7 mk=7/7 ..... EL3 starts secondaries and they execute our code, so
                        bring-up works and the fault is entirely inside Windows
                        (ACPI ingestion / HAL), not the firmware or monitor.
    on=7/7 mk=0/7 ..... monitor accepts the call but the core never runs our
                        stub (entry state / cache / address problem).
    on=0/7 ............ monitor refuses CPU_ON; nothing Windows does can help.
                        -1 NOT_SUPPORTED, -2 INVALID_PARAMS, -3 DENIED,
                        -4 ALREADY_ON, -7 NOT_PRESENT, -9 INVALID_ADDRESS.
**/
STATIC
VOID
Star2LtePsciReportOnScreen (
  VOID
  )
{
  CHAR16  Line[160];
  UINTN   Idx;
  UINT32  OnOk;
  UINT32  MarkOk;

  if (gST->ConOut == NULL) {
    return;
  }

  if (!mStar2LtePsciRan) {
    gST->ConOut->OutputString (gST->ConOut, L"  PSCI selftest: DID NOT RUN\r\n");
    return;
  }

  OnOk   = 0;
  MarkOk = 0;
  for (Idx = 0; Idx < STAR2LTE_PSCI_TARGET_COUNT; Idx++) {
    if (mStar2LtePsciOn[Idx] == 0) {
      OnOk++;
    }

    if (mStar2LtePsciMark[Idx] == 0x5A5ABEEFu) {
      MarkOk++;
    }
  }

  UnicodeSPrint (
    Line, sizeof (Line),
    L"  PSCI ver=%08x  CPU_ON ok=%u/7  markers=%u/7\r\n",
    mStar2LtePsciVer, OnOk, MarkOk
    );
  gST->ConOut->OutputString (gST->ConOut, Line);

  UnicodeSPrint (
    Line, sizeof (Line),
    L"  on: %08x %08x %08x %08x %08x %08x %08x\r\n",
    mStar2LtePsciOn[0], mStar2LtePsciOn[1], mStar2LtePsciOn[2],
    mStar2LtePsciOn[3], mStar2LtePsciOn[4], mStar2LtePsciOn[5],
    mStar2LtePsciOn[6]
    );
  gST->ConOut->OutputString (gST->ConOut, Line);

  UnicodeSPrint (
    Line, sizeof (Line),
    L"  mk: %08x %08x %08x %08x %08x %08x %08x\r\n",
    mStar2LtePsciMark[0], mStar2LtePsciMark[1], mStar2LtePsciMark[2],
    mStar2LtePsciMark[3], mStar2LtePsciMark[4], mStar2LtePsciMark[5],
    mStar2LtePsciMark[6]
    );
  gST->ConOut->OutputString (gST->ConOut, Line);

  //
  // AFFINITY_INFO after CPU_ON: 0 = ON, 1 = OFF, 2 = ON_PENDING, 0xDEAD = the
  // loop never reached this target. Read alongside mk: ON with no marker means
  // the core powered but never ran our code; OFF after a SUCCESS return means
  // the monitor lied about starting it.
  //
  UnicodeSPrint (
    Line, sizeof (Line),
    L"  af: %08x %08x %08x %08x %08x %08x %08x\r\n",
    mStar2LtePsciAff[0], mStar2LtePsciAff[1], mStar2LtePsciAff[2],
    mStar2LtePsciAff[3], mStar2LtePsciAff[4], mStar2LtePsciAff[5],
    mStar2LtePsciAff[6]
    );
  gST->ConOut->OutputString (gST->ConOut, Line);
}

#endif // STAR2LTE_PSCI_SELFTEST

//
// ----------------------------------------------------------------------------
// ACPI parking protocol bring-up (boot-v22)
// ----------------------------------------------------------------------------
//
// WHY THIS EXISTS. Windows starts a secondary with
//
//     PSCI CPU_ON (mpidr, entry = HalpSecondaryEntry, context_id = mailboxPA)
//
// and its secondary trampoline reads the per-CPU mailbox physical address from
// x0 on entry and brings up its MMU (TTBR0/TTBR1/TCR/MAIR/TPIDR) through it.
//
// Samsung's EL3 does not deliver the PSCI context_id. boot-v20's selftest parked
// three A55s and each one recorded its entry x0 as 0xBFFF01C -- the same constant
// on all three, and not the 0xC0DE000x we passed. So every Windows secondary
// enters that trampoline with a bogus pointer, loads page tables and the KPCR out
// of arbitrary memory, enables the MMU and dies silently. CPU_ON still returns
// SUCCESS, the core never checks in, and KeStartAllProcessors gives up: NPACT=1.
//
// boot-v21 ruled out every remaining alternative. With a MADT that enabled only
// the four A55 cores we had *proved* startable (v20: ON=0 and a marker written by
// each core's own store), Windows still reported NPACT=1 while its per-processor
// array showed all four correctly enumerated. The defect is the x0 handoff, not
// the topology, not the cluster, and not the capability gate (PGATE=0xB).
//
// THE FIX. The ACPI parking protocol exists for exactly this: firmware, not the
// monitor, owns the handoff register. We start each secondary into a stub that
// knows its own mailbox address as a baked immediate, spins until the OS writes a
// jump address at mailbox+0x08, then enters the OS with x0 set *by us*. EL3 never
// gets to choose x0.
//
// This is fail-safe in both directions. ParkingProtocolVersion is only set for a
// core that CPU_ON actually accepted, so the MADT never advertises a park that did
// not happen. And the stub polls only our own page -- never the mailbox Windows
// allocates for itself -- so if this Windows build ignores the parking protocol
// the core simply stays parked forever instead of racing a CPU_ON that already
// gave up on it.
//
// The big cluster is deliberately excluded. CPU_ON for MPIDR 0x100 never returns
// and takes the SoC down with it (v20 ended mid-record at "=CPU 00000100 AFF0=",
// and the platform needed ~17 minutes of watchdog resets to reach recovery).
//
#ifndef STAR2LTE_PSCI_PARK
#define STAR2LTE_PSCI_PARK  1
#endif

#if STAR2LTE_PSCI_PARK

#define STAR2LTE_PARK_CPU_ON      0xC4000003u
#define STAR2LTE_PARK_COUNT       3u
#define STAR2LTE_PARK_STUB_OFF    0x800u
#define STAR2LTE_PARK_MARK_OFF    0x18u
#define STAR2LTE_PARK_ALIVE       0x5A5ABEEFu

//
// A55 secondaries only. MPIDR 0x100-0x103 (Mongoose M3) are excluded by
// construction; see the block comment above.
//
STATIC CONST UINT32  mStar2LteParkTargets[STAR2LTE_PARK_COUNT] = {
  0x00000001u, 0x00000002u, 0x00000003u
};

STATIC UINT64   mStar2LteParkPage[STAR2LTE_PARK_COUNT];
STATIC UINT32   mStar2LteParkOn[STAR2LTE_PARK_COUNT];
STATIC UINT32   mStar2LteParkAlive[STAR2LTE_PARK_COUNT];
STATIC UINT32   mStar2LteParkPatched = 0;

/**
  Emit the parking stub for one secondary.

  Runs with the MMU and caches OFF, so it can use neither our page tables nor
  anything still sitting in our caches: the mailbox address is baked in as an
  immediate and every access goes straight to DRAM.

  The DC CIVAC in the poll loop is what makes the handoff observable. A parked
  core reads with caches off, which does not snoop, so a cacheable write by the
  boot processor could otherwise sit in its L1 forever. Cleaning-and-invalidating
  the mailbox line to the point of coherency before each read forces any dirty
  copy out to DRAM first. The delay loop keeps three cores from hammering the
  interconnect for the whole of Windows' boot.

  @param[out]  Code      Buffer receiving the stub (>= 17 instructions).
  @param[in]   MailAddr  Physical address of this CPU's 4 KB mailbox page.
**/
STATIC
VOID
Star2LteParkEmitStub (
  OUT UINT32  *Code,
  IN  UINT64  MailAddr
  )
{
  UINTN  i;

  i = 0;
  Code[i++] = 0xD2800000u | (0u << 21) | (((UINT32)(MailAddr >>  0) & 0xFFFFu) << 5) | 1u;  // MOVZ X1, #a0
  Code[i++] = 0xF2800000u | (1u << 21) | (((UINT32)(MailAddr >> 16) & 0xFFFFu) << 5) | 1u;  // MOVK X1, #a1, LSL #16
  Code[i++] = 0xF2800000u | (2u << 21) | (((UINT32)(MailAddr >> 32) & 0xFFFFu) << 5) | 1u;  // MOVK X1, #a2, LSL #32
  Code[i++] = 0xF2800000u | (3u << 21) | (((UINT32)(MailAddr >> 48) & 0xFFFFu) << 5) | 1u;  // MOVK X1, #a3, LSL #48
  //
  // Liveness witness. Written with the core's own store before it starts
  // polling, so "parked" can never be inferred from the absence of evidence.
  //
  Code[i++] = 0x52800000u | (0u << 21) | (0xBEEFu << 5) | 2u;                               // MOVZ W2, #0xBEEF
  Code[i++] = 0x72800000u | (1u << 21) | (0x5A5Au << 5) | 2u;                               // MOVK W2, #0x5A5A, LSL #16
  Code[i++] = 0xB9001822u;                                                                  // STR  W2, [X1, #0x18]
  Code[i++] = 0xD5033F9Fu;                                                                  // DSB  SY
  Code[i++] = 0x52840003u;                                                                  // poll: MOVZ W3, #0x2000
  Code[i++] = 0x51000463u;                                                                  // delay: SUB W3, W3, #1
  Code[i++] = 0x35FFFFE3u;                                                                  // CBNZ W3, delay
  Code[i++] = 0xD50B7E21u;                                                                  // DC   CIVAC, X1
  Code[i++] = 0xD5033F9Fu;                                                                  // DSB  SY
  Code[i++] = 0xF9400422u;                                                                  // LDR  X2, [X1, #8]
  Code[i++] = 0xB4FFFF42u;                                                                  // CBZ  X2, poll
  Code[i++] = 0xAA0103E0u;                                                                  // MOV  X0, X1     <-- the fix
  Code[i++] = 0xD61F0040u;                                                                  // BR   X2
}

/**
  Publish the parked mailboxes in the MADT that is actually in the XSDT.

  Patches the runtime table rather than Madt.aslc because the mailbox pages are
  allocated at BDS time. Only entries whose CPU_ON succeeded are advertised, so
  the MADT cannot claim a park that did not happen, and the header checksum is
  recomputed because Windows validates it.
**/
STATIC
VOID
Star2LteParkPublishMadt (
  VOID
  )
{
  UINTN        Index;
  CONST UINT8  *Rsdp;
  CONST UINT8  *Xsdt;
  UINT64       XsdtAddress;
  UINT32       XsdtLength;
  UINTN        EntryCount;
  UINTN        EntryIndex;

  Rsdp = NULL;
  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &gEfiAcpi20TableGuid) &&
        (gST->ConfigurationTable[Index].VendorTable != NULL))
    {
      Rsdp = (CONST UINT8 *)gST->ConfigurationTable[Index].VendorTable;
      break;
    }
  }

  if (Rsdp == NULL) {
    BdsPramStr (" NORSDP");
    return;
  }

  XsdtAddress = Star2LteRdU64 (Rsdp + 24);
  if (XsdtAddress == 0) {
    BdsPramStr (" NOXSDT");
    return;
  }

  Xsdt       = (CONST UINT8 *)(UINTN)XsdtAddress;
  XsdtLength = Star2LteRdU32 (Xsdt + 4);
  if ((XsdtLength < 36u) || (XsdtLength > 0x1000u)) {
    BdsPramStr (" BADXSDT");
    return;
  }

  EntryCount = (XsdtLength - 36u) / sizeof (UINT64);
  for (EntryIndex = 0; EntryIndex < EntryCount; EntryIndex++) {
    UINT8   *Madt;
    UINT64  TableAddress;
    UINT32  Length;
    UINT32  Offset;
    UINT32  Sum;

    TableAddress = Star2LteRdU64 (Xsdt + 36u + (EntryIndex * sizeof (UINT64)));
    if (TableAddress == 0) {
      continue;
    }

    Madt = (UINT8 *)(UINTN)TableAddress;
    if (Star2LteRdU32 (Madt) != STAR2LTE_MADT_SIG) {
      continue;
    }

    Length = Star2LteRdU32 (Madt + 4);
    if ((Length < 44u) || (Length > 0x1000u)) {
      continue;
    }

    Offset = 44u;
    while ((Offset + 2u) <= Length) {
      UINT32  SubLen;

      SubLen = Madt[Offset + 1u];
      if ((SubLen < 2u) || ((Offset + SubLen) > Length)) {
        break;
      }

      if ((Madt[Offset] == 0x0Bu) && (SubLen >= 80u)) {
        UINT64  Mpidr;
        UINTN   Slot;

        Mpidr = Star2LteRdU64 (Madt + Offset + 68u);
        for (Slot = 0; Slot < STAR2LTE_PARK_COUNT; Slot++) {
          if ((mStar2LteParkOn[Slot] == 0) &&
              (mStar2LteParkPage[Slot] != 0) &&
              (Mpidr == (UINT64)mStar2LteParkTargets[Slot]))
          {
            UINT32  Version;

            //
            // GICC +16 ParkingProtocolVersion, +24 ParkedAddress (ACPI 6.3).
            //
            Version = 1u;
            CopyMem (Madt + Offset + 16u, &Version, sizeof (UINT32));
            CopyMem (Madt + Offset + 24u, &mStar2LteParkPage[Slot], sizeof (UINT64));
            mStar2LteParkPatched++;
          }
        }
      }

      Offset += SubLen;
    }

    Madt[9] = 0;
    Sum     = 0;
    for (Offset = 0; Offset < Length; Offset++) {
      Sum += Madt[Offset];
    }

    Madt[9] = (UINT8)((0x100u - (Sum & 0xFFu)) & 0xFFu);
  }
}

/**
  Start every A55 secondary into its parking stub and advertise it in the MADT.
**/
STATIC
VOID
Star2LteParkSecondaries (
  VOID
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Pages;
  UINTN                 Idx;
  UINTN                 Fn;
  UINTN                 A1;
  UINTN                 A2;
  UINTN                 A3;
  UINTN                 Val;

  BdsPramStr ("\n=PARK");

  for (Idx = 0; Idx < STAR2LTE_PARK_COUNT; Idx++) {
    mStar2LteParkPage[Idx]  = 0;
    mStar2LteParkOn[Idx]    = 0xDEADDEADu;
    mStar2LteParkAlive[Idx] = 0xDEADDEADu;
  }

  //
  // Below 4 GB and reserved, so Windows neither reuses the pages nor needs a
  // 64-bit mailbox mapping. AllocateMaxAddress rather than a fixed address:
  // a PEI-level carveout at a guessed PA regressed the boot before (ext46).
  //
  Pages  = 0xFFFFFFFFULL;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiReservedMemoryType,
                  STAR2LTE_PARK_COUNT,
                  &Pages
                  );
  BdsPramStr (" PG=");
  BdsPramHex64 ((UINT64)Pages, 16);
  BdsPramStatusLite (Status);
  if (EFI_ERROR (Status) || (Pages == 0)) {
    return;
  }

  ZeroMem ((VOID *)(UINTN)Pages, STAR2LTE_PARK_COUNT * EFI_PAGE_SIZE);

  for (Idx = 0; Idx < STAR2LTE_PARK_COUNT; Idx++) {
    UINT64  Mail;

    Mail                   = (UINT64)Pages + ((UINT64)Idx * EFI_PAGE_SIZE);
    mStar2LteParkPage[Idx] = Mail;

    //
    // Mailbox +0x00 is the processor id the OS matches against; the jump address
    // at +0x08 stays zero until the OS writes it.
    //
    *(volatile UINT32 *)(UINTN)Mail = mStar2LteParkTargets[Idx];
    Star2LteParkEmitStub (
      (UINT32 *)(UINTN)(Mail + STAR2LTE_PARK_STUB_OFF),
      Mail
      );
  }

  //
  // Publish every stub to the point of coherency before any secondary, running
  // with caches off, can fetch it.
  //
  WriteBackInvalidateDataCacheRange (
    (VOID *)(UINTN)Pages,
    STAR2LTE_PARK_COUNT * EFI_PAGE_SIZE
    );
  InvalidateInstructionCacheRange (
    (VOID *)(UINTN)Pages,
    STAR2LTE_PARK_COUNT * EFI_PAGE_SIZE
    );

  for (Idx = 0; Idx < STAR2LTE_PARK_COUNT; Idx++) {
    Fn = (UINTN)STAR2LTE_PARK_CPU_ON;
    A1 = (UINTN)mStar2LteParkTargets[Idx];
    A2 = (UINTN)(mStar2LteParkPage[Idx] + STAR2LTE_PARK_STUB_OFF);
    A3 = 0;

    Val                  = ArmCallSmc3 (Fn, &A1, &A2, &A3);
    mStar2LteParkOn[Idx] = (UINT32)Val;

    BdsPramStr ("\n=PCPU ");
    BdsPramHex (mStar2LteParkTargets[Idx], 8);
    BdsPramStr (" MB=");
    BdsPramHex64 (mStar2LteParkPage[Idx], 16);
    BdsPramStr (" ON=");
    BdsPramHex64 ((UINT64)Val, 16);
  }

  gBS->Stall (30000);

  for (Idx = 0; Idx < STAR2LTE_PARK_COUNT; Idx++) {
    volatile UINT32  *Mark;

    Mark = (volatile UINT32 *)(UINTN)(mStar2LteParkPage[Idx] + STAR2LTE_PARK_MARK_OFF);
    WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Mark, 4);
    mStar2LteParkAlive[Idx] = *Mark;
  }

  BdsPramStr ("\n=PALIVE");
  for (Idx = 0; Idx < STAR2LTE_PARK_COUNT; Idx++) {
    BdsPramByte (' ');
    BdsPramHex (mStar2LteParkAlive[Idx], 8);
  }

  BdsPramStr ("\n=PMADT");
  Star2LteParkPublishMadt ();
  BdsPramStr (" n=");
  BdsPramHex (mStar2LteParkPatched, 2);
  BdsPramStr ("\n=PARKEND");
}

#endif // STAR2LTE_PSCI_PARK

//
// ----------------------------------------------------------------------------
// STAR2LTE_PMU_PROBE -- Exynos 9810 PMU reconnaissance for big-cluster bring-up
// ----------------------------------------------------------------------------
//
// boot-v22 brought the four Cortex-A55s up. The four Mongoose M3s (MPIDR
// 0x100..0x103) remain unreachable: CPU_ON for 0x100 NEVER RETURNS and takes
// the SoC with it. boot-v20 proved this precisely -- its record ends at
//
//     =CPU 00000100 AFF0=00000000ffffffff
//
// i.e. the AFFINITY_INFO SMC before it returned and its result was printed in
// full, and then the CPU_ON SMC swallowed the calling core: " ON=" never
// appeared. The device needed ~17 minutes of watchdog resets to recover.
//
// The monitor is not the problem in the abstract -- Linux starts all eight
// cores through the very same call. The live device tree says so explicitly:
//
//     /psci: compatible="arm,psci", method="smc",
//            cpu_on=0xC4000003, cpu_off=0x84000002, cpu_suspend=0xC4000001
//     /cpus/cpu@0100: enable-method="psci", compatible="arm,meerkat arm,armv8"
//
// so both our function ID and our MPIDR encoding are exactly what Samsung's own
// kernel uses. What differs is SoC state: the M3 cluster is powered down when
// UEFI runs, and EL3's power-up sequence polls a PMU status handshake that
// never completes.
//
// Rather than guess the register map -- a wrong guess costs ~17 minutes per
// attempt -- this probe measures it. It is strictly READ-ONLY with respect to
// the PMU and never issues CPU_ON for a big core:
//
//   1. snapshot the whole 64 KB PMU block BEFORE any secondary is started;
//   2. let the (already proven) parking code start MPIDR 0x001/0x002/0x003;
//   3. snapshot the PMU block again;
//   4. report every word that changed.
//
// Step 4 is the payload. The registers that move when three little cores come
// up ARE the per-core power-control registers, so the diff yields the region
// base and the per-core stride directly, with no assumption about Samsung's
// layout. Extrapolating the same stride to cluster 1 gives the registers the
// big cores need -- and the census printed alongside it shows what those
// registers currently read, i.e. whether the M3 cluster is powered at all.
//
// The probe ends in Star2LteRecoveryReset(), deliberately: the pmsg ring is
// 16,128 bytes and the UFS driver's records overwrite every BDS record once
// Windows starts (verified on the v22 captures, whose ring begins at
// "=UFSINIT" with the entire BDS preamble gone). Returning to recovery instead
// of booting Windows is what makes this measurement readable at all, and it
// costs one ~1 minute cycle instead of a full boot.
//
// Ordering note: the ring is circular, so whatever is written LAST survives.
// The census is therefore printed before the diff, and the diff -- the actual
// result -- is printed immediately before the reset.
//

#ifndef STAR2LTE_PMU_PROBE
#define STAR2LTE_PMU_PROBE 0
#endif

#if STAR2LTE_PMU_PROBE

#define STAR2LTE_PMU_PROBE_BASE    0x14060000ULL

//
// boot-v23 swept the whole 64 KB block and never came back: its ring ends at
// "=PMUA.00", i.e. the marker for chunk 0 was printed and the read of PMU
// 0x0000..0x0FFF then wedged the bus. That page is therefore permanently
// excluded -- nothing in it is needed anyway, since the handful of chunk-0
// registers this port uses (SWRESET 0x400, WDT 0x408/0x40C, INFORM2/3
// 0x808/0x80C, SYSIP_DAT0 0x810) are already device-verified individually.
//
// The window below covers the region where every Exynos generation has kept
// its per-core power control: CONFIGURATION/STATUS pairs on an 0x80 stride
// from 0x2000, with the cluster's non-CPU block above them. boot-v24 proved
// page 1 (0x1000..0x1FFF) is fatal too -- its ring ends at "=PMUA.1000" -- so
// the sweep now starts at 0x2000 and pages 0 and 1 are both excluded.
//
#define STAR2LTE_PMU_PROBE_LO      0x2000u
#define STAR2LTE_PMU_PROBE_HI      0x4000u
#define STAR2LTE_PMU_PROBE_BYTES   (STAR2LTE_PMU_PROBE_HI - STAR2LTE_PMU_PROBE_LO)
#define STAR2LTE_PMU_PROBE_WORDS   (STAR2LTE_PMU_PROBE_BYTES / 4u)
#define STAR2LTE_PMU_PROBE_STEP    0x40u          // checkpoint granularity
#define STAR2LTE_PMU_CENSUS_MAX    320u
#define STAR2LTE_PMU_DIFF_MAX      96u

//
// The classic Exynos per-core power block: CONFIGURATION at
// 0x2000 + n*0x80 and STATUS four bytes above it, n being the linear core
// index 0..7 (four A55s then four M3s). Unverified on 9810 -- that is exactly
// what this probe is for -- but it costs sixteen reads to find out, and with
// half the cores running the answer is unmistakable.
//
#define STAR2LTE_PMU_CORE_BASE     0x2000u
#define STAR2LTE_PMU_CORE_STRIDE   0x80u

//
// Hang checkpoint. The offset about to be read is published to PRAM before the
// access, so a bus wedge leaves its own address behind: PRAM survives the
// reset, and the NEXT boot prints it as "=PMUCK". A hang stops being a dead
// end and becomes one measured bad offset per cycle.
//
// Both words live in the documented free window 0xFED17F0C..0xFED17F5F, clear
// of the boot-attempt counter at 0xFED17F10/14 and of the UFS driver's crash
// latch at 0xFED17F40/44.
//
#define STAR2LTE_PMU_CK_MAGIC       SIGNATURE_32 ('P', 'M', 'C', '1')
#define STAR2LTE_PMU_CK_MAGIC_ADDR  0x00000000FED17F20ULL
#define STAR2LTE_PMU_CK_OFF_ADDR    0x00000000FED17F24ULL
#define STAR2LTE_PMU_CK_DONE        0xFFFFFFFFu

STATIC UINT32  *mStar2LtePmuBefore = NULL;
STATIC UINT32  *mStar2LtePmuAfter  = NULL;
STATIC UINT32  mStar2LtePmuStart   = STAR2LTE_PMU_PROBE_LO;

/**
  Read MPIDR_EL1 directly. ArmLib is not linked into this library, and the file
  already reaches for inline asm elsewhere for exactly this reason.
**/
STATIC
UINT64
Star2LtePmuReadMpidr (
  VOID
  )
{
  UINT64  Mpidr;

  __asm__ __volatile__ ("mrs %0, mpidr_el1" : "=r" (Mpidr));
  return Mpidr;
}

/**
  Publish the offset we are about to touch, so that a hang identifies itself.
**/
STATIC
VOID
Star2LtePmuCheckpoint (
  IN UINT32  Offset
  )
{
  *(volatile UINT32 *)(UINTN)STAR2LTE_PMU_CK_MAGIC_ADDR = STAR2LTE_PMU_CK_MAGIC;
  *(volatile UINT32 *)(UINTN)STAR2LTE_PMU_CK_OFF_ADDR   = Offset;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

/**
  Read and print the eight conventional Exynos per-core power registers.

  Done as its own pass, BEFORE the bulk sweep, and printed as it goes: these
  sixteen words are the whole point of the exercise, so they must reach the
  ring even if a later access in the same page wedges the bus. With four A55s
  running and four M3s down, a correct guess at the map shows up immediately as
  four "on" values and four "off" values.
**/
STATIC
VOID
Star2LtePmuCoreRegs (
  IN CONST CHAR8  *Tag
  )
{
  volatile CONST UINT32  *Pmu;
  UINT32                 Off;
  UINTN                  Core;

  Pmu = (volatile CONST UINT32 *)(UINTN)STAR2LTE_PMU_PROBE_BASE;

  BdsPramStr ("\n=");
  BdsPramStr (Tag);
  for (Core = 0; Core < 8; Core++) {
    Off = STAR2LTE_PMU_CORE_BASE + ((UINT32)Core * STAR2LTE_PMU_CORE_STRIDE);

    BdsPramByte (' ');
    BdsPramHex (Off, 4);
    BdsPramByte ('=');

    Star2LtePmuCheckpoint (Off);
    BdsPramHex (Pmu[Off / 4u], 8);

    BdsPramByte ('/');

    Star2LtePmuCheckpoint (Off + 4u);
    BdsPramHex (Pmu[(Off + 4u) / 4u], 8);
  }
}

/**
  Copy the entire PMU register block into Out.

  A progress marker is emitted per 4 KB page. Unimplemented offsets inside a
  decoded syscon block read back as zero, but if any of them wedges the bus
  instead, the last marker left in the ring localises the offending page
  without costing a second boot.

  @param[out] Out  Buffer of STAR2LTE_PMU_PROBE_WORDS UINT32s.
**/
STATIC
VOID
Star2LtePmuRead (
  OUT UINT32  *Out
  )
{
  volatile CONST UINT32  *Pmu;
  UINT32                 Off;
  UINTN                  Idx;

  Pmu = (volatile CONST UINT32 *)(UINTN)STAR2LTE_PMU_PROBE_BASE;

  for (Off = mStar2LtePmuStart; Off < STAR2LTE_PMU_PROBE_HI; Off += 4u) {
    //
    // Publish before touching. If this access wedges the bus the value stays
    // in PRAM and the next boot names the exact offset that did it.
    //
    if ((Off % STAR2LTE_PMU_PROBE_STEP) == 0) {
      Star2LtePmuCheckpoint (Off);
    }

    //
    // The ring is only 16 KB and the census/diff still have to fit, so the
    // human-readable trail is far coarser than the PRAM checkpoint.
    //
    if ((Off % 0x400u) == 0) {
      BdsPramByte ('.');
      BdsPramHex (Off, 4);
    }

    Idx      = (Off - STAR2LTE_PMU_PROBE_LO) / 4u;
    Out[Idx] = Pmu[Off / 4u];
  }

  Star2LtePmuCheckpoint (STAR2LTE_PMU_CK_DONE);
}

/**
  Snapshot the PMU before any secondary is started.
**/
STATIC
VOID
Star2LtePmuProbeBefore (
  VOID
  )
{
  UINT32  Prev;

  BdsPramStr ("\n=PMUP base=");
  BdsPramHex64 (STAR2LTE_PMU_PROBE_BASE, 16);
  BdsPramStr (" lo=");
  BdsPramHex (STAR2LTE_PMU_PROBE_LO, 4);
  BdsPramStr (" hi=");
  BdsPramHex (STAR2LTE_PMU_PROBE_HI, 4);
  BdsPramStr (" mpidr=");
  BdsPramHex64 (Star2LtePmuReadMpidr (), 16);

  //
  // What the previous boot was touching when it died. DONE means the previous
  // boot completed its sweep; anything else is a proven-fatal offset, and this
  // boot resumes past its whole checkpoint group rather than dying on it again.
  // Progress is therefore monotonic: each hang costs one boot and permanently
  // retires one 64-byte group.
  //
  BdsPramStr ("\n=PMUCK ");
  if (*(volatile UINT32 *)(UINTN)STAR2LTE_PMU_CK_MAGIC_ADDR != STAR2LTE_PMU_CK_MAGIC) {
    BdsPramStr ("none");
  } else {
    Prev = *(volatile UINT32 *)(UINTN)STAR2LTE_PMU_CK_OFF_ADDR;
    BdsPramHex (Prev, 8);
    if ((Prev != STAR2LTE_PMU_CK_DONE) && (Prev >= STAR2LTE_PMU_PROBE_LO) &&
        (Prev < STAR2LTE_PMU_PROBE_HI))
    {
      mStar2LtePmuStart = Prev + STAR2LTE_PMU_PROBE_STEP;
    }
  }

  BdsPramStr (" start=");
  BdsPramHex (mStar2LtePmuStart, 4);

  mStar2LtePmuBefore = (UINT32 *)AllocateZeroPool (STAR2LTE_PMU_PROBE_BYTES);
  mStar2LtePmuAfter  = (UINT32 *)AllocateZeroPool (STAR2LTE_PMU_PROBE_BYTES);
  if ((mStar2LtePmuBefore == NULL) || (mStar2LtePmuAfter == NULL)) {
    BdsPramStr (" ALLOCFAIL");
    return;
  }

  BdsPramStr ("\n=PMUA");
  Star2LtePmuCoreRegs ("PMUC0");
  Star2LtePmuRead (mStar2LtePmuBefore);
  BdsPramStr (" ok");
}

/**
  Snapshot the PMU again, report the census and the diff, then return to
  recovery. Does not return when the probe ran to completion.
**/
STATIC
VOID
Star2LtePmuProbeAfter (
  VOID
  )
{
  UINTN   Idx;
  UINTN   Shown;

  if ((mStar2LtePmuBefore == NULL) || (mStar2LtePmuAfter == NULL)) {
    return;
  }

  BdsPramStr ("\n=PMUB");
  Star2LtePmuCoreRegs ("PMUC1");
  Star2LtePmuRead (mStar2LtePmuAfter);
  BdsPramStr (" ok");

  //
  // Census of the post-bring-up state. Printed BEFORE the diff so that, if it
  // overruns the ring, it is the census that is lost to the wrap rather than
  // the result.
  //
  BdsPramStr ("\n=PMUZ");
  Shown = 0;
  for (Idx = 0; Idx < STAR2LTE_PMU_PROBE_WORDS; Idx++) {
    if (mStar2LtePmuAfter[Idx] == 0) {
      continue;
    }

    if (Shown >= STAR2LTE_PMU_CENSUS_MAX) {
      BdsPramStr (" ...");
      break;
    }

    BdsPramByte (' ');
    BdsPramHex ((UINT32)(Idx * 4u) + STAR2LTE_PMU_PROBE_LO, 4);
    BdsPramByte ('=');
    BdsPramHex (mStar2LtePmuAfter[Idx], 8);
    Shown++;
  }

  BdsPramStr ("\n=PMUZN ");
  BdsPramHex ((UINT32)Shown, 4);

  //
  // The result: every PMU word that moved when MPIDR 0x001/0x002/0x003 came up.
  //
  BdsPramStr ("\n=PMUD");
  Shown = 0;
  for (Idx = 0; Idx < STAR2LTE_PMU_PROBE_WORDS; Idx++) {
    if (mStar2LtePmuBefore[Idx] == mStar2LtePmuAfter[Idx]) {
      continue;
    }

    if (Shown >= STAR2LTE_PMU_DIFF_MAX) {
      BdsPramStr (" ...");
      break;
    }

    BdsPramByte (' ');
    BdsPramHex ((UINT32)(Idx * 4u) + STAR2LTE_PMU_PROBE_LO, 4);
    BdsPramByte ('=');
    BdsPramHex (mStar2LtePmuBefore[Idx], 8);
    BdsPramByte ('>');
    BdsPramHex (mStar2LtePmuAfter[Idx], 8);
    Shown++;
  }

  BdsPramStr ("\n=PMUDN ");
  BdsPramHex ((UINT32)Shown, 4);
  BdsPramStr ("\n=PMUPEND\n");

  //
  // Straight back to TWRP: booting Windows here would destroy everything
  // printed above (the UFS driver wraps this ring within seconds).
  //
  Star2LteRecoveryReset ((UINT8)'M');
}

#endif // STAR2LTE_PMU_PROBE

//
// ----------------------------------------------------------------------------
// Big-cluster (Mongoose M3) CPU_ON probe -- boot-v26
//
// Three PMU sweeps (v23/v24/v25) each wedged the AXI bus permanently, because
// an undecoded Exynos sub-block never completes the read and this port has the
// Samsung watchdog disabled. Blind register discovery is abandoned.
//
// The watchdog does NOT rescue a CPU_ON hang. boot-v29 armed it (~30 s, recovery
// boot reason set first) immediately before a second CPU_ON, that SMC never
// returned, and the SoC sat on the Samsung logo until it was physically
// force-reset. So a CPU_ON hang costs exactly what a PMU bus wedge costs, and
// this probe must never issue a call that is known to hang.
//
// What v26/v28/v29 did establish, on hardware:
//
//   =BIGON r=0000000000000000 wd=off alive=00000000 @poll=03e8/03e8
//
//   1. CPU_ON(MPIDR 0x100) returns SUCCESS immediately. The old belief that the
//      big cluster hard-hangs the SoC on CPU_ON is false -- v20 hung on its
//      *second* call, not its first.
//   2. The core never reaches the stub: 1000/1000 polls x 5 ms = a full 5 s with
//      no alive marker, against ~30 ms for every A55.
//   3. A second CPU_ON to the same target never returns.
//
// SUCCESS-then-never-arrives plus a second request that blocks forever is the
// signature of a power transaction that was accepted and never completed: EL3
// forwards big-cluster bring-up to the ACPM/FlexPMU co-processor and returns
// without waiting.
//
// So stop guessing and read FlexPMU's own state. The device tree names exactly
// where it lives:
//
//   exynos_flexpmu_dbg/data-base = 0x0204F800, data-size = 0x400
//
// That 1 KB is the block behind /sys/kernel/debug/flexpmu-dbg -- cpu_status,
// seq_status, cur_sequence, and the "Reset-Release flag" / "Hotplug out flag" /
// "nonCPU CL1" fields that bigcluster-hotplug.sh watched move under Linux. It is
// APM *SRAM*, not a PMU register block: plain always-decoded memory, which is
// why this is safe where the v23-v25 PMU sweeps were not. Linux ioremaps it from
// non-secure EL1, so it is reachable from UEFI too.
//
// This probe therefore snapshots those 256 words, issues ONE CPU_ON, snapshots
// again, and reports the diff. The diff is layout-independent: any change proves
// ACPM received and acted on the request, and no change proves EL3 never reached
// it. No second CPU_ON, so the experiment cannot hang.
//
// One variable per build: STAR2LTE_BIG_TARGET selects which big core to try.
//
// SET TO 0 FOR ANY SHIPPABLE BUILD. The target executes the exact register-only
// CPU_OFF stub proven from TWRP, and the caller makes exactly one CPU_ON.
// STAR2LTE_BIG_PROBE_DIVERT returns to recovery after recording the result.
// ----------------------------------------------------------------------------
//

#define STAR2LTE_BIG_PROBE      0
#define STAR2LTE_BIG_TARGET     0x00000100u

//
// FlexPMU debug block, straight out of /proc/device-tree/exynos_flexpmu_dbg.
// Mapped a page at a time because SetMemoryAttributes works in pages.
//
#define STAR2LTE_APM_DBG_BASE   0x0204F800ULL
#define STAR2LTE_APM_DBG_WORDS  256u           // data-size 0x400 / 4
#define STAR2LTE_APM_MAP_BASE   0x0204F000ULL
#define STAR2LTE_APM_MAP_SIZE   0x1000ULL

//
// Ring budget: we reset immediately after printing, so the whole ring is ours.
//
#define STAR2LTE_APM_CENSUS_MAX 256u
#define STAR2LTE_APM_DIFF_MAX   48u

//
// Same divider as the ExitBootServices watchdog: 26 MHz / (93 * 128) ~= 2184 Hz,
// so 0xFFF5 counts is roughly 30 seconds.
//
#define STAR2LTE_BIG_WDT_COUNT  0xFFF5u

//
// 200 polls x 5 ms = 1 s. v29 already proved 5 s is futile and v35 proved a 20 s
// poll only gives the SoC longer to die, so keep the wait short: the answer now
// comes from PSCI, not from the bus.
//
#define STAR2LTE_BIG_POLL_US    5000u
#define STAR2LTE_BIG_POLL_MAX   200u

//
// v34: the MMIO witness (PMU INFORM2, uncached, equally visible to both
// clusters) stayed 0 alongside the DRAM marker. Both a DRAM and a Device-memory
// store are invisible, so either the core never runs, or it runs and has no
// working path to the bus -- in which case it wedges on its first store and
// looks identical. v36 discriminates with a store-free entry + a second CPU_ON.
//
#define STAR2LTE_BIG_TRACE_EVERY 40u
#define STAR2LTE_BIG_TRACE_MAX   6u
#define STAR2LTE_APM_IDX_CPUSTAT_HI 0x03u
#define STAR2LTE_APM_IDX_ACCESS     0x0Eu
#define STAR2LTE_APM_IDX_SWFLAG     0x2Au

//
// Exact read-only PMU status words used by the successful TWRP module. The
// target powers up and calls CPU_OFF in roughly 1.5 ms, so the old 5 ms trace
// interval cannot observe it. Poll these two known-safe status registers in a
// tight loop and classify CPU-off independently of cluster retention.
//
#define STAR2LTE_BIG_CPU_STATUS_ADDR      (EXYNOS_PMU_BASE + 0x2004ULL)
#define STAR2LTE_BIG_CLUSTER_STATUS_ADDR  (EXYNOS_PMU_BASE + 0x2044ULL)
#define STAR2LTE_BIG_STATUS_ON_MASK       0x0000000Fu
#define STAR2LTE_BIG_FAST_POLL_MAX        500000u
#define STAR2LTE_BIG_FAST_TRACE_MAX       16u

//
// v33 measured, and it changes the question entirely: FlexPMU reports the big
// core as fully up after CPU_ON. cpu_stat CL1_CPU0 0->1, SW_flag "Hotplug out"
// 0xfe->0xee (bit 4 = linear cpu4 cleared), and "Access Type" 0->1 -- which is
// exactly the value Linux shows while all eight cores are running. The
// Reset-Release flag was ALREADY 0xff before the call, so there is no cold
// release pending either. The power sequence completes.
//
// So the core is powered, released and marked online, yet the DRAM alive marker
// never appears. Two possibilities remain, and they need different fixes:
//
//   (a) the core is not executing our entry point at all, or
//   (b) it is executing, but its store is not visible to us -- the little cores
//       share cluster 0's L2 with the poller, the big core does not, and
//       cluster 1 may not be in the coherency domain yet.
//
// This build discriminates them with a marker the interconnect cannot hide: a
// PMU register. The big core runs a 7-instruction pre-stub that stores to PMU
// INFORM2 (Device memory, uncached, equally visible to both clusters) and then
// branches to the ordinary parking stub. INFORM2 is safe scratch here because
// Star2LteSetRecoveryBootReason() overwrites it a few hundred microseconds later
// on the way out.
//
#define STAR2LTE_BIG_STUB_OFF   0x400u
#define STAR2LTE_BIG_MMIO_MARK  EXYNOS9810_PMU_INFORM2

//
// Survives the reset that a hang causes; the ring does not.
//
#define STAR2LTE_BIG_LATCH_MAGIC_ADDR  0xFED17F28ULL
#define STAR2LTE_BIG_LATCH_STATE_ADDR  0xFED17F2CULL
#define STAR2LTE_BIG_LATCH_RES_ADDR    0xFED17F30ULL
#define STAR2LTE_BIG_LATCH_ALIVE_ADDR  0xFED17F34ULL
#define STAR2LTE_BIG_LATCH_POLL_ADDR   0xFED17F38ULL
#define STAR2LTE_BIG_LATCH_RES2_ADDR   0xFED17F3CULL
#define STAR2LTE_BIG_LATCH_MAGIC       0x42494731u   // 'BIG1'
#define STAR2LTE_BIG_STATE_PRE         1u            // about to issue CPU_ON
#define STAR2LTE_BIG_STATE_POST        2u            // CPU_ON returned
#define STAR2LTE_BIG_STATE_MAP         3u            // about to map APM SRAM
#define STAR2LTE_BIG_STATE_READ1       4u            // about to touch APM SRAM
#define STAR2LTE_BIG_STATE_SNAP        5u            // first APM read survived
#define STAR2LTE_BIG_STATE_2ND         6u            // about to issue the SECOND CPU_ON
#define STAR2LTE_BIG_STATE_DONE        9u            // probe ran to completion, no hang

//
// One 64-byte cache line covering the whole big latch (0xFED17F28..3C). Cleaning
// it is a WRITE-BACK, so the neighbouring owners that share the line (BATT at
// 0xFED17F10/14, the PMU probe at /20//24) keep their values -- their dirty data
// is pushed to RAM too, which is what they want anyway.
//
#define STAR2LTE_BIG_LATCH_WINDOW      0xFED17F00ULL
#define STAR2LTE_BIG_LATCH_WINDOW_SZ   0x40u

//
// The probe boot's own ring never reaches the host: the probe resets, and the
// boots that follow re-init the ring before ADB is ever up. The BATT-divert boot
// IS reliably captured, and it prints the latch, so the answer has to travel in
// PRAM rather than in the ring.
//
// The free window is 0xFED17F0C..0xFED17F5F and it is nearly full: BATT owns
// 0xFED17F10/14, the PMU probe 0xFED17F20/24, the big latch 0xFED17F28..3C, and
// the UFS driver's crash latch 0xFED17F40/44. RAM_STATUS sits at 0xFED17F60 and
// the KiSystemStartup latch at 0xFED17F80/90/98/A0/A8, both outside the window
// and both owned by someone else.
//
// That leaves exactly six words, 0xFED17F48..0xFED17F5C. Enough for the verdict
// (did anything move), a sanity check on the read, and one worked example.
//
#define STAR2LTE_APM_LAT_N_ADDR    0xFED17F48ULL   // number of differing words
#define STAR2LTE_APM_LAT_NZ_ADDR   0xFED17F4CULL   // non-zero words in "before"
#define STAR2LTE_APM_LAT_SUM_ADDR  0xFED17F50ULL   // XOR of all "before" words
#define STAR2LTE_APM_LAT_IDX_ADDR  0xFED17F54ULL   // index of first differing word
#define STAR2LTE_APM_LAT_B_ADDR    0xFED17F58ULL   // ...its value before CPU_ON
#define STAR2LTE_APM_LAT_A_ADDR    0xFED17F5CULL   // ...and after

#if STAR2LTE_BIG_PROBE && STAR2LTE_PSCI_PARK

/**
  Push the PRAM latch window out of the caches.

  The latch lives in normal cacheable memory, so a store plus `dsb sy` is NOT
  enough to survive the PMU SWRESET that follows it: DSB orders the write, it
  does not evict the line. Earlier latches appeared to work only by luck -- they
  were followed by a long-running SMC, which gave the dirty line time to be
  evicted naturally. v36 cycle 1 latched DONE and reset a few microseconds later,
  the line never left L1, and the next boot read the stale value.

  Anything written to the latch must therefore be cleaned explicitly.
**/
STATIC
VOID
Star2LteBigLatchFlush (
  VOID
  )
{
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)STAR2LTE_BIG_LATCH_WINDOW, STAR2LTE_BIG_LATCH_WINDOW_SZ);
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
VOID
Star2LteBigLatch (
  IN UINT32  State
  )
{
  *(volatile UINT32 *)(UINTN)STAR2LTE_BIG_LATCH_MAGIC_ADDR = STAR2LTE_BIG_LATCH_MAGIC;
  *(volatile UINT32 *)(UINTN)STAR2LTE_BIG_LATCH_STATE_ADDR = State;
  Star2LteBigLatchFlush ();
}

/**
  Print whatever the last probe run latched.

  Called at the very top of BDS, before the boot-attempt watchdog, because the
  probe's own boot always resets and its ring is re-inited by the next boot --
  only the boot that ends in the BATT divert reliably parks in TWRP long enough
  to be read. The latch is in PRAM and survives every reset in between, so the
  result reaches the host on whichever boot the host actually gets to capture.
**/
STATIC
VOID
Star2LteBigReportLatch (
  VOID
  )
{
  if (*(volatile UINT32 *)(UINTN)STAR2LTE_BIG_LATCH_MAGIC_ADDR != STAR2LTE_BIG_LATCH_MAGIC) {
    return;
  }

  BdsPramStr ("\n=BIGLATCH st=");
  BdsPramHex (*(volatile UINT32 *)(UINTN)STAR2LTE_BIG_LATCH_STATE_ADDR, 2);
  BdsPramStr (" r=");
  BdsPramHex (*(volatile UINT32 *)(UINTN)STAR2LTE_BIG_LATCH_RES_ADDR, 8);
  BdsPramStr (" alive=");
  BdsPramHex (*(volatile UINT32 *)(UINTN)STAR2LTE_BIG_LATCH_ALIVE_ADDR, 8);
  BdsPramStr (" poll=");
  BdsPramHex (*(volatile UINT32 *)(UINTN)STAR2LTE_BIG_LATCH_POLL_ADDR, 4);

  //
  // The FlexPMU result, carried in PRAM because the probe boot's ring is gone by
  // the time the host can read anything.
  //
  {
    BdsPramStr ("\n=APMLAT nz=");
    BdsPramHex (*(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_NZ_ADDR, 4);
    BdsPramStr (" sum=");
    BdsPramHex (*(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_SUM_ADDR, 8);
    BdsPramStr (" n=");
    BdsPramHex (*(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_N_ADDR, 4);
    BdsPramStr (" first=");
    BdsPramHex (*(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_IDX_ADDR, 2);
    BdsPramStr (":");
    BdsPramHex (*(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_B_ADDR, 8);
    BdsPramStr ("->");
    BdsPramHex (*(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_A_ADDR, 8);
  }
}

/**
  Arm the cluster-0 hardware watchdog with the recovery boot reason already set,
  so that an unrecoverable hang inside EL3 resets the SoC straight into TWRP.
**/
STATIC
VOID
Star2LteBigArmWatchdog (
  VOID
  )
{
  volatile UINT32  *Wdt;

  Wdt = (volatile UINT32 *)(UINTN)EXYNOS9810_WDT_BASE;

  Star2LteSetRecoveryBootReason ();
  *(volatile UINT32 *)(UINTN)EXYNOS9810_PMU_WDT_DISABLE    &= ~STAR2LTE_WDT_CLUSTER0_RESET_BIT;
  *(volatile UINT32 *)(UINTN)EXYNOS9810_PMU_WDT_MASK_RESET &= ~STAR2LTE_WDT_CLUSTER0_RESET_BIT;
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  Wdt[STAR2LTE_WDT_WTCON / sizeof (UINT32)] = 0u;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  Wdt[STAR2LTE_WDT_WTDAT / sizeof (UINT32)] = STAR2LTE_BIG_WDT_COUNT;
  Wdt[STAR2LTE_WDT_WTCNT / sizeof (UINT32)] = STAR2LTE_BIG_WDT_COUNT;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  Wdt[STAR2LTE_WDT_WTCON / sizeof (UINT32)] = STAR2LTE_WDT_ENABLE_RESET;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

STATIC
VOID
Star2LteBigDisarmWatchdog (
  VOID
  )
{
  volatile UINT32  *Wdt;

  Wdt = (volatile UINT32 *)(UINTN)EXYNOS9810_WDT_BASE;

  Wdt[STAR2LTE_WDT_WTCON / sizeof (UINT32)] = 0u;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

/**
  Attempt exactly one big-cluster CPU_ON, bracketed so the outcome is knowable
  whether it returns, hangs, or takes the SoC down.
**/
//
// 1 KB each. File-scope rather than stack so the BDS stack is untouched.
//
STATIC UINT32  mApmBefore[STAR2LTE_APM_DBG_WORDS];
STATIC UINT32  mApmAfter[STAR2LTE_APM_DBG_WORDS];

/**
  Map the FlexPMU debug page so BDS can read it.

  exynos_flexpmu_dbg has no "reg" -- its window is given as data-base/data-size
  and the Linux driver simply ioremaps it, so it is non-secure accessible. It is
  outside every window in PlatformMemoryMapLib, so add it here rather than
  perturbing the platform table (which would move a module outside BdsDxe in the
  FV diff). The CPU arch protocol is already a declared dependency of this
  module and its SetMemoryAttributes creates the translation entry.

  @retval EFI_SUCCESS   The page is mapped device-uncached.
**/
STATIC
EFI_STATUS
Star2LteApmMapDbg (
  VOID
  )
{
  EFI_CPU_ARCH_PROTOCOL  *Cpu;
  EFI_STATUS             Status;

  Cpu    = NULL;
  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (EFI_ERROR (Status) || (Cpu == NULL)) {
    return EFI_NOT_FOUND;
  }

  return Cpu->SetMemoryAttributes (
                Cpu,
                STAR2LTE_APM_MAP_BASE,
                STAR2LTE_APM_MAP_SIZE,
                EFI_MEMORY_UC
                );
}

/**
  Copy the 1 KB FlexPMU debug block into caller storage.

  @param[out] Out  Array of STAR2LTE_APM_DBG_WORDS UINT32s.
**/
STATIC
VOID
Star2LteApmSnap (
  OUT UINT32  *Out
  )
{
  volatile UINT32  *Src;
  UINTN            Idx;

  Src = (volatile UINT32 *)(UINTN)STAR2LTE_APM_DBG_BASE;
  for (Idx = 0; Idx < STAR2LTE_APM_DBG_WORDS; Idx++) {
    Out[Idx] = Src[Idx];
  }
}

STATIC
VOID
Star2LteBigEmitProbeStub (
  OUT UINT32  *Code
  )
{
  //
  // Exact five-word target proven twice from TWRP, first through the ION/WC
  // mapping and then through the normal cacheable linear alias plus a clean to
  // PoC. It needs no stack, memory access, MMIO, or exception vectors:
  //
  //   mov   w0, #2
  //   movk  w0, #0x8400, lsl #16
  //   mov   w1, #0x10000
  //   smc   #0
  // 1:b     1b
  //
  // The target therefore returns itself to the normal PSCI off state. If
  // CPU_OFF unexpectedly returns, it spins without touching the bus.
  //
  Code[0] = 0x52800040u;
  Code[1] = 0x72B08000u;
  Code[2] = 0x52A00021u;
  Code[3] = 0xD4000003u;
  Code[4] = 0x14000000u;
}

STATIC UINT32  mBigTrace[STAR2LTE_BIG_TRACE_MAX][4];
STATIC UINT32  mBigFastTrace[STAR2LTE_BIG_FAST_TRACE_MAX][2];

STATIC
VOID
Star2LteBigClusterProbe (
  VOID
  )
{
  EFI_PHYSICAL_ADDRESS  Addr;
  EFI_STATUS            Status;
  UINTN                 Fn;
  UINTN                 A1;
  UINTN                 A2;
  UINTN                 A3;
  UINTN                 Val;
  volatile UINT32       *Mark;
  UINT32                Prev;
  UINT32                Seen;
  UINT32                MmioSeen;
  UINTN                 Poll;
  UINTN                 Traces;
  UINTN                 Idx;
  UINTN                 Shown;
  UINTN                 FastPoll;
  UINTN                 FastTraces;
  UINT32                ApmOk;
  UINT32                First;
  UINT32                CpuStatus;
  UINT32                ClusterStatus;
  UINT32                LastCpuStatus;
  UINT32                LastClusterStatus;
  UINT32                SawCpuOnline;
  UINT32                SawCpuOffAfterOnline;

  //
  // Report what the previous boot got to, then claim the latch for this one.
  //
  Prev = 0;
  if (*(volatile UINT32 *)(UINTN)STAR2LTE_BIG_LATCH_MAGIC_ADDR == STAR2LTE_BIG_LATCH_MAGIC) {
    Prev = *(volatile UINT32 *)(UINTN)STAR2LTE_BIG_LATCH_STATE_ADDR;
  }

  BdsPramStr ("\n=BIGP t=");
  BdsPramHex (STAR2LTE_BIG_TARGET, 8);
  BdsPramStr (" prev=");
  BdsPramHex (Prev, 8);

  //
  // SELF-DISABLE. v35 bootlooped: CPU_ON never returned, the armed watchdog reset
  // the SoC, and the next boot walked straight back into the same SMC. Any latch
  // state other than DONE means the previous boot hung inside this probe, so skip
  // it entirely and let the boot proceed. One hang maximum, then it stops by
  // itself -- the ring still carries `prev=` so the hang point is not lost.
  //
  if ((Prev != 0) && (Prev != STAR2LTE_BIG_STATE_DONE)) {
    BdsPramStr (" SKIP-PREV-HUNG\n");
    //
    // Re-arm for the boot after this one. The cooldown is what breaks the loop:
    // a hang costs one skipped boot, and that boot still diverts to recovery, so
    // the device is always reachable within two boots instead of bootlooping.
    //
    Star2LteBigLatch (STAR2LTE_BIG_STATE_DONE);
    return;
  }

  //
  // One reserved page below 4 GB, same recipe as the parking block, so the big
  // core lands on a stub that is identical to the one the A55s already run.
  //
  Addr   = 0xFFFFFFFFULL;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiReservedMemoryType,
                  1,
                  &Addr
                  );
  if (EFI_ERROR (Status)) {
    BdsPramStr (" ALLOCFAIL\n");
    return;
  }

  ZeroMem ((VOID *)(UINTN)Addr, EFI_PAGE_SIZE);
  *(volatile UINT32 *)(UINTN)Addr = STAR2LTE_BIG_TARGET;
  Star2LteParkEmitStub (
    (UINT32 *)(UINTN)(Addr + STAR2LTE_PARK_STUB_OFF),
    (UINT64)Addr
    );
  Star2LteBigEmitProbeStub ((UINT32 *)(UINTN)(Addr + STAR2LTE_BIG_STUB_OFF));

  //
  // Cluster 1's L2 is cold and non-coherent at power-up, so the stub has to be
  // at the point of coherency before the core can fetch it.
  //
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Addr, EFI_PAGE_SIZE);
  InvalidateInstructionCacheRange ((VOID *)(UINTN)Addr, EFI_PAGE_SIZE);

  BdsPramStr (" mb=");
  BdsPramHex64 ((UINT64)Addr, 16);

  //
  // Map and snapshot FlexPMU's debug block BEFORE the CPU_ON. Each step latches
  // first, so if a read of APM SRAM faults, the next boot names the exact stage.
  //
  ApmOk = 0;
  First = 0;
  //
  // 0xFFFFFFFF = "started, never finished", so a hang is distinguishable from a
  // genuine zero-diff result. The real count is written last.
  //
  *(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_N_ADDR   = 0xFFFFFFFFu;
  *(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_NZ_ADDR  = 0;
  *(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_SUM_ADDR = 0;
  *(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_IDX_ADDR = 0;
  *(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_B_ADDR   = 0;
  *(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_A_ADDR   = 0;
  Star2LteBigLatch (STAR2LTE_BIG_STATE_MAP);
  Status = Star2LteApmMapDbg ();
  BdsPramStr ("\n=APMMAP ");
  BdsPramStatusLite (Status);
  if (!EFI_ERROR (Status)) {
    //
    // One word first. If APM SRAM is not reachable from non-secure EL1 at this
    // point in boot, this is where it dies, and the latch says so.
    //
    Star2LteBigLatch (STAR2LTE_BIG_STATE_READ1);
    First = *(volatile UINT32 *)(UINTN)STAR2LTE_APM_DBG_BASE;
    Star2LteBigLatch (STAR2LTE_BIG_STATE_SNAP);
    BdsPramStr (" w0=");
    BdsPramHex (First, 8);

    Star2LteApmSnap (mApmBefore);
    ApmOk = 1;

    //
    // Non-zero census of the pre-CPU_ON state. Linux reports Reset-Release flag
    // 0xff, Hotplug out flag 0x0, nonCPU CL0/CL1 1/1 with all eight cores up;
    // this is the same block as seen from UEFI, so the two can be correlated.
    //
    BdsPramStr ("\n=APMPRE");
    Shown = 0;
    for (Idx = 0; Idx < STAR2LTE_APM_DBG_WORDS; Idx++) {
      if (mApmBefore[Idx] == 0) {
        continue;
      }

      if (Shown >= STAR2LTE_APM_CENSUS_MAX) {
        BdsPramStr (" +more");
        break;
      }

      BdsPramStr (" ");
      BdsPramHex ((UINT32)Idx, 2);
      BdsPramStr (":");
      BdsPramHex (mApmBefore[Idx], 8);
      Shown++;
    }

    BdsPramStr ("\n");
  }

  Star2LteBigArmWatchdog ();
  BdsPramStr (" wd=on\n");

  //
  // Clear the MMIO witness so a stale value cannot be mistaken for a check-in.
  //
  *(volatile UINT32 *)(UINTN)STAR2LTE_BIG_MMIO_MARK = 0;
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  Star2LteBigLatch (STAR2LTE_BIG_STATE_PRE);

  Fn  = (UINTN)STAR2LTE_PARK_CPU_ON;
  A1  = (UINTN)STAR2LTE_BIG_TARGET;
  A2  = (UINTN)(Addr + STAR2LTE_BIG_STUB_OFF);
  A3  = 0;
  Val = ArmCallSmc3 (Fn, &A1, &A2, &A3);

  //
  // The TWRP positive control observed the complete online -> CPU_OFF sequence
  // between samples 0 and 793 of this same tight status loop. Do this before
  // printing, cache maintenance, or a timed stall can hide the short event.
  //
  LastCpuStatus        = 0xFFFFFFFFu;
  LastClusterStatus    = 0xFFFFFFFFu;
  SawCpuOnline         = 0;
  SawCpuOffAfterOnline = 0;
  FastTraces           = 0;
  CpuStatus            = 0;
  ClusterStatus        = 0;
  for (FastPoll = 0; FastPoll < STAR2LTE_BIG_FAST_POLL_MAX; FastPoll++) {
    CpuStatus     = *(volatile UINT32 *)(UINTN)STAR2LTE_BIG_CPU_STATUS_ADDR;
    ClusterStatus = *(volatile UINT32 *)(UINTN)STAR2LTE_BIG_CLUSTER_STATUS_ADDR;

    if ((CpuStatus != LastCpuStatus) || (ClusterStatus != LastClusterStatus)) {
      if (FastTraces < STAR2LTE_BIG_FAST_TRACE_MAX) {
        mBigFastTrace[FastTraces][0] = CpuStatus;
        mBigFastTrace[FastTraces][1] = ClusterStatus;
      }

      FastTraces++;
      LastCpuStatus     = CpuStatus;
      LastClusterStatus = ClusterStatus;
    }

    if ((CpuStatus & STAR2LTE_BIG_STATUS_ON_MASK) == STAR2LTE_BIG_STATUS_ON_MASK) {
      SawCpuOnline = 1;
    }

    if ((SawCpuOnline != 0) &&
        ((CpuStatus & STAR2LTE_BIG_STATUS_ON_MASK) == 0))
    {
      SawCpuOffAfterOnline = 1;
      break;
    }

    __asm__ __volatile__ ("yield");
  }

  Star2LteBigLatch (STAR2LTE_BIG_STATE_POST);
  Star2LteBigDisarmWatchdog ();

  BdsPramStr ("=BIGON r=");
  BdsPramHex64 ((UINT64)Val, 16);
  BdsPramStr (" wd=off");
  BdsPramStr ("\n=BIGPMU on=");
  BdsPramHex (SawCpuOnline, 1);
  BdsPramStr (" off=");
  BdsPramHex (SawCpuOffAfterOnline, 1);
  BdsPramStr (" polls=");
  BdsPramHex ((UINT32)FastPoll, 8);
  BdsPramStr (" n=");
  BdsPramHex ((UINT32)FastTraces, 4);
  for (Idx = 0;
       (Idx < FastTraces) && (Idx < STAR2LTE_BIG_FAST_TRACE_MAX);
       Idx++)
  {
    BdsPramStr ("\n ");
    BdsPramHex ((UINT32)Idx, 2);
    BdsPramStr (":cpu=");
    BdsPramHex (mBigFastTrace[Idx][0], 8);
    BdsPramStr (" cl=");
    BdsPramHex (mBigFastTrace[Idx][1], 8);
  }

  //
  // Poll rather than sample once. The little cores answer within 30 ms, but the
  // big cluster's first bring-up also does a cold reset-release (Linux shows
  // "Reset-Release flag" sticky at 0xff once set), plus an L2/CCI power-up that
  // FlexPMU sequences itself, so it can be far slower. Report WHEN it checks in.
  //
  Mark = (volatile UINT32 *)(UINTN)(Addr + STAR2LTE_PARK_MARK_OFF);
  Seen = 0;
  MmioSeen = 0;
  Traces = 0;
  for (Poll = 0; Poll < STAR2LTE_BIG_POLL_MAX; Poll++) {
    gBS->Stall (STAR2LTE_BIG_POLL_US);
    MmioSeen = *(volatile UINT32 *)(UINTN)STAR2LTE_BIG_MMIO_MARK;
    WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Mark, 4);
    Seen = *Mark;

    if (((Poll % STAR2LTE_BIG_TRACE_EVERY) == 0) && (Traces < STAR2LTE_BIG_TRACE_MAX)) {
      volatile UINT32  *Apm;

      Apm = (volatile UINT32 *)(UINTN)STAR2LTE_APM_DBG_BASE;
      mBigTrace[Traces][0] = Apm[STAR2LTE_APM_IDX_CPUSTAT_HI];
      mBigTrace[Traces][1] = Apm[STAR2LTE_APM_IDX_ACCESS];
      mBigTrace[Traces][2] = Apm[STAR2LTE_APM_IDX_SWFLAG];
      mBigTrace[Traces][3] = MmioSeen;
      Traces++;
    }

    if ((Seen != 0) || (MmioSeen != 0)) {
      break;
    }
  }

  BdsPramStr (" alive=");
  BdsPramHex (Seen, 8);
  BdsPramStr (" mmio=");
  BdsPramHex (MmioSeen, 8);
  BdsPramStr (" @poll=");
  BdsPramHex ((UINT32)Poll, 4);
  BdsPramStr ("/");
  BdsPramHex ((UINT32)STAR2LTE_BIG_POLL_MAX, 4);

  //
  // 1 Hz trace of the three FlexPMU fields that moved across CPU_ON, plus the
  // MMIO witness. If FlexPMU reverts the core after nobody checks in, cpu_stat
  // drops back and the hotplug-out bit is re-set here; if the state is simply
  // static for 20 s, the core is up-and-idle at the wrong address.
  //
  BdsPramStr ("\n=BIGTRC n=");
  BdsPramHex ((UINT32)Traces, 2);
  for (Idx = 0; Idx < Traces; Idx++) {
    BdsPramStr ("\n ");
    BdsPramHex ((UINT32)Idx, 2);
    BdsPramStr (":cs=");
    BdsPramHex (mBigTrace[Idx][0], 8);
    BdsPramStr (" ac=");
    BdsPramHex (mBigTrace[Idx][1], 8);
    BdsPramStr (" sw=");
    BdsPramHex (mBigTrace[Idx][2], 8);
    BdsPramStr (" mm=");
    BdsPramHex (mBigTrace[Idx][3], 8);
  }

  //
  // Latch the full result so it survives into the ring of whichever later boot
  // the host manages to capture.
  //
  *(volatile UINT32 *)(UINTN)STAR2LTE_BIG_LATCH_RES_ADDR   = (UINT32)Val;
  *(volatile UINT32 *)(UINTN)STAR2LTE_BIG_LATCH_ALIVE_ADDR = Seen;
  *(volatile UINT32 *)(UINTN)STAR2LTE_BIG_LATCH_POLL_ADDR  = (UINT32)Poll;
  *(volatile UINT32 *)(UINTN)STAR2LTE_BIG_LATCH_RES2_ADDR =
    SawCpuOnline | (SawCpuOffAfterOnline << 1);
  Star2LteBigLatchFlush ();

  //
  // Snapshot FlexPMU again and diff. This is the whole point of the build and it
  // is layout-independent: any word that moved proves ACPM received the request
  // and acted on it, so the core really is being sequenced and the failure is
  // downstream (reset-release, L2/CCI, or our stub). Nothing moving proves EL3
  // never reached ACPM at all, and the SUCCESS it returned was empty.
  //
  // Deliberately NO second CPU_ON here -- it is issued after =BIGEND, once the
  // APM diff is safely latched, so a hang there cannot cost us this evidence.
  //
  if (ApmOk != 0) {
    Star2LteApmSnap (mApmAfter);

    //
    // Latch first, print second: the print only reaches a ring nobody reads.
    //
    {
      UINT32  Diffs;
      UINT32  Nz;
      UINT32  Sum;
      UINT32  FirstIdx;
      UINT32  FirstB;
      UINT32  FirstA;

      Diffs    = 0;
      Nz       = 0;
      Sum      = 0;
      FirstIdx = 0;
      FirstB   = 0;
      FirstA   = 0;

      for (Idx = 0; Idx < STAR2LTE_APM_DBG_WORDS; Idx++) {
        Sum ^= mApmBefore[Idx];
        if (mApmBefore[Idx] != 0) {
          Nz++;
        }

        if (mApmBefore[Idx] == mApmAfter[Idx]) {
          continue;
        }

        if (Diffs == 0) {
          FirstIdx = (UINT32)Idx;
          FirstB   = mApmBefore[Idx];
          FirstA   = mApmAfter[Idx];
        }

        Diffs++;
      }

      *(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_NZ_ADDR  = Nz;
      *(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_SUM_ADDR = Sum;
      *(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_IDX_ADDR = FirstIdx;
      *(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_B_ADDR   = FirstB;
      *(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_A_ADDR   = FirstA;
      //
      // n last: it is the word the host reads as "this run completed".
      //
      *(volatile UINT32 *)(UINTN)STAR2LTE_APM_LAT_N_ADDR = Diffs;
      __asm__ __volatile__ ("dsb sy" ::: "memory");
      WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)STAR2LTE_APM_LAT_NZ_ADDR, 0x40u);
      __asm__ __volatile__ ("dsb sy" ::: "memory");
    }

    BdsPramStr ("\n=APMDIFF");
    Shown = 0;
    for (Idx = 0; Idx < STAR2LTE_APM_DBG_WORDS; Idx++) {
      if (mApmBefore[Idx] == mApmAfter[Idx]) {
        continue;
      }

      if (Shown >= STAR2LTE_APM_DIFF_MAX) {
        BdsPramStr (" +more");
        break;
      }

      BdsPramStr (" ");
      BdsPramHex ((UINT32)Idx, 2);
      BdsPramStr (":");
      BdsPramHex (mApmBefore[Idx], 8);
      BdsPramStr ("->");
      BdsPramHex (mApmAfter[Idx], 8);
      Shown++;
    }

    BdsPramStr (" n=");
    BdsPramHex ((UINT32)Shown, 4);
  } else {
    BdsPramStr ("\n=APMDIFF skipped");
  }

  BdsPramStr ("\n=BIGEND\n");

  //
  // Exactly one CPU_ON is permitted. The prior second-call discriminator hangs
  // EL3 on this platform and is intentionally absent.
  //
  Star2LteBigLatch (STAR2LTE_BIG_STATE_DONE);

  //
  // Deliberately NO reset here: the caller (the boot-attempt watchdog's divert
  // branch) does the recovery reset, which is the only path measured to actually
  // park in TWRP. Returning keeps this boot's ring -- the one holding =APMPRE and
  // =APMDIFF -- as the ring the host reads.
  //
}

#endif // STAR2LTE_BIG_PROBE && STAR2LTE_PSCI_PARK

//
// ----------------------------------------------------------------------------
// PlatformBootManagerLib entry points (called by BdsDxe)
// ----------------------------------------------------------------------------
//

VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  BDS_DPUT ('a');   // DIAG: BDS BeforeConsole entered
#if STAR2LTE_BIG_PROBE && STAR2LTE_PSCI_PARK
  //
  // Before the attempt watchdog: the divert path does not return, and the
  // divert boot is the one that reliably parks in TWRP to be read.
  //
  Star2LteBigReportLatch ();
#endif
  Star2LteBootAttemptWatchdog ();
  RegisterConsoles ();
  BDS_DPUT ('b');   // DIAG: consoles registered (ConOut var set)
#if STAR2LTE_PMU_PROBE
  //
  // Must bracket Star2LteParkSecondaries() below: the diff between this
  // snapshot and the one taken after it is the whole measurement.
  //
  Star2LtePmuProbeBefore ();
#endif
#if STAR2LTE_PSCI_SELFTEST
  //
  // Deliberately after Star2LteBootAttemptWatchdog() above: if the monitor hangs
  // us on CPU_ON, the watchdog has already armed the divert to recovery.
  //
  Star2LtePsciSelfTest ();
  BDS_DPUT ('P');   // DIAG: PSCI self-test returned
#endif
#if STAR2LTE_PSCI_PARK
  //
  // After the watchdog for the same reason as the selftest: if the monitor hangs
  // us on CPU_ON the divert to recovery is already armed.
  //
  Star2LteParkSecondaries ();
  BDS_DPUT ('K');   // DIAG: parking protocol bring-up returned
#endif
#if STAR2LTE_BIG_PROBE && STAR2LTE_PSCI_PARK && !STAR2LTE_BIG_PROBE_DIVERT
  //
  // Last, so the working 4-core state is fully established first. Arms the
  // hardware watchdog itself and returns to recovery; does not come back.
  //
  Star2LteBigClusterProbe ();
  BDS_DPUT ('G');   // DIAG: big-cluster probe returned (allocation failure only)
#endif
#if STAR2LTE_PMU_PROBE
  //
  // Reports the diff and returns to recovery; does not come back.
  //
  Star2LtePmuProbeAfter ();
  BDS_DPUT ('M');   // DIAG: PMU probe returned (only on allocation failure)
#endif
}

VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_STATUS  TouchRailStatus;

  //
  // Bring up everything (UFS/USB) so the ESP is reachable, then make Windows
  // the default boot target.
  //
  BDS_DPUT ('c');   // DIAG: BDS AfterConsole entered (console should be live now)

#if STAR2LTE_TOUCH_RAIL_FIX
  TouchRailStatus = Star2LteEnableTouchRails ();
  if (EFI_ERROR (TouchRailStatus)) {
    DEBUG ((
      DEBUG_WARN,
      "Star2Lte: touchscreen rail enable failed: %r\n",
      TouchRailStatus
      ));
  } else {
    DEBUG ((DEBUG_INFO, "Star2Lte: touchscreen rails enabled and verified.\n"));
  }
#else
  TouchRailStatus = EFI_UNSUPPORTED;
#endif

  //
  // Push the SEC-phase progress text ("Starting UEFI..." / "memory map ready")
  // to the panel so it is visible during the (slow) device connect below, before
  // the graphics console clears the screen for the banner.
  //
  BdsPresentPanel ();
  BDS_DPUT ('d');              // DIAG: present returned (FB still mapped)

#if !STAR2LTE_FAST_STORAGE_ENUM
  ConnectAllControllers ();
  BDS_DPUT ('e');              // DIAG: ConnectAllControllers done
#endif

  {
    UINTN       UfsHcCount;
    UINTN       ExtScsiCount;
    UINTN       ScsiIoCount;
    UINTN       BlockIoCount;
    EFI_STATUS  UfsConnect;
    EFI_STATUS  ExtScsiConnect;
    EFI_STATUS  ScsiIoConnect;

    Star2LteConnectUfsScsiStack (&UfsHcCount, &ExtScsiCount, &ScsiIoCount, &BlockIoCount, &UfsConnect, &ExtScsiConnect, &ScsiIoConnect);
  }
  BDS_DPUT ('e');              // DIAG: targeted storage connect done

  RegisterWindowsBootOption ();
  BDS_DPUT ('f');              // DIAG: boot option scan done

  //
  // FIRST LIGHT TEXT: now that GraphicsConsole should be bound to our framebuffer
  // GOP (which now triggers DECON on every Blt), draw a line through ConOut. This
  // proves UEFI TEXT renders on the panel independent of whether BDS finds a
  // bootable device (the trail previously ended at 'f' without reaching
  // UnableToBoot). If the text shows, the console path is fully up.
  //
  if (STAR2LTE_PANEL_DIAG_TEXT && (gST->ConOut != NULL)) {
    BDS_DPUT ('p');            // DIAG: ConOut present, about to clear+print
    gST->ConOut->ClearScreen (gST->ConOut);
    gST->ConOut->OutputString (
      gST->ConOut,
      L"\r\n  Star2Lte UEFI - first light. Graphics console is up.\r\n"
      L"  Exynos 9810 / Galaxy S9+  |  EDK2 BDS reached.\r\n"
      );
    ReportStorageOnScreen ();   // DIAG: UFS/disk/filesystem enumeration on panel
#if STAR2LTE_PSCI_SELFTEST
    //
    // Hold the panel afterwards: this is the only surviving channel for the SMP
    // result once the device boots all the way into Windows, and it has to be
    // readable by eye before the boot continues.
    //
    Star2LtePsciReportOnScreen ();
    gBS->Stall (20 * 1000 * 1000);
#endif
    BDS_DPUT ('q');            // DIAG: OutputString returned (text rendered)
  } else {
    BDS_DPUT ('r');            // DIAG: ConOut is NULL (no console bound)
  }

  Star2LteTraceInstallBootHooks ();
  Star2LteInstallCpuExceptionTrace ();

#if STAR2LTE_USB_DEBUG_MODE >= STAR2LTE_USB_DEBUG_SNAPSHOT
  Star2LteUsbDebugSnapshot (BdsPramStr);
#endif
#if STAR2LTE_USB_DEBUG_MODE == STAR2LTE_USB_DEBUG_HID
  Star2LteUsbDebugStart (BdsPramStr);
#endif
  Star2LtePublishSecureBootOffVariables ();
  mStar2LteTraceSetVariableEnabled = TRUE;
  Star2LteReserveWinloadPages ();
  Star2LteInstallBlockIoTrace ();
  if (!Star2LteBootRegisteredBootmgr ()) {
    Star2LteDirectBootRootBootmgr ();
  }
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (
  IN UINT16  TimeoutRemain
  )
{
  // Optional progress UI. No-op for bring-up.
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  BDS_DPUT ('g');   // DIAG: UnableToBoot entered (no bootable device)

  //
  // Last possible moment, and always reached in the failing case: mirror the
  // storage stack's 0xFED13000 diagnostic block into the PRAM ring so it
  // survives into pstore. Emitted exactly once per boot.
  //
  Star2LteDumpStorageDiag ();

  //
  // Nothing bootable was found (no UFS ESP yet, or Windows not installed).
  // Force-connect all handles so GraphicsConsole gets attached to the
  // framebuffer GOP and UEFI text appears on the panel — this gives us first
  // light even when there is no bootable storage. Then spin; the UEFI text
  // output and serial console are the diagnostic lifeline until a Windows ESP
  // is present.
  //
#if !STAR2LTE_FAST_STORAGE_ENUM
  ConnectAllControllers ();
#endif

  //
  // Make sure the console banner is on the panel, then a breadcrumb.
  //
  BdsPresentPanel ();
  BDS_DPUT ('h');              // DIAG: present in UnableToBoot returned

  DEBUG ((DEBUG_ERROR,
    "BDS: nothing to boot. Is the Windows ESP present on UFS and is UFS up?\n"));

  if (STAR2LTE_PANEL_DIAG_TEXT && (gST->ConOut != NULL)) {
    BDS_DPUT ('i');   // DIAG: ConOut non-NULL, about to OutputString
    gST->ConOut->OutputString (gST->ConOut,
      L"\r\n*** Star2Lte UEFI: no bootable device. Connect UFS/USB. ***\r\n");
    BDS_DPUT ('j');   // DIAG: OutputString returned (GraphicsConsole rendered)
  } else {
    BDS_DPUT ('k');   // DIAG: ConOut is NULL (no console bound!)
  }

#if STAR2LTE_AUTO_RECOVERY
  //
  // Unattended bring-up loop: drop to TWRP so the PC can flash the next cycle. A
  // successful cycle boots Windows and never reaches here, so reaching this point
  // IS the "failed, try the next image" signal. Countdown first (panel readable +
  // serial flush), then signal recovery and software-reset the SoC.
  //
  {
    UINTN   Seconds;
    CHAR16  CountLine[96];

    for (Seconds = STAR2LTE_AUTO_RECOVERY_DELAY_S; Seconds > 0; Seconds--) {
      if (STAR2LTE_PANEL_DIAG_TEXT && (gST->ConOut != NULL)) {
        UnicodeSPrint (
          CountLine, sizeof (CountLine),
          L"  [auto] no boot device - rebooting to recovery in %2u s ...\r",
          (UINT32)Seconds
          );
        gST->ConOut->OutputString (gST->ConOut, CountLine);
      }
      gBS->Stall (1000000);   // 1 second
    }
    if (STAR2LTE_PANEL_DIAG_TEXT && (gST->ConOut != NULL)) {
      gST->ConOut->OutputString (
        gST->ConOut,
        L"\r\n  [auto] signalling recovery + SoC reset now.                 \r\n"
        );
    }
    BDS_DPUT ('R');   // DIAG: auto-recovery reboot armed
    //
    // SEC reset-reason RECOVERY -> PMU INFORM3, barrier, then PMU SWRESET (proven in
    // m1_power.c). The next power-on enters TWRP. If it does not take, fall through to
    // the dead loop below - the same safe halt as before, never worse.
    //
    Star2LteSetRecoveryBootReason ();
    *(volatile UINT32 *)(UINTN)EXYNOS9810_PMU_SWRESET = 0x1u;
    __asm__ __volatile__ ("dsb sy" ::: "memory");
  }
#endif

  CpuDeadLoop ();
}
