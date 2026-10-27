// SPDX-License-Identifier: BSD-2-Clause-Patent
using S9Woa.Installer.Core.Deploy;
using S9Woa.Installer.Core.Processes;

namespace S9Woa.Installer.Core.Tests;

public class StorageVariantTests
{
    // A 256 GB unit: 62,468,096 blocks of 4096, with the same 1,664-block tail and backup GPT
    // behind USERDATA as the 64 GB unit (the exact tail does not matter to the checks).
    private const long Disk256 = 62_468_096L * 4096;
    private const long Userdata256 = Disk256 - PartitionMap.WindowsOffset - 6_836_224;
    private const long Disk128 = 31_234_048L * 4096;

    [Theory]
    [InlineData(PartitionMap.DiskBytes, "64 GB")]
    [InlineData(Disk128, "128 GB")]
    [InlineData(Disk256, "256 GB")]
    [InlineData(PartitionMap.DiskBytes + 4096, null)]   // the 64 GB unit is pinned exactly
    [InlineData(PartitionMap.DiskBytes + 1, null)]      // not whole 4 KB blocks
    [InlineData(40_000_000L * 4096, null)]              // between bands
    [InlineData(125_000_000L * 4096, null)]             // a 512 GB unit was never sold
    public void ClassifiesTheThreeShippedUnits(long diskBytes, string? variant) =>
        Assert.Equal(variant, PartitionMap.StorageVariant(diskBytes));

    [Fact]
    public void TheValidatedSixtyFourGigabyteLayoutFits() =>
        Assert.Null(PartitionMap.CheckWindowsTarget(PartitionMap.WindowsOffset, PartitionMap.WindowsBytes, PartitionMap.DiskBytes));

    [Fact]
    public void ALargerUserdataAtTheSameOffsetFits()
    {
        Assert.Null(PartitionMap.CheckWindowsTarget(PartitionMap.WindowsOffset, Userdata256, Disk256));
        Assert.Null(PartitionMap.CheckWindowsTarget(PartitionMap.WindowsOffset, PartitionMap.WindowsBytes, Disk128));
    }

    [Fact]
    public void AnythingElseIsRefusedBeforeAWrite()
    {
        // 64 GB: USERDATA must be exactly the validated size.
        Assert.NotNull(PartitionMap.CheckWindowsTarget(PartitionMap.WindowsOffset, PartitionMap.WindowsBytes - 4096, PartitionMap.DiskBytes));
        // Moved start: the image's NTFS geometry would not match.
        Assert.NotNull(PartitionMap.CheckWindowsTarget(PartitionMap.WindowsOffset + 4096, Userdata256 - 4096, Disk256));
        // Too small for the Windows volume.
        Assert.NotNull(PartitionMap.CheckWindowsTarget(PartitionMap.WindowsOffset, PartitionMap.WindowsBytes - 4096, Disk256));
        // Runs past the end of the disk.
        Assert.NotNull(PartitionMap.CheckWindowsTarget(PartitionMap.WindowsOffset, Disk256, Disk256));
        // Unknown unit.
        Assert.NotNull(PartitionMap.CheckWindowsTarget(PartitionMap.WindowsOffset, Userdata256, 40_000_000L * 4096));
    }

    private sealed class SysfsRunner(string stdout) : IProcessRunner
    {
        public string? Command { get; private set; }

        public Task<ProcessResult> RunAsync(string fileName, IReadOnlyList<string> arguments, TimeSpan timeout, CancellationToken cancellationToken = default)
        {
            Command = arguments[^1];
            return Task.FromResult(new ProcessResult(0, stdout, ""));
        }
    }

    [Fact]
    public async Task ReadsThePartitionExtentFromSysfsInSectors()
    {
        var runner = new SysfsRunner($"{PartitionMap.WindowsOffset / 512}\n{Userdata256 / 512}\n{Disk256 / 512}\n");
        var twrp = new TwrpClient("adb", "serial", runner);

        var (start, size, disk) = await twrp.PartitionExtentAsync("USERDATA");

        Assert.Equal((PartitionMap.WindowsOffset, Userdata256, Disk256), (start, size, disk));
        Assert.Contains("readlink -f /dev/block/by-name/USERDATA", runner.Command, StringComparison.Ordinal);
        Assert.Contains("/sys/class/block/$n/../size", runner.Command, StringComparison.Ordinal);
    }

    [Fact]
    public async Task AnUnreadableExtentIsAnError()
    {
        var twrp = new TwrpClient("adb", "serial", new SysfsRunner("cat: /sys/class/block/sda25/start: No such file\n"));
        await Assert.ThrowsAsync<InvalidOperationException>(() => twrp.PartitionExtentAsync("USERDATA"));
    }
}
