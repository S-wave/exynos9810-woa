## @file
#  Platform description (DSC) for Star2LtePkg — Galaxy S9+ / Exynos 9810.
#
#  Modeled on the PrePi-based AArch64 phone ports (WOA-Project Lumia950XLPkg /
#  mu_andromeda_platforms): a combined SEC+PEI (ArmPlatformPkg/PrePi) hands off
#  straight to the DXE core, then BDS.
#
#  NOTE ON VERSIONS: library .inf paths track a specific edk2 checkout. If the
#  build complains a path moved, find the current location in your edk2 tree
#  (paths drift between releases). Build ONE component at a time, starting with
#  the SerialPortLib, then PrePi, then DxeMain.
##

[Defines]
  PLATFORM_NAME                  = Star2LtePkg
  PLATFORM_GUID                  = 11111111-2222-3333-4444-555555555500
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x0001001B
  OUTPUT_DIRECTORY               = Build/Star2LtePkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Platform/Star2LtePkg/Star2LtePkg.fdf

!ifndef STAR2LTE_USB_DEBUG_MODE
  DEFINE STAR2LTE_USB_DEBUG_MODE = 0
!endif

#
# ext210: staged extra-RAM publication at SEC/PEI (see MemoryInitPeiLib.c).
#   0 = 711 MiB baseline, 3 = the deployed 1,357 MiB, 4 = adds the 4 GiB bank.
#
!ifndef STAR2LTE_RAM_STAGE
  DEFINE STAR2LTE_RAM_STAGE = 0
!endif

[BuildOptions]
  GCC:*_*_AARCH64_PLATFORM_FLAGS = -march=armv8-a
  # CRITICAL for AArch64: force 4KB PE section alignment. Without this the
  # default /ALIGN:32 produces a TE StrippedSize that is NOT page-aligned, which
  # breaks every PC-relative adrp/ldr access to .data after TE stripping (we hit
  # this exactly: mSystemMemoryEnd read as 0 -> SEC stack = 0x1 -> CEntryPoint
  # prologue faulted). Matches ArmVirtPkg/ArmVirt.dsc.inc.
  GCC:*_*_AARCH64_DLINK_FLAGS    = -z common-page-size=0x1000
  CLANGPDB:*_*_*_DLINK_FLAGS     = /ALIGN:0x1000
  # LLVM 23 added -Wunused-but-set-global; the diagnostic globals the BDS keeps for
  # debugging trip it under -Werror. Older clang ignores the flag (-Wno-unknown-warning-option).
  CLANGPDB:*_*_AARCH64_CC_FLAGS  = -DSTAR2LTE_USB_DEBUG_MODE=$(STAR2LTE_USB_DEBUG_MODE) -DSTAR2LTE_RAM_STAGE=$(STAR2LTE_RAM_STAGE) -Wno-unused-but-set-global
  CLANGDWARF:*_*_AARCH64_CC_FLAGS = -DSTAR2LTE_USB_DEBUG_MODE=$(STAR2LTE_USB_DEBUG_MODE) -DSTAR2LTE_RAM_STAGE=$(STAR2LTE_RAM_STAGE) -Wno-unused-but-set-global

