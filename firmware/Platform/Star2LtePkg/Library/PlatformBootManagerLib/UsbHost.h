/** @file
  USB host hand-off for Star2Lte: leaves the DWC3 controller in xHCI host mode,
  with VBUS supplied, for the Windows inbox xHCI driver (ACPI PNP0D10).
**/

#ifndef STAR2LTE_USB_HOST_H_
#define STAR2LTE_USB_HOST_H_

#include "UsbDebug.h"

#ifndef STAR2LTE_USB_HOST
#define STAR2LTE_USB_HOST  0
#endif

#if STAR2LTE_USB_HOST && (STAR2LTE_USB_DEBUG_MODE == STAR2LTE_USB_DEBUG_HID)
#error STAR2LTE_USB_HOST and the USB HID debug transport both own the DWC3 controller; pick one.
#endif

#if STAR2LTE_USB_HOST
EFI_STATUS
Star2LteUsbHostStart (
  IN STAR2LTE_USB_DEBUG_LOG  Log
  );
#endif

#endif
