/** @file
  USB host hand-off for Star2Lte.

  Leaves the DWC3 controller at 0x10C00000 in xHCI host mode for Windows, which
  binds its inbox xHCI driver to the DSDT's PNP0D10 device (USB0). Three things
  have to be true for a device on the USB-C port to enumerate:

    1. VBUS. The phone is the 5 V source in host mode. The MAX77705 charger boosts
       the battery onto CHGIN when CHG_CNFG_00 has OTG and BOOST set, exactly as
       Samsung's max77705_set_otg() does. It is only switched on when nothing is
       supplying CHGIN (CHG_INT_OK.CHGIN_OK clear), so the phone never pushes 5 V
       into a charger or a PC port.
    2. PHY. The USB2 PHY is brought up the way UsbDebugHid.c (validated in device
       mode) and phy_exynos_usb_v3p1_enable() do it, then switched to host the way
       phy_exynos_usb_v3p1_config_host_mode() does: D+/D- pull-downs on, the
       forced VBUS-valid/B-valid signals off.
    3. Controller. GCTL.PRTCAPDIR = host. Windows' xHCI driver resets and runs
       the xHCI part itself.

  Everything is guarded: the boot-loader-programmed USB clocks are checked (not
  changed), and the HSI2C controller is only used if the boot loader left its
  timing programmed.

  A standalone DXE driver, so it can be added to the released firmware image
  (FMMT) without rebuilding its BDS. Each step is logged to the volatile, runtime
  UEFI variable Star2LteUsbHost, which Windows can read; the BDS record ring is
  overwritten by the storage driver once Windows starts.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Platform/Exynos9810.h>

//
// Vendor GUID of the Star2LteUsbHost log variable.
//
STATIC EFI_GUID  mStar2LteUsbHostLogGuid = {
  0x3c61ef0f, 0x2a3e, 0x4f7b, { 0x9a, 0x6b, 0x85, 0x10, 0x9e, 0x81, 0x0c, 0x55 }
};

//
// USB clocks, as left running by the boot loader (it uses USB for Download mode).
// Same registers and expected values as UsbDebugHid.c.
//
#define USB_TOP_MUX                    (EXYNOS_CMU_TOP_BASE + 0x1060u)
#define USB_TOP_DIV                    (EXYNOS_CMU_TOP_BASE + 0x1858u)
#define USB_TOP_GATE                   (EXYNOS_CMU_TOP_BASE + 0x2064u)
#define USB_FSYS_MUX                   (EXYNOS_CMU_FSYS0_BASE + 0x01E0u)
#define USB_FSYS_MUX_AUTOGATE          (EXYNOS_CMU_FSYS0_BASE + 0x0228u)
#define USB_FSYS_GATE                  (EXYNOS_CMU_FSYS0_BASE + 0x206Cu)
#define USB_CLK_AUTOGATE               BIT28
#define USB_CLK_GATE_MANUAL            BIT20
#define USB_CLK_GATE_VALUE             BIT21
#define USB_TOP_MUX_SELECT_MASK        0x3u
#define USB_TOP_MUX_SELECT_PLL2_DIV2   0x2u
#define USB_TOP_MUX_BUSY               BIT16
#define USB_TOP_DIV_RATIO_MASK         0xFu
#define USB_TOP_DIV_RATIO_NORMAL       0x7u
#define USB_TOP_DIV_BUSY               BIT16
#define USB_FSYS_MUX_SELECT            BIT4
#define USB_FSYS_MUX_BUSY              BIT7

//
// USBCON (PHY/link control) at EXYNOS_USBDRD_PHY_BASE.
//
#define USB_PHY_LINK_CTRL              0x04u
#define USB_PHY_CLKRST                 0x20u
#define USB_PHY_COMBO_PMA_CTRL         0x48u
#define USB_PHY_UTMI                   0x50u
#define USB_PHY_HSP                    0x54u
#define USB_PHY_HSP_TEST               0x5Cu
#define USB_PHY_LINK_PIPE_RX_IDLE      BIT18
#define USB_PHY_LINK_PIPE_STATUS       BIT17
#define USB_PHY_LINK_PIPE_FORCE        BIT16
#define USB_PHY_LINK_DIS_QACT_LINK     BIT12
#define USB_PHY_LINK_DIS_QACT_ID0      BIT11
#define USB_PHY_LINK_DIS_QACT_VBUS     BIT10
#define USB_PHY_LINK_DIS_QACT_BVALID   BIT9
#define USB_PHY_LINK_FORCE_QACT        BIT8
#define USB_PHY_LINK_FILTER_MASK       (0xFu << 4)
#define USB_PHY_CLKRST_PHY_RESET       BIT3
#define USB_PHY_CLKRST_PHY_SELECT      BIT2
#define USB_PHY_CLKRST_PORT_RESET      BIT1
#define USB_PHY_CLKRST_LINK_RESET      BIT0
#define USB_PHY_PMA_LOW_POWER_N        BIT4
#define USB_PHY_UTMI_VBUS_VALID        BIT5
#define USB_PHY_UTMI_BVALID            BIT4
#define USB_PHY_UTMI_DP_PULLDOWN       BIT3
#define USB_PHY_UTMI_DM_PULLDOWN       BIT2
#define USB_PHY_UTMI_SUSPEND           BIT1
#define USB_PHY_UTMI_SLEEP             BIT0
#define USB_PHY_HSP_VBUS_EXT_SELECT    BIT13
#define USB_PHY_HSP_VBUS_EXT           BIT12
#define USB_PHY_HSP_UTMI_SUSPEND       BIT9
#define USB_PHY_HSP_COMMON_ON          BIT8
#define USB_PHY_HSP_TEST_SIDDQ         BIT24

//
// DWC3 global registers (offsets from EXYNOS_USBDRD_BASE).
//
#define DWC3_GSBUSCFG0                 0xC100u
#define DWC3_GSBUSCFG1                 0xC104u
#define DWC3_GCTL                      0xC110u
#define DWC3_GSNPSID                   0xC120u
#define DWC3_GUCTL                     0xC12Cu
#define DWC3_GUSB2PHYCFG0              0xC200u
#define DWC3_GUSB3PIPECTL0             0xC2C0u
#define DWC3_GCTL_PRTCAP_MASK          (0x3u << 12)
#define DWC3_GCTL_PRTCAP_HOST          (0x1u << 12)
#define DWC3_GCTL_CORE_SOFT_RESET      BIT11
#define DWC3_GCTL_SOFITPSYNC           BIT10
#define DWC3_GCTL_DSBLCLKGTNG          BIT0
#define DWC3_GSBUSCFG0_INCRBRSTEN      BIT0
#define DWC3_GSBUSCFG0_INCR16BRSTEN    BIT3
#define DWC3_GSBUSCFG0_DESWRREQINFO    (2u << 16)
#define DWC3_GSBUSCFG0_DATWRREQINFO    (2u << 20)
#define DWC3_GSBUSCFG0_DESRDREQINFO    (2u << 24)
#define DWC3_GSBUSCFG0_DATRDREQINFO    (2u << 28)
#define DWC3_GSBUSCFG1_BREQLIMIT_MASK  (0xFu << 8)
#define DWC3_GSBUSCFG1_BREQLIMIT(V)    (((V) & 0xFu) << 8)
#define DWC3_GUCTL_REFCLKPER_MASK      (0x3FFu << 22)
#define DWC3_GUCTL_REFCLKPER(V)        (((V) & 0x3FFu) << 22)
#define DWC3_GUCTL_USBHSTINAUTORETRYEN BIT14
#define DWC3_GUCTL_SPRSCTRLTRANSEN     BIT17
#define DWC3_PHY_SOFT_RESET            BIT31
#define DWC3_GUSB2PHYCFG_U2_FREECLK    BIT30
#define DWC3_GUSB2PHYCFG_SUSPHY        BIT6
#define DWC3_GUSB2PHYCFG_ENBLSLPM      BIT8
#define DWC3_GUSB3PIPECTL_DISRXDETINP3 BIT28
#define DWC3_GUSB3PIPECTL_U1U2EXITFAIL BIT25
#define DWC3_GUSB3PIPECTL_SUSPHY       BIT17
#define DWC3_SNPSID_MASK               0xFFFF0000u
#define DWC3_SNPSID_DWC3               0x55330000u

//
// HSI2C at 0x14360000 (live DT: hsi2c@14360000, max77705@66). Same register block
// as the HSI2C10 engine in drivers/Exynos9810Hsi2c, used polled, without touching
// the timing the boot loader programmed: this bus's source clock is not the
// 200 MHz PERIC0 clock the touchscreen driver's timing math assumes.
//
#define MAX77705_HSI2C_BASE            0x0000000014360000ULL
#define EHI2C_CONTROL                  0x0000u
#define EHI2C_FIFO_CONTROL             0x0004u
#define EHI2C_TRAILING_CONTROL         0x0008u
#define EHI2C_INTERRUPT_ENABLE         0x0020u
#define EHI2C_INTERRUPT_STATUS         0x0024u
#define EHI2C_FIFO_STATUS              0x0030u
#define EHI2C_TX_DATA                  0x0034u
#define EHI2C_RX_DATA                  0x0038u
#define EHI2C_CONFIGURATION            0x0040u
#define EHI2C_AUTO_CONFIGURATION       0x0044u
#define EHI2C_TIMEOUT                  0x0048u
#define EHI2C_TIMING_FS3               0x0068u
#define EHI2C_ADDRESS                  0x0070u
#define EHI2C_CONTROL_MASTER           BIT3
#define EHI2C_CONTROL_RX_CHANNEL       BIT6
#define EHI2C_CONTROL_TX_CHANNEL       BIT7
#define EHI2C_FIFO_RX_ENABLE           BIT0
#define EHI2C_FIFO_TX_ENABLE           BIT1
#define EHI2C_FIFO_RX_TRIGGER(L)       ((UINT32)(L) << 4)
#define EHI2C_FIFO_TX_TRIGGER(L)       ((UINT32)(L) << 16)
#define EHI2C_INTERRUPT_TX_ALMOST_EMPTY BIT0
#define EHI2C_INTERRUPT_RX_ALMOST_FULL BIT1
#define EHI2C_INTERRUPT_TRANSFER_DONE  BIT7
#define EHI2C_INTERRUPT_ERROR_MASK     (BIT8 | BIT9 | BIT10 | BIT11)
#define EHI2C_INTERRUPT_ALL            0x00000FFFu
#define EHI2C_FIFO_TX_FULL             BIT7
#define EHI2C_FIFO_RX_LEVEL(V)         (((V) >> 16) & 0x7Fu)
#define EHI2C_CONFIGURATION_AUTO_MODE  BIT31
#define EHI2C_AUTO_READ                BIT16
#define EHI2C_AUTO_STOP                BIT17
#define EHI2C_AUTO_RUN                 BIT31
#define EHI2C_TRAILING_COUNT_MAX       0x00FFFFFFu
#define EHI2C_ADDRESS_MASTER(A)        (((UINT32)(A) & 0x3FFu) << 10)
#define EHI2C_TIMING_FS3_DIVIDER(V)    (((V) >> 16) & 0xFFu)
#define EHI2C_POLL_US                  20000u

//
// MAX77705 charger (I2C_ADDR_CHG = 0xD2 >> 1 in drivers/mfd/max77705.c), registers
// from include/linux/mfd/max77705-private.h and max77705_charger.h.
//
#define MAX77705_CHG_ADDRESS           0x69u
#define MAX77705_CHG_REG_INT_OK        0xB2u
#define MAX77705_CHG_REG_CNFG_00       0xB7u
#define MAX77705_CHG_REG_CNFG_06       0xBDu
#define MAX77705_CHG_REG_CNFG_08       0xBFu
#define MAX77705_CHGIN_OK              BIT6
#define MAX77705_CNFG_00_OTG_CTRL      (BIT1 | BIT3)    // OTG | BOOST
#define MAX77705_CNFG_06_CHGPROT       (0x3u << 2)      // unlock charger settings
#define MAX77705_CNFG_08_FSW_MASK      0x3u
#define MAX77705_CNFG_08_FSW_3MHZ      0x0u

STATIC CHAR8  mUsbHostLog[1024];
STATIC UINTN  mUsbHostLogLength;

STATIC
VOID
UsbHostLogValue (
  IN CONST CHAR8  *Tag,
  IN UINT32       Value
  )
{
  if (mUsbHostLogLength + 32 >= sizeof (mUsbHostLog)) {
    return;
  }

  mUsbHostLogLength += AsciiSPrint (
                         mUsbHostLog + mUsbHostLogLength,
                         sizeof (mUsbHostLog) - mUsbHostLogLength,
                         "%a=%08x\n",
                         Tag,
                         Value
                         );
}

STATIC
UINT32
DwcRead (
  IN UINT32  Offset
  )
{
  return MmioRead32 ((UINTN)(EXYNOS_USBDRD_BASE + Offset));
}

STATIC
VOID
DwcWrite (
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 ((UINTN)(EXYNOS_USBDRD_BASE + Offset), Value);
}

STATIC
UINT32
PhyRead (
  IN UINT32  Offset
  )
{
  return MmioRead32 ((UINTN)(EXYNOS_USBDRD_PHY_BASE + Offset));
}

STATIC
VOID
PhyWrite (
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 ((UINTN)(EXYNOS_USBDRD_PHY_BASE + Offset), Value);
}

STATIC
UINT32
Hsi2cRead (
  IN UINT32  Offset
  )
{
  return MmioRead32 ((UINTN)(MAX77705_HSI2C_BASE + Offset));
}

STATIC
VOID
Hsi2cWrite (
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 ((UINTN)(MAX77705_HSI2C_BASE + Offset), Value);
}

/**
  The boot loader leaves the USB clock tree running for Download mode. Check it
  is still the configuration UsbDebugHid.c was validated against; never change it.
  Returns a bitmask of mismatches (0 = all as expected).
**/
STATIC
UINT32
UsbHostCheckClocks (
  VOID
  )
{
  UINT32  Errors;
  UINT32  Value;

  Errors = 0;

  Value = MmioRead32 ((UINTN)USB_TOP_MUX);
  if ((Value & (USB_TOP_MUX_SELECT_MASK | USB_TOP_MUX_BUSY | USB_CLK_AUTOGATE)) != USB_TOP_MUX_SELECT_PLL2_DIV2) {
    Errors |= BIT0;
  }

  Value = MmioRead32 ((UINTN)USB_TOP_DIV);
  if ((Value & (USB_TOP_DIV_RATIO_MASK | USB_TOP_DIV_BUSY | USB_CLK_AUTOGATE)) != USB_TOP_DIV_RATIO_NORMAL) {
    Errors |= BIT1;
  }

  Value = MmioRead32 ((UINTN)USB_TOP_GATE);
  if ((Value & (USB_CLK_GATE_MANUAL | USB_CLK_GATE_VALUE | USB_CLK_AUTOGATE)) != USB_CLK_GATE_VALUE) {
    Errors |= BIT2;
  }

  Value = MmioRead32 ((UINTN)USB_FSYS_MUX);
  if ((Value & (USB_FSYS_MUX_SELECT | USB_FSYS_MUX_BUSY)) != USB_FSYS_MUX_SELECT) {
    Errors |= BIT3;
  }

  Value = MmioRead32 ((UINTN)USB_FSYS_MUX_AUTOGATE);
  if ((Value & USB_CLK_AUTOGATE) != 0u) {
    Errors |= BIT4;
  }

  Value = MmioRead32 ((UINTN)USB_FSYS_GATE);
  if ((Value & (USB_CLK_GATE_MANUAL | USB_CLK_GATE_VALUE | USB_CLK_AUTOGATE)) != USB_CLK_GATE_VALUE) {
    Errors |= BIT5;
  }

  return Errors;
}

