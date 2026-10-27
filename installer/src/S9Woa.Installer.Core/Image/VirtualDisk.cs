// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.ComponentModel;
using System.Globalization;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace S9Woa.Installer.Core.Image;

/// <summary>A virtual disk attached to this PC; disposing it detaches it.</summary>
public interface IAttachedVirtualDisk : IDisposable
{
    /// <summary>The disk number Windows gave it (\\.\PhysicalDriveN).</summary>
    int DiskNumber { get; }
}

/// <summary>Creates and attaches the VHDX the Windows image is built in.</summary>
public interface IVirtualDiskHost
{
    /// <summary>Creates a dynamically expanding VHDX with the given sector sizes and attaches it read-write.</summary>
    IAttachedVirtualDisk CreateAndAttach(string path, long sizeBytes, uint blockSizeBytes, uint logicalSectorBytes, uint physicalSectorBytes);

    /// <summary>Attaches an existing VHDX read-only.</summary>
    IAttachedVirtualDisk AttachReadOnly(string path);

    /// <summary>Detaches the VHDX if anything (an earlier run, Mount-VHD) left it attached, then deletes it.</summary>
    void DetachAndDelete(string path);
}

/// <summary>
/// The Windows virtual disk API (virtdisk.dll), which every edition of Windows 8 and later has.
/// The Hyper-V cmdlets this replaces (New-VHD, Mount-VHD, Dismount-VHD) are missing on Windows
/// Home and fail with permission or WMI errors wherever Hyper-V's management service is not fully
/// set up. Attachments made here last only as long as their handle, so a crashed or cancelled
/// build cannot leave the disk attached and the file locked.
/// </summary>
public sealed class VirtualDiskHost : IVirtualDiskHost
{
    public IAttachedVirtualDisk CreateAndAttach(string path, long sizeBytes, uint blockSizeBytes, uint logicalSectorBytes, uint physicalSectorBytes)
    {
        var storage = Native.Vhdx;
        var create = new Native.CreateParameters
        {
            Version = 2,
            MaximumSize = (ulong)sizeBytes,
            BlockSizeInBytes = blockSizeBytes,
            SectorSizeInBytes = logicalSectorBytes,
            PhysicalSectorSizeInBytes = physicalSectorBytes,
        };
        Check(Native.CreateVirtualDisk(ref storage, path, 0, IntPtr.Zero, 0, 0, ref create, IntPtr.Zero, out var handle),
            $"Could not create the virtual disk {path}");
        return Attach(handle, path, readOnly: false);
    }

    public IAttachedVirtualDisk AttachReadOnly(string path) => Attach(Open(path, readOnly: true), path, readOnly: true);

    public void DetachAndDelete(string path)
    {
        if (!File.Exists(path))
        {
            return;
        }
        using (var handle = Open(path, readOnly: false))
        {
            // Not attached is the normal case and not an error.
            var r = Native.DetachVirtualDisk(handle, 0, 0);
            if (r is not 0 and not Native.ErrorNotReady and not Native.ErrorVirtualDiskNotAttached)
            {
                Check(r, $"Could not detach the leftover virtual disk {path}");
            }
        }
        File.Delete(path);
    }

    private static SafeFileHandle Open(string path, bool readOnly)
    {
        var storage = Native.Vhdx;
        var open = new Native.OpenParameters { Version = 2, ReadOnly = readOnly ? 1 : 0 };
        Check(Native.OpenVirtualDisk(ref storage, path, 0, 0, ref open, out var handle), $"Could not open the virtual disk {path}");
        return handle;
    }

    private static Attached Attach(SafeFileHandle handle, string path, bool readOnly)
    {
        try
        {
            var attach = new Native.AttachParameters { Version = 1 };
            var flags = Native.AttachNoDriveLetter | (readOnly ? Native.AttachReadOnly : 0u);
            Check(Native.AttachVirtualDisk(handle, IntPtr.Zero, flags, 0, ref attach, IntPtr.Zero),
                $"Could not attach the virtual disk {path}");
            var size = 520u;
            var buffer = new char[size / 2];
            Check(Native.GetVirtualDiskPhysicalPath(handle, ref size, buffer), $"Could not find the disk number of {path}");
            return new Attached(handle, ParseDiskNumber(new string(buffer).TrimEnd('\0')));
        }
        catch
        {
            handle.Dispose();
            throw;
        }
    }

    /// <summary>"\\.\PhysicalDrive3" -> 3.</summary>
    internal static int ParseDiskNumber(string physicalPath)
    {
        const string Prefix = @"\\.\PhysicalDrive";
        if (!physicalPath.StartsWith(Prefix, StringComparison.OrdinalIgnoreCase)
            || !int.TryParse(physicalPath.AsSpan(Prefix.Length), NumberStyles.None, CultureInfo.InvariantCulture, out var n))
        {
            throw new InvalidOperationException($"Unexpected virtual disk path: {physicalPath}");
        }
        return n;
    }

