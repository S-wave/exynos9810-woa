// SPDX-License-Identifier: BSD-2-Clause-Patent
using S9Woa.Installer.Core.Image;
using S9Woa.Installer.Core.Processes;

namespace S9Woa.Installer.Core.Tests;

public class ImageBuilderTests
{
    private sealed class RecordingRunner : IProcessRunner
    {
        public List<string> Calls { get; } = [];
        public Func<IReadOnlyList<string>, string>? StdOut { get; init; }
        public Task<ProcessResult> RunAsync(string fileName, IReadOnlyList<string> arguments, TimeSpan timeout,
            CancellationToken cancellationToken = default)
        {
            Calls.Add($"{Path.GetFileName(fileName)} {string.Join(' ', arguments)}");
            return Task.FromResult(new ProcessResult(0, StdOut?.Invoke(arguments) ?? "", ""));
        }
    }

    [Fact]
    public void ParsesEditionsAndChooses()
    {
        const string output =
            "Index : 1\r\nName : Windows 11 Home\r\nDescription : Windows 11 Home\r\nSize : 1\r\n\r\n" +
            "Index : 6\r\nName : Windows 11 Pro\r\nDescription : Windows 11 Pro\r\nSize : 1\r\n";
        var editions = WindowsMedia.ParseImageInfo(output);
        Assert.Equal(2, editions.Count);
        Assert.Equal(6, editions[1].Index);
        Assert.Equal("Windows 11 Pro", editions[1].Name);
        Assert.Equal(6, WindowsMedia.ChooseEdition(editions).Index);
        Assert.Equal(1, WindowsMedia.ChooseEdition([editions[0]]).Index);
        Assert.Throws<InvalidOperationException>(() => WindowsMedia.ChooseEdition([]));

        // The validated IoT Enterprise 23H2 media: Enterprise at 1, IoT Enterprise at 2.
        var iot = WindowsMedia.ParseImageInfo(
            "Index : 1\r\nName : Windows 11 Enterprise\r\nDescription : x\r\n\r\n" +
            "Index : 2\r\nName : Windows 11 IoT Enterprise\r\nDescription : x\r\n");
        Assert.Equal(2, WindowsMedia.ChooseEdition(iot).Index);
    }