/**
  One polled HSI2C auto-mode transfer. A transfer without Stop leaves the bus held
  for a repeated start, which is how a register read is done (write the register
  number without Stop, then read with Stop).
**/
STATIC
BOOLEAN
Hsi2cTransfer (
  IN     UINT8    Address,
  IN     BOOLEAN  Read,
  IN OUT UINT8    *Buffer,
  IN     UINTN    Length,
  IN     BOOLEAN  Stop
  )
{
  UINT32  Control;
  UINT32  AutoConfiguration;
  UINT32  Status;
  UINT32  Elapsed;
  UINTN   Done;

  if ((Length == 0) || (Length > 16)) {
    return FALSE;
  }

  Hsi2cWrite (EHI2C_INTERRUPT_STATUS, EHI2C_INTERRUPT_ALL);
  Hsi2cWrite (
    EHI2C_FIFO_CONTROL,
    EHI2C_FIFO_RX_ENABLE | EHI2C_FIFO_TX_ENABLE |
    EHI2C_FIFO_RX_TRIGGER (1) | EHI2C_FIFO_TX_TRIGGER (1)
    );

  Control           = EHI2C_CONTROL_MASTER | (Read ? EHI2C_CONTROL_RX_CHANNEL : EHI2C_CONTROL_TX_CHANNEL);
  AutoConfiguration = (UINT32)Length | (Read ? EHI2C_AUTO_READ : 0u) | (Stop ? EHI2C_AUTO_STOP : 0u);

  Hsi2cWrite (EHI2C_CONTROL, Control);
  Hsi2cWrite (EHI2C_ADDRESS, EHI2C_ADDRESS_MASTER (Address));
  Hsi2cWrite (EHI2C_AUTO_CONFIGURATION, AutoConfiguration);
  Hsi2cWrite (
    EHI2C_INTERRUPT_ENABLE,
    EHI2C_INTERRUPT_TRANSFER_DONE | EHI2C_INTERRUPT_ERROR_MASK |
    (Read ? EHI2C_INTERRUPT_RX_ALMOST_FULL : EHI2C_INTERRUPT_TX_ALMOST_EMPTY)
    );

  Done = 0;
  if (!Read) {
    //
    // At most 16 bytes, well inside the FIFO: preload it before the run bit.
    //
    while ((Done < Length) && ((Hsi2cRead (EHI2C_FIFO_STATUS) & EHI2C_FIFO_TX_FULL) == 0u)) {
      Hsi2cWrite (EHI2C_TX_DATA, Buffer[Done++]);
    }
  }

  Hsi2cWrite (EHI2C_AUTO_CONFIGURATION, AutoConfiguration | EHI2C_AUTO_RUN);

  for (Elapsed = 0; Elapsed < EHI2C_POLL_US; Elapsed += 10) {
    if (Read) {
      while ((Done < Length) && (EHI2C_FIFO_RX_LEVEL (Hsi2cRead (EHI2C_FIFO_STATUS)) != 0u)) {
        Buffer[Done++] = (UINT8)Hsi2cRead (EHI2C_RX_DATA);
      }
    } else {
      while ((Done < Length) && ((Hsi2cRead (EHI2C_FIFO_STATUS) & EHI2C_FIFO_TX_FULL) == 0u)) {
        Hsi2cWrite (EHI2C_TX_DATA, Buffer[Done++]);
      }
    }

    Status = Hsi2cRead (EHI2C_INTERRUPT_STATUS);
    if ((Status & EHI2C_INTERRUPT_ERROR_MASK) != 0u) {
      UsbHostLogValue ("UHI2CERR", Status);
      break;
    }

    if (((Status & EHI2C_INTERRUPT_TRANSFER_DONE) != 0u) && (Done == Length)) {
      Hsi2cWrite (EHI2C_INTERRUPT_STATUS, EHI2C_INTERRUPT_ALL);
      Hsi2cWrite (EHI2C_INTERRUPT_ENABLE, 0);
      return TRUE;
    }

    MicroSecondDelay (10);
  }

  Hsi2cWrite (EHI2C_INTERRUPT_ENABLE, 0);
  Hsi2cWrite (EHI2C_AUTO_CONFIGURATION, 0);
  Hsi2cWrite (EHI2C_FIFO_CONTROL, 0);
  Hsi2cWrite (EHI2C_INTERRUPT_STATUS, EHI2C_INTERRUPT_ALL);
  return FALSE;
}

