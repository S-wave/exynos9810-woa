// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.Globalization;
using S9Woa.Installer.Core.Deploy;
using S9Woa.Installer.Core.Processes;

namespace S9Woa.Installer.Core.Image;

/// <summary>Outputs of a full image build, ready for the transfer stage.</summary>
/// <param name="LoaderSha256">SHA-256 of the image's <c>winload.efi</c>; the UEFI must be built for exactly this loader.</param>
/// <param name="KernelSha256">SHA-256 of the image's <c>ntoskrnl.exe</c>; likewise pinned by the UEFI.</param>
public sealed record BuiltImage(string WindowsImage, long WindowsBytes, string WindowsSha256, string EspDirectory,
    string LoaderSha256 = "", string KernelSha256 = "");

/// <summary>
/// Builds the Windows volume for the phone inside a VHDX that replicates the phone's main
/// UFS unit: a 4Kn disk of the same size with the NTFS Windows partition at exactly the
/// offset and size of USERDATA (plus a small ESP and MSR in the unused space before it).
/// Windows is applied, drivers injected, the slim profile run, the OOBE answer file
/// written and <c>bcdboot</c> laid down; the BCD is pointed at the phone's partitions.
/// The VHDX is then detached and re-attached read-only, and the Windows partition is read
/// straight off the virtual disk, so the exported image is a cleanly unmounted volume whose
/// NTFS geometry matches USERDATA byte for byte. The ESP files are copied out separately.
/// The disk is created and attached through Windows' own virtual disk API
/// (<see cref="VirtualDiskHost"/>), so Hyper-V does not have to be installed or running.
/// </summary>
public sealed class VhdxImageBuilder
{
    private static readonly TimeSpan DiskTimeout = TimeSpan.FromMinutes(15);
    private static readonly TimeSpan BootTimeout = TimeSpan.FromMinutes(10);

    private readonly IProcessRunner _runner;
    private readonly ImageBuilder _imageBuilder;
    private readonly BootConfiguration _boot;
    private readonly RawImageExporter _exporter = new();
    private readonly Func<int, IRawDiskSource> _diskFactory;
    private readonly IVirtualDiskHost _virtualDisks;
    private readonly string _powershell;
    private readonly string _bcdboot;

    public VhdxImageBuilder(IProcessRunner runner, string? system32 = null, Func<int, IRawDiskSource>? diskFactory = null,
        IVirtualDiskHost? virtualDisks = null)
    {
        _virtualDisks = virtualDisks ?? new VirtualDiskHost();
        _runner = runner;
        system32 ??= Environment.GetFolderPath(Environment.SpecialFolder.System);
        _imageBuilder = new ImageBuilder(runner, system32);
        _boot = new BootConfiguration(runner, system32);
        _diskFactory = diskFactory ?? (n => RawDeviceSource.ForPhysicalDrive(n));
        _powershell = Path.Combine(system32, @"WindowsPowerShell\v1.0\powershell.exe");
        _bcdboot = Path.Combine(system32, "bcdboot.exe");
        var free = FreeDriveLetters();
        EspLetter = free.Count > 1 ? free[0] : 'S';
        WindowsLetter = free.Count > 1 ? free[1] : 'W';
    }

    public char EspLetter { get; init; }
    public char WindowsLetter { get; init; }

    /// <summary>Unused drive letters from Z downwards (A–C are never used).</summary>
    internal static IReadOnlyList<char> FreeDriveLetters()
    {
        var used = DriveInfo.GetDrives().Select(d => char.ToUpperInvariant(d.Name[0])).ToHashSet();
        return Enumerable.Range('D', 'Z' - 'D' + 1).Select(c => (char)c).Reverse().Where(c => !used.Contains(c)).ToList();
    }

    private static string N(long v) => v.ToString(CultureInfo.InvariantCulture);

    /// <summary>The replica's sector size: the phone's UFS is a 4096-byte sector device.</summary>
    internal const uint SectorBytes = 4096;

    /// <summary>Block size of the dynamic VHDX (what the earlier New-VHD build used).</summary>
    internal const uint BlockBytes = 32 * 1024 * 1024;

    /// <summary>
    /// PowerShell that waits for the just-attached disk to show up, brings it online (and writable,
    /// unless it was attached read-only) when this PC's SAN policy left it offline, and checks its
    /// sector size. Leaves it in <c>$disk</c>.
    /// </summary>
    private static string[] FindDisk(int diskNumber, bool readOnly) =>
    [
        "$ErrorActionPreference = 'Stop'",
        "$disk = $null",
        $"for ($i = 0; $i -lt 60 -and -not $disk; $i++) {{ Update-HostStorageCache; $disk = Get-Disk -Number {diskNumber} -ErrorAction SilentlyContinue; if (-not $disk) {{ Start-Sleep -Milliseconds 500 }} }}",
        $"if (-not $disk) {{ throw 'The virtual disk did not show up as disk {diskNumber}' }}",
        $"if ($disk.IsOffline) {{ Set-Disk -Number {diskNumber} -IsOffline $false; $disk = Get-Disk -Number {diskNumber} }}",
        .. readOnly ? Array.Empty<string>() :
            [$"if ($disk.IsReadOnly) {{ Set-Disk -Number {diskNumber} -IsReadOnly $false; $disk = Get-Disk -Number {diskNumber} }}"],
        $"if ($disk.LogicalSectorSize -ne {SectorBytes}) {{ throw \"The virtual disk has $($disk.LogicalSectorSize)-byte sectors, not {SectorBytes}\" }}",
    ];