    [Fact]
    public void FindsInstallImage()
    {
        var dir = Directory.CreateTempSubdirectory("s9woa-media").FullName;
        try
        {
            Directory.CreateDirectory(Path.Combine(dir, "sources"));
            var wim = Path.Combine(dir, "sources", "install.wim");
            File.WriteAllText(wim, "");
            Assert.Equal(wim, WindowsMedia.FindInstallImage(dir));
            Assert.Equal(wim, WindowsMedia.FindInstallImage(wim));
            Assert.Null(WindowsMedia.FindInstallImage(Path.Combine(dir, "nope.wim")));
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public async Task OpensAnIsoReadOnlyAndFindsItsInstallImage()
    {
        var script = WindowsMedia.MountIsoScript(@"C:\iso\it's.iso");
        Assert.Contains(@"-ImagePath 'C:\iso\it''s.iso'", script, StringComparison.Ordinal);
        Assert.Contains("-Access ReadOnly -StorageType ISO", script, StringComparison.Ordinal);
        Assert.Contains("if (-not $img.Attached)", script, StringComparison.Ordinal);

        // No drive letter: a clear error instead of "no install.wim".
        var runner = new RecordingRunner { StdOut = _ => "\r\n" };
        await Assert.ThrowsAsync<InvalidOperationException>(() => new WindowsMedia(runner, @"C:\sys").ResolveInstallImageAsync(@"C:\w.iso"));
        Assert.Contains(runner.Calls, c => c.StartsWith("powershell.exe -NoProfile", StringComparison.Ordinal));

        // A .wim is used as is, without PowerShell.
        var wimRunner = new RecordingRunner();
        Assert.Null(await new WindowsMedia(wimRunner, @"C:\sys").ResolveInstallImageAsync(@"C:\missing\install.wim"));
        Assert.Empty(wimRunner.Calls);
    }

    [Fact]
    public void DiscoversDriverFolders()
    {
        var dir = Directory.CreateTempSubdirectory("s9woa-drv").FullName;
        try
        {
            Directory.CreateDirectory(Path.Combine(dir, "Ufs"));
            Directory.CreateDirectory(Path.Combine(dir, "Touch"));
            File.WriteAllText(Path.Combine(dir, "Ufs", "Exynos9810Ufs.inf"), "");
            File.WriteAllText(Path.Combine(dir, "Touch", "S6SY761Touch.inf"), "");
            var found = ImageBuilder.DiscoverDrivers(dir);
            Assert.Equal(2, found.Count);
            Assert.Contains(found, p => p.Name == "Ufs");
            Assert.Empty(ImageBuilder.DiscoverDrivers(Path.Combine(dir, "missing")));
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public async Task BuildAppliesInjectsThenSlims()
    {
        var apply = Directory.CreateTempSubdirectory("s9woa-apply").FullName;
        try
        {
            var runner = new RecordingRunner();
            var drivers = new[] { new DriverPackage("Ufs", @"C:\d\Ufs"), new DriverPackage("Touch", @"C:\d\Touch") };
            await new ImageBuilder(runner, @"C:\sys").BuildAsync(apply, @"C:\m\install.wim", 6, drivers, SlimProfile.None);

            var applyIdx = runner.Calls.FindIndex(c => c.Contains("/Apply-Image", StringComparison.Ordinal));
            var ufsIdx = runner.Calls.FindIndex(c => c.Contains("/Add-Driver", StringComparison.Ordinal) && c.Contains(@"C:\d\Ufs", StringComparison.Ordinal));
            Assert.True(applyIdx >= 0 && ufsIdx > applyIdx);
            Assert.Contains(runner.Calls, c => c.Contains("/Add-Driver", StringComparison.Ordinal) && c.Contains("/ForceUnsigned", StringComparison.Ordinal));
            Assert.Contains(runner.Calls, c => c.Contains(@"/ImageFile:C:\m\install.wim", StringComparison.Ordinal) && c.Contains("/Index:6", StringComparison.Ordinal));
        }
        finally
        {
            Directory.Delete(apply, recursive: true);
        }
    }

    [Fact]
    public void VhdxScriptMirrorsThePhoneDisk()
    {
        var builder = new VhdxImageBuilder(new RecordingRunner(), @"C:\sys") { EspLetter = 'S', WindowsLetter = 'W' };
        var script = builder.CreateScript(7);
        // No Hyper-V cmdlets: the disk is created and attached through virtdisk.dll.
        Assert.DoesNotContain("-VHD", script, StringComparison.Ordinal);
        Assert.Contains("Get-Disk -Number 7", script, StringComparison.Ordinal);
        Assert.Contains("Set-Disk -Number 7 -IsOffline $false", script, StringComparison.Ordinal);
        Assert.Contains("Set-Disk -Number 7 -IsReadOnly $false", script, StringComparison.Ordinal);
        Assert.Contains("LogicalSectorSize -ne 4096", script, StringComparison.Ordinal);
        Assert.Contains("{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}", script, StringComparison.Ordinal); // ESP
        Assert.Contains("{e3c9e316-0b5c-4db8-817d-f92df00215ae}", script, StringComparison.Ordinal); // MSR
        Assert.Contains("-Offset 6951534592 -Size 57004785664 -Alignment 4096", script, StringComparison.Ordinal); // = USERDATA
        Assert.Contains("-FileSystem NTFS -AllocationUnitSize 4096", script, StringComparison.Ordinal);
        Assert.Contains("-NewDriveLetter S", script, StringComparison.Ordinal);
        Assert.Contains("-NewDriveLetter W", script, StringComparison.Ordinal);
        var verify = VhdxImageBuilder.VerifyReadOnlyScript(9);
        Assert.DoesNotContain("-VHD", verify, StringComparison.Ordinal);
        Assert.Contains("Get-Partition -DiskNumber $disk.Number", verify, StringComparison.Ordinal);
        Assert.Contains("6951534592", verify, StringComparison.Ordinal);
        Assert.Contains("-not $disk.IsReadOnly", verify, StringComparison.Ordinal);
        Assert.DoesNotContain("-IsReadOnly $false", verify, StringComparison.Ordinal); // never made writable
        Assert.DoesNotContain('C', VhdxImageBuilder.FreeDriveLetters());
    }

    private sealed class FakeVirtualDisks : IVirtualDiskHost
    {
        public List<string> Events { get; } = [];

        private sealed class Disk(FakeVirtualDisks owner, int number, string kind) : IAttachedVirtualDisk
        {
            public int DiskNumber => number;
            public void Dispose() => owner.Events.Add($"detach {kind}");
        }

        public IAttachedVirtualDisk CreateAndAttach(string path, long sizeBytes, uint blockSizeBytes, uint logicalSectorBytes, uint physicalSectorBytes)
        {
            Events.Add($"create {path} {sizeBytes} {blockSizeBytes} {logicalSectorBytes}/{physicalSectorBytes}");
            return new Disk(this, 7, "rw");
        }

        public IAttachedVirtualDisk AttachReadOnly(string path)
        {
            Events.Add($"attach-ro {path}");
            return new Disk(this, 9, "ro");
        }

        public void DetachAndDelete(string path) => Events.Add($"clear {path}");
    }

    [Fact]
    public async Task AFailedBuildStillDetachesTheVirtualDisk()
    {
        var disks = new FakeVirtualDisks();
        // DISM fails right after the disk is laid out.
        var runner = new FailingRunner(c => c.Contains("/Apply-Image", StringComparison.Ordinal));
        var builder = new VhdxImageBuilder(runner, @"C:\sys", virtualDisks: disks) { EspLetter = 'S', WindowsLetter = 'W' };
        var outDir = Directory.CreateTempSubdirectory("s9woa-out").FullName;
        try
        {
            await Assert.ThrowsAnyAsync<Exception>(() => builder.BuildAsync(@"C:\img\s9.vhdx", @"C:\m\install.wim", 6,
                [new DriverPackage("Ufs", @"C:\d\Ufs")], SlimProfile.None, new UnattendOptions(), outDir));
        }
        finally
        {
            Directory.Delete(outDir, recursive: true);
        }

        Assert.Equal(
            [@"clear C:\img\s9.vhdx", @"create C:\img\s9.vhdx 63963136000 33554432 4096/4096", "detach rw"],
            disks.Events);
        // The layout script ran against the disk number the attach reported.
        Assert.Contains(runner.Scripts, s => s.Contains("Get-Disk -Number 7", StringComparison.Ordinal));
    }

    /// <summary>Fails the first call matching a predicate; records the PowerShell scripts it is given.</summary>
    private sealed class FailingRunner(Func<string, bool> fail) : IProcessRunner
    {
        public List<string> Scripts { get; } = [];

        public Task<ProcessResult> RunAsync(string fileName, IReadOnlyList<string> arguments, TimeSpan timeout,
            CancellationToken cancellationToken = default)
        {
            var file = arguments.SkipWhile(a => a != "-File").Skip(1).FirstOrDefault();
            if (file is not null && File.Exists(file))
            {
                Scripts.Add(File.ReadAllText(file));
            }
            var line = string.Join(' ', arguments);
            return Task.FromResult(fail(line) ? new ProcessResult(1, "", "simulated failure") : new ProcessResult(0, "", ""));
        }
    }

    [Theory]
    [InlineData(@"\\.\PhysicalDrive3", 3)]
    [InlineData(@"\\.\PHYSICALDRIVE12", 12)]
    public void ReadsTheDiskNumberFromThePhysicalPath(string path, int number) =>
        Assert.Equal(number, VirtualDiskHost.ParseDiskNumber(path));

    [Fact]
    public void RefusesAPhysicalPathItDoesNotUnderstand() =>
        Assert.Throws<InvalidOperationException>(() => VirtualDiskHost.ParseDiskNumber(@"\\?\Volume{1234}"));

    private sealed class FailingOfflineRunner(bool offlineSupported) : IProcessRunner
    {
        public List<string> Calls { get; } = [];
        public Task<ProcessResult> RunAsync(string fileName, IReadOnlyList<string> arguments, TimeSpan timeout,
            CancellationToken cancellationToken = default)
        {
            Calls.Add(string.Join(' ', arguments));
            var ok = offlineSupported || !arguments.Contains("/offline");
            return Task.FromResult(new ProcessResult(ok ? 0 : 1, ok ? "Boot files successfully created." : "The parameter is incorrect.", ""));
        }
    }

    [Theory]
    [InlineData(true, 1)]
    [InlineData(false, 2)]
    public async Task BcdbootTargetsThePhoneNotThisPc(bool offlineSupported, int calls)
    {
        var runner = new FailingOfflineRunner(offlineSupported);
        var builder = new VhdxImageBuilder(runner, @"C:\sys");
        await builder.WriteBootFilesAsync(@"W:\Windows", "S:", CancellationToken.None);
        Assert.Equal(calls, runner.Calls.Count);
        Assert.Equal(@"W:\Windows /s S: /f UEFI /offline", runner.Calls[0]);
        if (calls == 2)
        {
            Assert.Equal(@"W:\Windows /s S: /f UEFI", runner.Calls[1]);
        }
    }
}