################################################################################
#
# Library classes — shared by all module types
#
################################################################################
[LibraryClasses.common]
  # --- MdePkg base ---
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLibOptDxe/BaseMemoryLibOptDxe.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf

  # --- Stack check (compiler-inserted canaries; Null for bring-up) ---
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  PeCoffExtraActionLib|MdePkg/Library/BasePeCoffExtraActionLibNull/BasePeCoffExtraActionLibNull.inf
  SafeIntLib|MdePkg/Library/BaseSafeIntLib/BaseSafeIntLib.inf

  # --- Debug / status code ---
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf

  # --- Serial console: our Exynos UART (Milestone 1) ---
  SerialPortLib|Silicon/Exynos9810Pkg/Library/Exynos9810SerialPortLib/Exynos9810SerialPortLib.inf

  # --- ARM architecture ---
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  ArmMmuLib|UefiCpuPkg/Library/ArmMmuLib/ArmMmuBaseLib.inf
  ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf
  ArmHvcLib|ArmPkg/Library/ArmHvcLib/ArmHvcLib.inf
  ArmMonitorLib|ArmPkg/Library/ArmMonitorLib/ArmMonitorLib.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.inf
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  # edk2 master unified ARM exception handling into UefiCpuPkg and folded
  # ArmGicLib into ArmGicDxe; DefaultExceptionHandlerLib is no longer separate.
  CpuExceptionHandlerLib|UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeCpuExceptionHandlerLib.inf

  # --- ArmPlatformPkg: our device memory map lives here ---
  ArmPlatformLib|Platform/Star2LtePkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf

  # --- Generic services ---
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf

  # --- SCSI/UFS storage stack ---
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf

  # --- FV / decompression ---
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  LzmaDecompressLib|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  UefiDecompressLib|MdePkg/Library/BaseUefiDecompressLib/BaseUefiDecompressLib.inf
  PerformanceLib|MdePkg/Library/BasePerformanceLibNull/BasePerformanceLibNull.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf

  # --- Misc null/instances ---
  DebugAgentLib|MdeModulePkg/Library/DebugAgentLibNull/DebugAgentLibNull.inf
  PciExpressLib|MdePkg/Library/BasePciExpressLib/BasePciExpressLib.inf
  PciLib|MdePkg/Library/BasePciLibPciExpress/BasePciLibPciExpress.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf
  ImagePropertiesRecordLib|MdeModulePkg/Library/ImagePropertiesRecordLib/ImagePropertiesRecordLib.inf
  OrderedCollectionLib|MdePkg/Library/BaseOrderedCollectionRedBlackTreeLib/BaseOrderedCollectionRedBlackTreeLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf
  SecurityManagementLib|MdeModulePkg/Library/DxeSecurityManagementLib/DxeSecurityManagementLib.inf
  UefiRuntimeLib|MdePkg/Library/UefiRuntimeLib/UefiRuntimeLib.inf
  TimeBaseLib|EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.inf

  # --- Reset + RTC ---
  # Prefer PSCI reset/shutdown, then use the Exynos9810 PMU when the secure
  # monitor returns. The PMU pages are made runtime-addressable so Windows
  # ResetSystem calls cannot fall back into the runtime driver.
  ResetSystemLib|Platform/Star2LtePkg/Library/Star2LteResetSystemLib/Star2LteResetSystemLib.inf
  # Virtual RTC for bring-up (no Exynos RTC driver yet). Time won't persist.
  RealTimeClockLib|EmbeddedPkg/Library/VirtualRealTimeClockLib/VirtualRealTimeClockLib.inf

  # --- BDS / boot manager (launches Windows) ---
  UefiBootManagerLib|MdeModulePkg/Library/UefiBootManagerLib/UefiBootManagerLib.inf
  PlatformBootManagerLib|Platform/Star2LtePkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibNull/DxeCapsuleLibNull.inf

  # --- UEFI variable services backing libs ---
  AuthVariableLib|MdeModulePkg/Library/AuthVariableLibNull/AuthVariableLibNull.inf
  VarCheckLib|MdeModulePkg/Library/VarCheckLib/VarCheckLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLib.inf
  VariablePolicyHelperLib|MdeModulePkg/Library/VariablePolicyHelperLib/VariablePolicyHelperLib.inf
  VariableFlashInfoLib|MdeModulePkg/Library/BaseVariableFlashInfoLib/BaseVariableFlashInfoLib.inf
  TpmMeasurementLib|MdeModulePkg/Library/TpmMeasurementLibNull/TpmMeasurementLibNull.inf

