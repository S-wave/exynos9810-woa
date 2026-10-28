/** @file
  SSDT for the USB host hand-off (Star2LteUsbHostDxe).

  The DWC3 at usb@10C00000, left in xHCI host mode by UsbHost.c. Windows binds its
  inbox xHCI driver to PNP0D10. Interrupt: DT SPI 0xF4 (244) -> GSIV 276,
  level-high. The live device tree gives the controller no IOMMU and no
  dma-coherent property, so DMA is non-coherent (_CCA 0).

  A separate SSDT rather than a DSDT edit, so the released DSDT stays byte for
  byte as validated. Star2LteAcpiPlatformDxe installs every raw section of the
  ACPI tables file; tools/add-usb-host.ps1 appends this table to it.
**/

DefinitionBlock ("UsbHost.aml", "SSDT", 2, "EXYNOS", "USBHOST", 0x00000001)
{
    Scope (\_SB)
    {
        Device (USB0)
        {
            Name (_HID, "PNP0D10")
            Name (_UID, Zero)
            Name (_CCA, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x10C00000, 0x00010000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 276 }
            })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
    }
}
