<#
.SYNOPSIS
  Print the USB host hand-off log the firmware left for this boot.

.DESCRIPTION
  Star2LteUsbHostDxe records each step (UHCLK, UHID, UHINTOK, UHVBUSON, UHGCTL,
  UHUTMI, UHXHCI, UHSTATUS) in the volatile UEFI variable Star2LteUsbHost. Run
  this in an administrator PowerShell on the phone, in Windows.
#>
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class S9Uefi {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern uint GetFirmwareEnvironmentVariableW(string name, string guid, byte[] buffer, uint size);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool LookupPrivilegeValueW(string system, string name, out long luid);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TokenPrivileges state, uint length, IntPtr previous, IntPtr returnLength);
    [DllImport("kernel32.dll")]
    static extern IntPtr GetCurrentProcess();
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct TokenPrivileges { public uint Count; public long Luid; public uint Attributes; }
    public static void EnableSystemEnvironment() {
        IntPtr token;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28, out token)) throw new System.ComponentModel.Win32Exception();
        var tp = new TokenPrivileges { Count = 1, Attributes = 2 };
        if (!LookupPrivilegeValueW(null, "SeSystemEnvironmentPrivilege", out tp.Luid)) throw new System.ComponentModel.Win32Exception();
        if (!AdjustTokenPrivileges(token, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero)) throw new System.ComponentModel.Win32Exception();
    }
}
'@
[S9Uefi]::EnableSystemEnvironment()
$buffer = New-Object byte[] 4096
$length = [S9Uefi]::GetFirmwareEnvironmentVariableW("Star2LteUsbHost", "{3C61EF0F-2A3E-4F7B-9A6B-85109E810C55}", $buffer, $buffer.Length)
if ($length -eq 0) {
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    if ($err -eq 203) { throw "No Star2LteUsbHost log: this boot's firmware does not carry the USB host driver (or it did not run)." }
    throw (New-Object ComponentModel.Win32Exception $err)
}
[Text.Encoding]::ASCII.GetString($buffer, 0, $length)