[LibraryClasses.common.SEC]
  # PeilessSec (combined SEC+PEI) flow — replaces the removed ArmPlatformPkg/PrePi
  PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf
  HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
  PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf
  MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf
  MemoryInitPeiLib|ArmPlatformPkg/MemoryInitPei/MemoryInitPeiLib.inf
  PlatformPeiLib|ArmPlatformPkg/PlatformPei/PlatformPeiLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  # --- PeilessSec extras (new requirements vs the old PrePi) ---
  PeilessSecMeasureLib|SecurityPkg/Library/PeilessSecMeasureLib/PeilessSecMeasureLibNull.inf
  ArmTransferListLib|ArmPkg/Library/ArmTransferListLib/ArmTransferListLib.inf
  # SEC-safe ExtractGuidedSectionLib (the DXE instance rejects MODULE_TYPE=SEC).
  ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf

[LibraryClasses.common.DXE_CORE]
  HobLib|MdePkg/Library/DxeCoreHobLib/DxeCoreHobLib.inf
  MemoryAllocationLib|MdeModulePkg/Library/DxeCoreMemoryAllocationLib/DxeCoreMemoryAllocationLib.inf
  DxeCoreEntryPoint|MdePkg/Library/DxeCoreEntryPoint/DxeCoreEntryPoint.inf

[LibraryClasses.common.DXE_DRIVER]
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf

[LibraryClasses.common.UEFI_DRIVER, LibraryClasses.common.UEFI_APPLICATION]
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLibRuntimeDxe.inf

################################################################################
#
# Feature + fixed PCDs
#
################################################################################
[PcdsFeatureFlag.common]
  gEfiMdeModulePkgTokenSpaceGuid.PcdHiiOsRuntimeSupport|FALSE