    private static void Check(uint error, string what)
    {
        if (error != 0)
        {
            throw new InvalidOperationException($"{what}: {new Win32Exception((int)error).Message} (error {error}).");
        }
    }

    private sealed class Attached(SafeFileHandle handle, int diskNumber) : IAttachedVirtualDisk
    {
        public int DiskNumber { get; } = diskNumber;

        public void Dispose()
        {
            if (handle.IsClosed)
            {
                return;
            }
            // Closing the handle detaches too; detaching first makes it happen before Dispose returns.
            _ = Native.DetachVirtualDisk(handle, 0, 0);
            handle.Dispose();
        }
    }

    private static class Native
    {
        public const uint AttachReadOnly = 0x1;
        public const uint AttachNoDriveLetter = 0x2;
        public const uint ErrorNotReady = 21;
        public const uint ErrorVirtualDiskNotAttached = 0xC03A0015;

        public static VirtualStorageType Vhdx => new()
        {
            DeviceId = 3, // VIRTUAL_STORAGE_TYPE_DEVICE_VHDX
            VendorId = new Guid("ec984aec-a0f9-47e9-901f-71415a66345b"), // VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT
        };

        [StructLayout(LayoutKind.Sequential)]
        public struct VirtualStorageType
        {
            public uint DeviceId;
            public Guid VendorId;
        }

        /// <summary>CREATE_VIRTUAL_DISK_PARAMETERS, version 2, as laid out on 64-bit Windows.</summary>
        [StructLayout(LayoutKind.Explicit, Size = 128)]
        public struct CreateParameters
        {
            [FieldOffset(0)] public int Version;
            [FieldOffset(8)] public Guid UniqueId;
            [FieldOffset(24)] public ulong MaximumSize;
            [FieldOffset(32)] public uint BlockSizeInBytes;
            [FieldOffset(36)] public uint SectorSizeInBytes;
            [FieldOffset(40)] public uint PhysicalSectorSizeInBytes;
            [FieldOffset(48)] public IntPtr ParentPath;
            [FieldOffset(56)] public IntPtr SourcePath;
            [FieldOffset(64)] public int OpenFlags;
            [FieldOffset(68)] public VirtualStorageType ParentVirtualStorageType;
            [FieldOffset(88)] public VirtualStorageType SourceVirtualStorageType;
            [FieldOffset(108)] public Guid ResiliencyGuid;
        }

        /// <summary>OPEN_VIRTUAL_DISK_PARAMETERS, version 2.</summary>
        [StructLayout(LayoutKind.Explicit, Size = 44)]
        public struct OpenParameters
        {
            [FieldOffset(0)] public int Version;
            [FieldOffset(4)] public int GetInfoOnly;
            [FieldOffset(8)] public int ReadOnly;
            [FieldOffset(12)] public Guid ResiliencyGuid;
        }

        /// <summary>ATTACH_VIRTUAL_DISK_PARAMETERS, version 1.</summary>
        [StructLayout(LayoutKind.Explicit, Size = 24)]
        public struct AttachParameters
        {
            [FieldOffset(0)] public int Version;
            [FieldOffset(8)] public uint Reserved;
        }

        [DllImport("virtdisk.dll", CharSet = CharSet.Unicode)]
        public static extern uint CreateVirtualDisk(ref VirtualStorageType storageType, string path, uint accessMask,
            IntPtr securityDescriptor, uint flags, uint providerSpecificFlags, ref CreateParameters parameters,
            IntPtr overlapped, out SafeFileHandle handle);

        [DllImport("virtdisk.dll", CharSet = CharSet.Unicode)]
        public static extern uint OpenVirtualDisk(ref VirtualStorageType storageType, string path, uint accessMask,
            uint flags, ref OpenParameters parameters, out SafeFileHandle handle);

        [DllImport("virtdisk.dll")]
        public static extern uint AttachVirtualDisk(SafeFileHandle handle, IntPtr securityDescriptor, uint flags,
            uint providerSpecificFlags, ref AttachParameters parameters, IntPtr overlapped);

        [DllImport("virtdisk.dll")]
        public static extern uint DetachVirtualDisk(SafeFileHandle handle, uint flags, uint providerSpecificFlags);

        [DllImport("virtdisk.dll", CharSet = CharSet.Unicode)]
        public static extern uint GetVirtualDiskPhysicalPath(SafeFileHandle handle, ref uint diskPathSizeInBytes,
            [Out] char[] diskPath);
    }
}