    /// <summary>
    /// PowerShell that lays out the attached 4Kn replica disk. A 512-byte disk would format NTFS
    /// for 512-byte sectors, which will not mount when written raw to USERDATA, so the sector size
    /// and the partition geometry are asserted. It uses only the Storage module, which every
    /// edition of Windows has; the disk itself is created and attached by <see cref="VirtualDiskHost"/>.
    /// </summary>
    internal string CreateScript(int diskNumber) => string.Join("\n", [
        .. FindDisk(diskNumber, readOnly: false),
        $"foreach ($l in '{EspLetter}','{WindowsLetter}') {{ if (Test-Path \"${{l}}:\\\") {{ throw \"Drive ${{l}}: is already in use\" }} }}",
        "Initialize-Disk -Number $disk.Number -PartitionStyle GPT | Out-Null",
        "$esp = New-Partition -DiskNumber $disk.Number -Size 260MB -GptType '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'",
        "Format-Volume -Partition $esp -FileSystem FAT32 -NewFileSystemLabel 'System' -Confirm:$false | Out-Null",
        $"$esp | Set-Partition -NewDriveLetter {EspLetter}",
        "New-Partition -DiskNumber $disk.Number -Size 16MB -GptType '{e3c9e316-0b5c-4db8-817d-f92df00215ae}' | Out-Null",
        $"$win = New-Partition -DiskNumber $disk.Number -Offset {N(PartitionMap.WindowsOffset)} -Size {N(PartitionMap.WindowsBytes)} -Alignment 4096 -GptType '{{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}}'",
        $"if ($win.Offset -ne {N(PartitionMap.WindowsOffset)} -or $win.Size -ne {N(PartitionMap.WindowsBytes)}) {{ throw 'The Windows partition does not match USERDATA' }}",
        "Format-Volume -Partition $win -FileSystem NTFS -AllocationUnitSize 4096 -NewFileSystemLabel 'Windows' -Confirm:$false | Out-Null",
        $"$win | Set-Partition -NewDriveLetter {WindowsLetter}"]);

    /// <summary>Checks the finished VHDX, re-attached read-only, still holds the Windows partition.</summary>
    internal static string VerifyReadOnlyScript(int diskNumber) => string.Join("\n", [
        .. FindDisk(diskNumber, readOnly: true),
        "if (-not $disk.IsReadOnly) { throw 'The virtual disk is not attached read-only' }",
        $"$p = Get-Partition -DiskNumber $disk.Number | Where-Object {{ $_.Offset -eq {N(PartitionMap.WindowsOffset)} }}",
        $"if (-not $p -or $p.Size -ne {N(PartitionMap.WindowsBytes)}) {{ throw 'The Windows partition is missing from the image' }}"]);