[PcdsFixedAtBuild.common]
  # --- Debug mask (errors+warnings+info+load). Trim for RELEASE. ---
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8000004F
  # PcdDebugPropertyMask: 0x0E = PRINT|CODE|CLEAR_MEMORY, but ASSERT (0x01) and
  # ASSERT_DEADLOOP (0x20) DISABLED. On this chainloaded device with no draining UART,
  # a DEBUG-build ASSERT(FALSE) -> CpuDeadLoop() hard-hangs the boot (hit in EDK2's
  # DumpQueryResponseResult default case during UFS device-init). Making ASSERT a no-op
  # keeps any stray EDK2 assertion deeper in enumeration from wedging bring-up.
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x0E

  # --- UFS device-init completion poll budget (UfsFinishDeviceInitialization
  #     fDeviceInit ReadFlag loop). Default 600000 iterations; during Exynos UFS
  #     bring-up a not-yet-clearing flag would make that loop run effectively
  #     forever, so cap it small to fail fast and reach the on-screen status panel.
  #     TODO: restore default once UFS is stable. ---
  gEfiMdeModulePkgTokenSpaceGuid.PcdUfsInitialCompletionTimeout|2000

  # --- System DRAM. Window base 0x90000000 (RESTORED after ext72).
  #
  #     ext72 tried MEMORY-BASE PARITY WITH MU (base 0x80000000, expose the whole low
  #     bank incl. the "RKP kernel zone" 0x80094000-0x828DBFFF as normal RAM, matching
  #     RamManagerLib on starlte/9810). RESULT = REGRESSION, REVERTED. PRAM
  #     (pram-ext72-first-20260708-003520) proved winload then ran INSIDE the low bank
  #     (its own code executed at ra=0x80c14xxx) and placed its 4 MB transition page
  #     tables at 0x82000000 / 0x82400000 (the RKP zone). winload loaded the kernel
  #     (bio sz=17df6000) but HUNG at the final GetMemoryMap (bt0c0) BEFORE
  #     ExitBootServices — never reached =EBSOK, never reached the kernel tlbi. i.e.
  #     exposing the low bank as *conventional* RAM lets winload occupy the
  #     RKP-protected zone and die there. Base 0x90000000 keeps winload/UEFI entirely
  #     ABOVE the RKP zone (proven to reach the kernel: =EBSOK + =T0 0x907aa000, hang
  #     is the kernel's own tlbi at v15). Mu boots the same low-bank layout only
  #     because Mu ALSO exposes the high banks (0xC0000000..0xE0000000, 0xE1900000+)
  #     which steer its winload's allocator away from 0x82xxxxxx; matching that needs
  #     multi-region HOBs (RamManagerLib), not a single Base/Size — a larger change to
  #     revisit only if the kernel-PT scan (=PTSCAN) is inconclusive.
  #     Window: 0x90000000 .. 0xBC800000 (excludes RKP zone + both secure regions). ---
  # ext99 HIGH-ROOT TEST (base RAISED 0x90000000 -> 0x98000000) -- DONE, REVERTED.
  #   RESULT: the mechanism WORKS but the single-base lever is NON-VIABLE.
  #   - Confounding bug found+fixed: the UFS data bounce (Star2LteUfsHcDxe Star2LteAllocWcPages)
  #     hardcoded an AllocateMaxAddress ceiling of 0x98000000 (old A83 low-bounce experiment);
  #     with the base at 0x98000000 there is no conventional RAM below it, so every UFS bounce
  #     alloc failed E_OUT_OF_RESOURCES (138x) and the boot died in storage before winload. Fixed
  #     by restoring MAX_ADDRESS (high bounce), decoupling UFS from the base.
  #   - With UFS fixed the boot advanced: the kernel TTBR1 root DID move up (=LVMEL t1 0x82xxxxxx
  #     -> 0x980aa800, tracking the raise), confirming a raised floor steers winload's kernel-PT
  #     allocation up. BUT winload then HUNG at its final kernel-context restore: reached =KCTXV
  #     (VBAR_EL1 write) and never reached =EBSENTER/ExitBootServices (pram-ext99b-recov1-*).
  #     Same class as ext72/74: any single Base/Size change destabilizes winload's handoff.
  #   CONCLUSION: to place the kernel root high the correct lever is Mu-style MULTI-REGION HOBs
  #   that also expose the HIGH banks (0xC0000000+) to steer winload's allocator (RamManagerLib
  #   parity), NOT a single Base/Size raise. Reverted to the known-good winload-stable window.
  #   Window: 0x90000000 .. 0xBC800000 (excludes RKP zone + both secure regions).
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x0000000090000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x000000002C800000
  gExynos9810TokenSpaceGuid.PcdSystemMemoryBase|0x0000000090000000
  gExynos9810TokenSpaceGuid.PcdSystemMemorySize|0x000000002C800000

  # --- Debug UART base (Milestone 1). NOT in mainline; 0x10440000 is the
  #     downstream PERIC0 USI/UART candidate — CONFIRM on-device. ---
  gExynos9810TokenSpaceGuid.PcdSerialRegisterBase|0x0000000010440000

  # --- GICv2 (gic-400), VERIFIED. Distributor + memory-mapped CPU interface.
  #     GICv2 uses PcdGicInterruptInterfaceBase (GICC), NOT redistributors. ---
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x0000000010101000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0x0000000010102000

  # --- CPU topology (4x A55 + 4x M3). ---
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|2
  # Primary (boot) core MpId. sboot most likely enters on LITTLE cluster0/core0
  # == 0x0. TODO-VERIFY from the Milestone-1 UART log (print MPIDR_EL1).
  # NOTE: edk2 master moved this PCD to gArmTokenSpaceGuid.
  gArmTokenSpaceGuid.PcdArmPrimaryCore|0x0

  # PeilessSec stack + UEFI region sizing (replaces the removed ArmPlatformStackLib).
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x00010000
  gArmPlatformTokenSpaceGuid.PcdSystemMemoryUefiRegionSize|0x08000000

  # --- Architected generic timer GSIVs (match GTDT). VERIFIED from DT. ---
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|29
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|30
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|27
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|26
  # CRITICAL (runtime): stock sboot does NOT set CNTFRQ_EL0; the real rate is
  # 26 MHz. edk2 master REMOVED PcdArmArchTimerFreqInHz (timer libs now read
  # CNTFRQ_EL0 directly). On this device CNTFRQ_EL0 reads 0 and is RO at EL1
  # (writable only at EL3), so we cannot program it. HANDLED: a workspace-local
  # edit to ArmPkg/Library/ArmGenericTimerPhyCounterLib makes
  # ArmGenericTimerGetTimerFreq() fall back to 26 MHz (0x18CBA80) when CNTFRQ
  # reads 0 — the single chokepoint used by both ArmArchTimerLib (TimerLib) and
  # TimerDxe, so the ASSERT(TimerFreq != 0) and the period math both get 26 MHz.
  # gArmTokenSpaceGuid.PcdArmArchTimerFreqInHz|26000000   # PCD removed upstream

  # --- Firmware volume placement. MUST match Star2LtePkg.fdf. A relocating stub
  #     (m1-hello start.S style) copies the FD from sboot's load address up to
  #     0x90000000 — clean RAM ABOVE the RKP-monitored kernel zones
  #     (0x80094000-0x828DBFFF). 0x80080000 bootlooped (RKP reset on writes into
  #     the kernel-code region); 0x90000000 is device-proven and matches the
  #     edk2-exynos reference (configs/exynos9820.conf FD_BASE=0x90000000). ---
  gArmTokenSpaceGuid.PcdFdBaseAddress|0x0000000090000000
  gArmTokenSpaceGuid.PcdFdSize|0x00700000
  gArmTokenSpaceGuid.PcdFvBaseAddress|0x0000000090000000
  gArmTokenSpaceGuid.PcdFvSize|0x00700000

  # --- VFP / NEON: PcdVFPEnabled was removed upstream (handled via build flags). ---
  # gArmTokenSpaceGuid.PcdVFPEnabled|1

  # --- UEFI variable store: memory-backed (non-persistent) for bring-up.
  #     Windows sets BootOrder/BootXXXX here. Persisting them to a real flash
  #     partition is future work (see docs/08-edk2-internals.md). ---
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|TRUE

  # --- BDS: short timeout; show the boot menu only briefly. ---
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|3