STATIC
BOOLEAN
Max77705Read (
  IN  UINT8  Register,
  OUT UINT8  *Value
  )
{
  return Hsi2cTransfer (MAX77705_CHG_ADDRESS, FALSE, &Register, 1, FALSE) &&
         Hsi2cTransfer (MAX77705_CHG_ADDRESS, TRUE, Value, 1, TRUE);
}

STATIC
BOOLEAN
Max77705Update (
  IN UINT8  Register,
  IN UINT8  Mask,
  IN UINT8  Bits
  )
{
  UINT8  Value;
  UINT8  Packet[2];

  if (!Max77705Read (Register, &Value)) {
    return FALSE;
  }

  Packet[0] = Register;
  Packet[1] = (UINT8)((Value & ~Mask) | (Bits & Mask));
  return Hsi2cTransfer (MAX77705_CHG_ADDRESS, FALSE, Packet, 2, TRUE);
}

/**
  Switch on the MAX77705 boost so the USB-C port supplies 5 V, the way
  max77705_set_otg(enable) does, but only when nothing is feeding CHGIN.
  Returns TRUE when VBUS was enabled.
**/
STATIC
BOOLEAN
UsbHostEnableVbus (
  VOID
  )
{
  UINT32  Divider;
  UINT8   IntOk;
  UINT8   Config;

  //
  // Only use the bus as the boot loader configured it (it talks to this charger).
  // A zero clock divider means it never did; do not guess this bus's timing.
  //
  Divider = EHI2C_TIMING_FS3_DIVIDER (Hsi2cRead (EHI2C_TIMING_FS3));
  UsbHostLogValue ("UHI2CDIV", Divider);
  if (Divider == 0u) {
    return FALSE;
  }

  Hsi2cWrite (EHI2C_INTERRUPT_ENABLE, 0);
  Hsi2cWrite (EHI2C_CONTROL, EHI2C_CONTROL_MASTER);
  Hsi2cWrite (EHI2C_TRAILING_CONTROL, EHI2C_TRAILING_COUNT_MAX);
  Hsi2cWrite (EHI2C_TIMEOUT, 0);
  Hsi2cWrite (EHI2C_CONFIGURATION, EHI2C_CONFIGURATION_AUTO_MODE);

  if (!Max77705Read (MAX77705_CHG_REG_INT_OK, &IntOk)) {
    UsbHostLogValue ("UHVBUS", 0x80000001u);
    return FALSE;
  }

  UsbHostLogValue ("UHINTOK", IntOk);
  if ((IntOk & MAX77705_CHGIN_OK) != 0u) {
    //
    // A charger or PC is supplying the port: the phone must not drive VBUS.
    //
    UsbHostLogValue ("UHVBUS", 0x00000002u);
    return FALSE;
  }

  if (!Max77705Update (MAX77705_CHG_REG_CNFG_06, MAX77705_CNFG_06_CHGPROT, MAX77705_CNFG_06_CHGPROT) ||
      !Max77705Update (MAX77705_CHG_REG_CNFG_08, MAX77705_CNFG_08_FSW_MASK, MAX77705_CNFG_08_FSW_3MHZ) ||
      !Max77705Update (MAX77705_CHG_REG_CNFG_00, MAX77705_CNFG_00_OTG_CTRL, MAX77705_CNFG_00_OTG_CTRL))
  {
    UsbHostLogValue ("UHVBUS", 0x80000003u);
    return FALSE;
  }

  Config = 0;
  if (Max77705Read (MAX77705_CHG_REG_CNFG_00, &Config)) {
    UsbHostLogValue ("UHCNFG00", Config);
  }

  return (BOOLEAN)((Config & MAX77705_CNFG_00_OTG_CTRL) == MAX77705_CNFG_00_OTG_CTRL);
}