    public async Task<BuiltImage> BuildAsync(string vhdxPath, string installImage, int index,
        IReadOnlyList<DriverPackage> drivers, SlimProfile profile, UnattendOptions unattend, string outputDir,
        IProgress<string>? log = null, CancellationToken ct = default)
    {
        if (drivers.Count == 0)
        {
            throw new InvalidOperationException("No phone drivers were found to inject.");
        }
        var esp = $"{EspLetter}:";
        var winRoot = $"{WindowsLetter}:\\";
        Directory.CreateDirectory(outputDir);
        var windowsImage = Path.Combine(outputDir, "windows.img");
        var espOut = Path.Combine(outputDir, "esp");

        string loaderSha, kernelSha;
        log?.Report("Creating a virtual disk that mirrors the phone's storage...");
        // A run that was cancelled or crashed under an older installer (Mount-VHD attachments
        // outlive the process) can leave the file attached and locked; clear it first.
        _virtualDisks.DetachAndDelete(vhdxPath);
        var writable = _virtualDisks.CreateAndAttach(vhdxPath, PartitionMap.DiskBytes, BlockBytes, SectorBytes, SectorBytes);
        try
        {
            await RunScript(CreateScript(writable.DiskNumber), ct).ConfigureAwait(false);
            await _imageBuilder.BuildAsync(winRoot, installImage, index, drivers, profile, log, ct).ConfigureAwait(false);

            log?.Report("Writing the OOBE answer file...");
            var unattendPath = Path.Combine(winRoot, UnattendXml.RelativePath);
            Directory.CreateDirectory(Path.GetDirectoryName(unattendPath)!);
            await File.WriteAllBytesAsync(unattendPath, UnattendXml.BuildBytes(unattend), ct).ConfigureAwait(false);

            log?.Report("Writing UEFI boot files with bcdboot...");
            await WriteBootFilesAsync($"{winRoot}Windows", esp, ct).ConfigureAwait(false);
            await _boot.ConfigureForPhoneAsync(Path.Combine(esp + "\\", @"EFI\Microsoft\Boot\BCD"), log, ct).ConfigureAwait(false);
            loaderSha = await Sha256Async(Path.Combine(winRoot, @"Windows\System32\winload.efi"), ct).ConfigureAwait(false);
            kernelSha = await Sha256Async(Path.Combine(winRoot, @"Windows\System32\ntoskrnl.exe"), ct).ConfigureAwait(false);

            log?.Report("Copying the EFI boot files...");
            if (Directory.Exists(espOut))
            {
                Directory.Delete(espOut, recursive: true);
            }
            CopyTree(Path.Combine(esp + "\\", "EFI"), Path.Combine(espOut, "EFI"));
        }
        finally
        {
            log?.Report("Detaching the virtual disk...");
            writable.Dispose();
        }

        log?.Report("Exporting the Windows volume from a read-only attach (about 53 GB)...");
        using var readOnly = _virtualDisks.AttachReadOnly(vhdxPath);
        var disk = readOnly.DiskNumber;
        await RunScript(VerifyReadOnlyScript(disk), ct).ConfigureAwait(false);
        // Declared after readOnly, so it is closed before the disk is detached.
        using var disposable = _diskFactory(disk) as IDisposable;
        var source = (IRawDiskSource)disposable!;
        var (bytes, sha) = await _exporter.ExportAsync(source,
            new PartitionExtent(PartitionMap.WindowsOffset, PartitionMap.WindowsBytes), windowsImage, log, ct).ConfigureAwait(false);
        log?.Report("Windows image ready.");
        return new BuiltImage(windowsImage, bytes, sha, espOut, loaderSha, kernelSha);
    }

    /// <summary>
    /// Runs bcdboot for the phone, not for this PC. Current bcdboot picks the 2023-signed
    /// "EX" boot manager when this PC has Secure Boot on, and fails for images that predate
    /// it (22621.2428 has no <c>Boot\EFI_EX</c>). <c>/offline</c> makes it use the image's own
    /// boot manager regardless of this PC; older bcdboot lacks the switch, so retry without it.
    /// </summary>
    internal async Task WriteBootFilesAsync(string windowsDir, string esp, CancellationToken ct)
    {
        string[] args = [windowsDir, "/s", esp, "/f", "UEFI"];
        var r = await _runner.RunAsync(_bcdboot, [.. args, "/offline"], BootTimeout, ct).ConfigureAwait(false);
        if (r.Succeeded)
        {
            return;
        }
        var fallback = await _runner.RunAsync(_bcdboot, args, BootTimeout, ct).ConfigureAwait(false);
        if (!fallback.Succeeded)
        {
            throw new InvalidOperationException(
                $"bcdboot failed: {(fallback.StdErr + fallback.StdOut).Trim()} (with /offline: {(r.StdErr + r.StdOut).Trim()})");
        }
    }

    private static async Task<string> Sha256Async(string file, CancellationToken ct)
    {
        await using var s = File.OpenRead(file);
        return Convert.ToHexString(await System.Security.Cryptography.SHA256.HashDataAsync(s, ct).ConfigureAwait(false)).ToLowerInvariant();
    }

    internal static void CopyTree(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        foreach (var dir in Directory.EnumerateDirectories(source, "*", SearchOption.AllDirectories))
        {
            Directory.CreateDirectory(dir.Replace(source, destination, StringComparison.Ordinal));
        }
        foreach (var file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
        {
            File.Copy(file, file.Replace(source, destination, StringComparison.Ordinal), overwrite: true);
        }
    }

    private async Task<string> RunScript(string script, CancellationToken ct)
    {
        var file = Path.Combine(Path.GetTempPath(), $"s9woa-vhdx-{Guid.NewGuid():N}.ps1");
        await File.WriteAllTextAsync(file, script, ct).ConfigureAwait(false);
        try
        {
            var r = await _runner.RunAsync(_powershell,
                ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", file], DiskTimeout, ct).ConfigureAwait(false);
            if (!r.Succeeded)
            {
                throw new InvalidOperationException($"Virtual disk step failed: {(r.StdErr + r.StdOut).Trim()}");
            }
            return r.StdOut;
        }
        finally
        {
            try { File.Delete(file); } catch (IOException) { }
        }
    }
}