################################################################################
#
# Components — build order: SEC(PrePi) -> DXE core -> DXE/BDS drivers
#
################################################################################
[Components.common]
  #
  # Milestone 1 — build/verify the UART library in isolation first:
  #   build -p Platform/Star2LtePkg/Star2LtePkg.dsc -m \
  #     Silicon/Exynos9810Pkg/Library/Exynos9810SerialPortLib/Exynos9810SerialPortLib.inf
  #
  Silicon/Exynos9810Pkg/Library/Exynos9810SerialPortLib/Exynos9810SerialPortLib.inf

  #
  # Milestone 2+ — full boot chain. These mirror Star2LtePkg.fdf. The DSC must
  # list every module the FDF places, so keep the two in sync.
  #
  # --- SEC / PEI (PeilessSec replaces the removed ArmPlatformPkg/PrePi) ---
  ArmPlatformPkg/PeilessSec/PeilessSec.inf {
    <LibraryClasses>
      #
      # The DXE firmware volume (FVMAIN) is stored LZMA-compressed inside
      # FVMAIN_COMPACT (GUIDED section EE4E5898-... = LZMA_CUSTOM_DECOMPRESS_GUID).
      # PeilessSec's DecompressFirstFv -> FfsProcessFvFile extracts it via
      # ExtractGuidedSectionGetInfo/Decode, which only works if a handler for that
      # GUID has been REGISTERED. LzmaCustomDecompressLib registers it from its
      # CONSTRUCTOR (LIBRARY_CLASS = NULL), so it must be linked into THIS module
      # to have ProcessLibraryConstructorList() run that constructor. Without it,
      # DecompressFirstFv returns an error and SecMain's ASSERT_EFI_ERROR
      # CpuDeadLoops (device-confirmed: SEC breadcrumb trail stops at 'G', no 'H').
      #
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  }

  # --- DXE core + PCD ---
  MdeModulePkg/Core/Dxe/DxeMain.inf
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

  # --- Architectural protocols ---
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  MdeModulePkg/Universal/Metronome/Metronome.inf
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  # The DXE core requires the full set of architectural protocols before it will
  # hand off to BDS (CoreAllEfiServicesAvailable ASSERTs otherwise — device-
  # confirmed: DxeMain breadcrumb '8' prints after CoreDispatcher, then hangs).
  # These three were missing: Watchdog (generic software timer, no SoC HW WDT so
  # no Exynos PMU/RKP conflict), Capsule (stub), Monotonic Counter.
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  MdeModulePkg/Universal/MonotonicCounterRuntimeDxe/MonotonicCounterRuntimeDxe.inf

  # --- Variable services (memory-backed; PcdEmuVariableNvModeEnable=TRUE) ---
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf

  # --- Console: first-light framebuffer + serial + splitter ---
  # HiiDatabaseDxe FIRST — produces the 3 HII protocols (Database/String/
  # ConfigRouting) that BdsDxe's UefiHiiServicesLib-injected DEPEX requires.
  # Without it BdsDxe never dispatches and the Bds arch protocol is missing.
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  Platform/Star2LtePkg/Drivers/SimpleFramebufferDxe/SimpleFramebufferDxe.inf
  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf

  # --- ACPI (FADT/MADT/GTDT + DSDT) ---
  MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf
  Platform/Star2LtePkg/Drivers/Star2LteAcpiPlatformDxe/Star2LteAcpiPlatformDxe.inf
  Platform/Star2LtePkg/AcpiTables/AcpiTables.inf

  # --- RAM discovery (publish the verified high DRAM banks to DXE) ---
  Platform/Star2LtePkg/Drivers/Star2LteRamManagerDxe/Star2LteRamManagerDxe.inf

  # --- Storage + filesystem (Windows ESP on UFS) ---
  # Star2Lte UFS host controller produces EDKII_UFS_HOST_CONTROLLER_PROTOCOL over
  # the Exynos MMIO host (ufs@0x11120000); UfsPassThruDxe binds it and produces
  # ExtScsiPassThru; ScsiBus/ScsiDisk expose BlockIo; Partition/DiskIo/Fat expose
  # the EFI System Partition so BDS can find \EFI\Microsoft\Boot\bootmgfw.efi.
  Platform/Star2LtePkg/Drivers/Star2LteUfsHcDxe/Star2LteUfsHcDxe.inf
  #
  # USB host hand-off. Built here but not in the FDF: it is added, with its SSDT
  # (UsbHost.asl), to the released image by tools/add-usb-host.ps1.
  #
  Platform/Star2LtePkg/Drivers/Star2LteUsbHostDxe/Star2LteUsbHostDxe.inf
  MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  # EnglishDxe produces gEfiUnicodeCollation2Protocol, which EnhancedFatDxe
  # REQUIRES to start (Fat.inf lists it ## TO_START). Without a collation
  # provider, FatDriverBindingStart fails and Fat mounts ZERO volumes (fs=0),
  # so the Windows ESP never appears. (A18 diag: 3 valid FAT parts, fs=00.)
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf

  # --- BDS: finds + launches the Windows Boot Manager ---
  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf
  # Optional (not required to launch bootmgfw.efi). Re-add together with the
  # ShellPkg library tail once the minimal FD boots on device.
  # MdeModulePkg/Application/UiApp/UiApp.inf
  # ShellPkg/Application/Shell/Shell.inf