/**
  USB2 PHY bring-up. The first half is UsbDebugHid.c's validated UsbEnablePhy()
  (= phy_exynos_usb_v3p1_enable() with the VBUS pad unused, as on this board);
  the end applies phy_exynos_usb_v3p1_config_host_mode().
**/
STATIC
VOID
UsbHostEnablePhy (
  VOID
  )
{
  UINT32  Value;

  Value  = PhyRead (USB_PHY_LINK_CTRL);
  Value |= USB_PHY_LINK_DIS_QACT_ID0 | USB_PHY_LINK_DIS_QACT_VBUS |
           USB_PHY_LINK_DIS_QACT_BVALID | USB_PHY_LINK_DIS_QACT_LINK;
  Value &= ~USB_PHY_LINK_FORCE_QACT;
  MicroSecondDelay (500);
  PhyWrite (USB_PHY_LINK_CTRL, Value);
  MicroSecondDelay (500);
  PhyWrite (USB_PHY_LINK_CTRL, PhyRead (USB_PHY_LINK_CTRL) | USB_PHY_LINK_FORCE_QACT);
  MicroSecondDelay (500);

  Value = PhyRead (USB_PHY_CLKRST);
  PhyWrite (USB_PHY_CLKRST, Value | USB_PHY_CLKRST_LINK_RESET);
  MicroSecondDelay (10);
  PhyWrite (USB_PHY_CLKRST, Value & ~USB_PHY_CLKRST_LINK_RESET);

  PhyWrite (USB_PHY_CLKRST, PhyRead (USB_PHY_CLKRST) | USB_PHY_CLKRST_PHY_RESET | USB_PHY_CLKRST_PHY_SELECT);
  PhyWrite (USB_PHY_HSP_TEST, PhyRead (USB_PHY_HSP_TEST) & ~USB_PHY_HSP_TEST_SIDDQ);

  Value  = PhyRead (USB_PHY_UTMI);
  Value &= ~(USB_PHY_UTMI_SUSPEND | USB_PHY_UTMI_SLEEP | USB_PHY_UTMI_DP_PULLDOWN | USB_PHY_UTMI_DM_PULLDOWN);
  PhyWrite (USB_PHY_UTMI, Value);
  PhyWrite (USB_PHY_HSP, PhyRead (USB_PHY_HSP) | USB_PHY_HSP_UTMI_SUSPEND | USB_PHY_HSP_COMMON_ON);

  MicroSecondDelay (100);
  Value  = PhyRead (USB_PHY_CLKRST);
  Value |= USB_PHY_CLKRST_PHY_SELECT;
  Value &= ~(USB_PHY_CLKRST_PHY_RESET | USB_PHY_CLKRST_PORT_RESET);
  PhyWrite (USB_PHY_CLKRST, Value);

  Value  = PhyRead (USB_PHY_LINK_CTRL);
  Value |= USB_PHY_LINK_FILTER_MASK | USB_PHY_LINK_PIPE_FORCE | USB_PHY_LINK_PIPE_RX_IDLE;
  Value &= ~USB_PHY_LINK_PIPE_STATUS;
  PhyWrite (USB_PHY_LINK_CTRL, Value);

  PhyWrite (USB_PHY_COMBO_PMA_CTRL, PhyRead (USB_PHY_COMBO_PMA_CTRL) | USB_PHY_PMA_LOW_POWER_N);

  //
  // Host mode (phy_exynos_usb_v3p1_config_host_mode): pull D+/D- down and stop
  // forcing the device-side VBUS-valid/B-valid signals.
  //
  Value  = PhyRead (USB_PHY_UTMI);
  Value |= USB_PHY_UTMI_DP_PULLDOWN | USB_PHY_UTMI_DM_PULLDOWN;
  Value &= ~(USB_PHY_UTMI_BVALID | USB_PHY_UTMI_VBUS_VALID);
  PhyWrite (USB_PHY_UTMI, Value);
  PhyWrite (USB_PHY_HSP, PhyRead (USB_PHY_HSP) & ~(USB_PHY_HSP_VBUS_EXT_SELECT | USB_PHY_HSP_VBUS_EXT));

  MemoryFence ();
}

STATIC
EFI_STATUS
UsbHostStart (
  VOID
  )
{
  UINT32   ClockErrors;
  UINT32   Value;
  BOOLEAN  Vbus;

  ClockErrors = UsbHostCheckClocks ();
  UsbHostLogValue ("UHCLK", ClockErrors);
  if (ClockErrors != 0u) {
    return EFI_NOT_READY;
  }

  Value = DwcRead (DWC3_GSNPSID);
  UsbHostLogValue ("UHID", Value);
  if ((Value & DWC3_SNPSID_MASK) != DWC3_SNPSID_DWC3) {
    return EFI_UNSUPPORTED;
  }

  //
  // VBUS before the PHY, so a device is powered when the port starts looking.
  // Without VBUS the controller is still handed over: a self-powered hub can work.
  //
  Vbus = UsbHostEnableVbus ();
  UsbHostLogValue ("UHVBUSON", Vbus ? 1u : 0u);

  DwcWrite (DWC3_GCTL, DwcRead (DWC3_GCTL) | DWC3_GCTL_CORE_SOFT_RESET);
  DwcWrite (DWC3_GUSB2PHYCFG0, DwcRead (DWC3_GUSB2PHYCFG0) | DWC3_PHY_SOFT_RESET);
  DwcWrite (DWC3_GUSB3PIPECTL0, DwcRead (DWC3_GUSB3PIPECTL0) | DWC3_PHY_SOFT_RESET);
  MicroSecondDelay (100);

  UsbHostEnablePhy ();

  DwcWrite (DWC3_GUSB2PHYCFG0, DwcRead (DWC3_GUSB2PHYCFG0) & ~DWC3_PHY_SOFT_RESET);
  DwcWrite (DWC3_GUSB3PIPECTL0, DwcRead (DWC3_GUSB3PIPECTL0) & ~DWC3_PHY_SOFT_RESET);
  MicroSecondDelay (100);
  DwcWrite (DWC3_GCTL, DwcRead (DWC3_GCTL) & ~DWC3_GCTL_CORE_SOFT_RESET);

  Value  = DwcRead (DWC3_GCTL);
  Value &= ~(DWC3_GCTL_PRTCAP_MASK | DWC3_GCTL_DSBLCLKGTNG);
  Value |= DWC3_GCTL_PRTCAP_HOST | DWC3_GCTL_SOFITPSYNC;
  DwcWrite (DWC3_GCTL, Value);

  //
  // Bus and PHY quirks from the live DT (snps,dis-u2-freeclk-exists-quirk,
  // snps,u1u2_exitfail_quirk), as UsbDebugHid.c programs them.
  //
  Value = DwcRead (DWC3_GSBUSCFG0);
  Value |= DWC3_GSBUSCFG0_INCRBRSTEN | DWC3_GSBUSCFG0_INCR16BRSTEN |
           DWC3_GSBUSCFG0_DESWRREQINFO | DWC3_GSBUSCFG0_DATWRREQINFO |
           DWC3_GSBUSCFG0_DESRDREQINFO | DWC3_GSBUSCFG0_DATRDREQINFO;
  DwcWrite (DWC3_GSBUSCFG0, Value);

  Value  = DwcRead (DWC3_GSBUSCFG1);
  Value &= ~DWC3_GSBUSCFG1_BREQLIMIT_MASK;
  DwcWrite (DWC3_GSBUSCFG1, Value | DWC3_GSBUSCFG1_BREQLIMIT (3));

  Value  = DwcRead (DWC3_GUCTL);
  Value &= ~DWC3_GUCTL_REFCLKPER_MASK;
  Value |= DWC3_GUCTL_REFCLKPER (0x14) | DWC3_GUCTL_USBHSTINAUTORETRYEN | DWC3_GUCTL_SPRSCTRLTRANSEN;
  DwcWrite (DWC3_GUCTL, Value);

  DwcWrite (
    DWC3_GUSB3PIPECTL0,
    DwcRead (DWC3_GUSB3PIPECTL0) | DWC3_GUSB3PIPECTL_DISRXDETINP3 |
    DWC3_GUSB3PIPECTL_U1U2EXITFAIL | DWC3_GUSB3PIPECTL_SUSPHY
    );

  Value  = DwcRead (DWC3_GUSB2PHYCFG0);
  Value &= ~(DWC3_GUSB2PHYCFG_U2_FREECLK | DWC3_GUSB2PHYCFG_SUSPHY | DWC3_GUSB2PHYCFG_ENBLSLPM);
  DwcWrite (DWC3_GUSB2PHYCFG0, Value);

  MemoryFence ();
  UsbHostLogValue ("UHGCTL", DwcRead (DWC3_GCTL));
  UsbHostLogValue ("UHUTMI", PhyRead (USB_PHY_UTMI));
  //
  // xHCI CAPLENGTH/HCIVERSION at offset 0: proves the host register file is live.
  //
  UsbHostLogValue ("UHXHCI", DwcRead (0));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Star2LteUsbHostDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = UsbHostStart ();
  UsbHostLogValue ("UHSTATUS", (UINT32)Status);

  //
  // Volatile and runtime-visible: readable from Windows for this boot only.
  // Logging must never fail the boot, so its status is ignored.
  //
  gRT->SetVariable (
         L"Star2LteUsbHost",
         &mStar2LteUsbHostLogGuid,
         EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
         mUsbHostLogLength,
         mUsbHostLog
         );

  //
  // Nothing stays resident: the hand-off is register state, not a service.
  //
  return EFI_SUCCESS;
}
