// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.Text.RegularExpressions;
using S9Woa.Installer.Core.Processes;

namespace S9Woa.Installer.Core.Deploy;

/// <summary>
/// Talks to a device booted into TWRP over ADB: enumerate partitions by name,
/// read their size, dd them to and from TWRP's RAM, hash them, and move files
/// with adb push/pull. Binary payloads always travel as files staged in /tmp,
/// never as captured stdout, so nothing is corrupted by text decoding. No SD
/// card is needed.
/// </summary>
public sealed partial class TwrpClient
{
    private static readonly TimeSpan Quick = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan Long = TimeSpan.FromHours(1);

    /// <summary>A hung shell is the ntfs-3g deadlock symptom; don't wait the full <see cref="Quick"/> to notice.</summary>
    private static readonly TimeSpan DeadlockProbe = TimeSpan.FromSeconds(12);
    private readonly string _adb;
    private readonly string _serial;
    private readonly IProcessRunner _runner;

    public TwrpClient(string adbPath, string serial, IProcessRunner runner)
    {
        _adb = adbPath;
        _serial = serial;
        _runner = runner;
    }


    /// <summary>TWRP's tmpfs: backups and image chunks are staged here, in RAM.</summary>
    public string RamStagingDir { get; init; } = "/tmp/s9woa";

    public const string ByName = "/dev/block/by-name";

    [GeneratedRegex(@"(?<name>\S+)\s+->\s+(?<node>\S+)\s*$")]
    private static partial Regex LinkRegex();

    internal static IReadOnlyDictionary<string, string> ParsePartitionLinks(string lsOutput)
    {
        var map = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var raw in lsOutput.Split('\n'))
        {
            var m = LinkRegex().Match(raw.TrimEnd('\r'));
            if (m.Success)
            {
                var node = m.Groups["node"].Value;
                map[m.Groups["name"].Value] = node.StartsWith('/') ? node : $"/dev/block/{Path.GetFileName(node)}";
            }
        }
        return map;
    }

    internal static string? ParseSha256(string sumOutput) =>
        sumOutput.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries) is { Length: >= 1 } parts
            && parts[0].Length == 64 ? parts[0].ToLowerInvariant() : null;

    private async Task<ProcessResult> AdbAsync(IReadOnlyList<string> args, TimeSpan timeout, CancellationToken ct)
    {
        string[] full = ["-s", _serial, .. args];
        for (var attempt = 0; ; attempt++)
        {
            var r = await _runner.RunAsync(_adb, full, timeout, ct).ConfigureAwait(false);
            if (r.Succeeded || attempt >= AdbRetryDelays.Count || !IsTransientAdbFailure(r, args[0]))
            {
                return r;
            }
            // The adb server on this PC stopped answering for a moment (seen mid-copy: "cannot
            // connect to daemon ... (10060)" while the same server kept running), or the phone
            // dropped off USB briefly. Wait and send the same command again.
            await Task.Delay(AdbRetryDelays[attempt], ct).ConfigureAwait(false);
        }
    }

    /// <summary>Waits between attempts of an adb command that failed for a transient reason (about 80 s in all).</summary>
    internal IReadOnlyList<TimeSpan> AdbRetryDelays { get; init; } =
        [TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(5), TimeSpan.FromSeconds(8), TimeSpan.FromSeconds(12),
         TimeSpan.FromSeconds(18), TimeSpan.FromSeconds(35)];

    /// <summary>adb client errors that mean the command never reached the phone, so any command can be sent again.</summary>
    private static readonly string[] NotDelivered =
    [
        "cannot connect to daemon", "daemon not running", "daemon still not running", "failed to get feature set",
        "device offline", "no devices/emulators found", "' not found", "failed to start daemon",
    ];

    /// <summary>Transport errors in the middle of a file transfer; re-sending a push or pull is harmless.</summary>
    private static readonly string[] TransferDropped =
    [
        "protocol fault", "error: closed", "connection reset", "remote couldn't create file: Connection", "failed to read copy response",
    ];

    internal static bool IsTransientAdbFailure(ProcessResult result, string verb)
    {
        // Only adb's own messages count: a remote command's output never starts with "adb: error" / "error:".
        var adbText = string.Join('\n', (result.StdErr + "\n" + result.StdOut).Split('\n')
            .Select(l => l.Trim())
            .Where(l => l.StartsWith("adb: ", StringComparison.Ordinal) || l.StartsWith("error: ", StringComparison.Ordinal)
                || l.StartsWith("* daemon", StringComparison.Ordinal)));
        if (adbText.Length == 0)
        {
            return false;
        }
        if (NotDelivered.Any(p => adbText.Contains(p, StringComparison.OrdinalIgnoreCase)))
        {
            return true;
        }
        return verb is "push" or "pull" && TransferDropped.Any(p => adbText.Contains(p, StringComparison.OrdinalIgnoreCase));
    }

    private async Task<string> ShellCheckedAsync(string command, TimeSpan timeout, CancellationToken ct)
    {
        var r = await AdbAsync(["shell", WithQueuedStatus(command)], timeout, ct).ConfigureAwait(false);
        if (!r.Succeeded)
        {
            throw new InvalidOperationException($"TWRP command failed: {command}\n{(r.StdErr + r.StdOut).Trim()}");
        }
        return r.StdOut;
    }

    // The next install status for the phone, written by the next checked shell command.
    private WinReStatus? _queuedStatus;

    /// <summary>
    /// Queues an install status for the recovery's "Installing Windows" screen. It costs no
    /// round trip of its own: the next checked shell command carries it (see
    /// <see cref="StatusSuffix"/>), so the installer can refresh the phone after every chunk
    /// for free. A later call replaces a status that has not gone out yet.
    /// </summary>
    public void QueueInstallStatus(WinReStatus status) => Interlocked.Exchange(ref _queuedStatus, status);

    /// <summary>Drops a queued status that has not been written yet.</summary>
    public void DiscardQueuedInstallStatus() => Interlocked.Exchange(ref _queuedStatus, null);

    private string WithQueuedStatus(string command) =>
        Interlocked.Exchange(ref _queuedStatus, null) is { } status
            ? command.TrimEnd().TrimEnd(';') + "; " + StatusSuffix(status)
            : command;

    /// <summary>
    /// Shell to append after a command so it also (re)writes the status file, atomically
    /// (write + mv, so the watcher never reads half a file), without changing what the
    /// command returns: its exit code is kept and the write prints nothing. base64 keeps
    /// every character of the status away from the shell.
    /// </summary>
    internal static string StatusSuffix(WinReStatus status)
    {
        var b64 = Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(status.ToFileContents()));
        return $"s9r=$?; {{ mkdir -p {WinReStatus.DeviceDir} && echo {b64} | base64 -d > {WinReStatus.DevicePath}.tmp"
            + $" && mv -f {WinReStatus.DevicePath}.tmp {WinReStatus.DevicePath}; }} >/dev/null 2>&1; exit $s9r";
    }

    public async Task<IReadOnlyDictionary<string, string>> ListPartitionsAsync(CancellationToken ct = default) =>
        ParsePartitionLinks(await ShellCheckedAsync($"ls -l {ByName}", Quick, ct).ConfigureAwait(false));

    /// <summary>
    /// Returns the device's own spelling of a by-name link (this phone uses upper case, e.g.
    /// <c>USERDATA</c>), matched case-insensitively. Throws if the partition does not exist.
    /// </summary>
    public async Task<string> ResolvePartitionNameAsync(string name, CancellationToken ct = default)
    {
        var links = await ListPartitionsAsync(ct).ConfigureAwait(false);
        return links.Keys.FirstOrDefault(k => string.Equals(k, name, StringComparison.OrdinalIgnoreCase))
            ?? throw new InvalidOperationException($"The phone has no partition named {name}.");
    }

    /// <summary>Seconds since TWRP booted.</summary>
    public async Task<double> UptimeSecondsAsync(CancellationToken ct = default)
    {
        var text = (await ShellCheckedAsync("cut -d' ' -f1 /proc/uptime", Quick, ct).ConfigureAwait(false)).Trim();
        return double.TryParse(text, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var s) ? s : 0;
    }

    /// <summary>First available FAT formatter in TWRP, or null.</summary>
    public async Task<string?> FindFatFormatterAsync(CancellationToken ct = default)
    {
        var r = await AdbAsync(["shell", "for t in mkfs.fat mkfs.vfat mkdosfs newfs_msdos; do command -v $t && break; done"], Quick, ct).ConfigureAwait(false);
        var path = r.StdOut.Trim().Split('\n').FirstOrDefault()?.Trim();
        return string.IsNullOrEmpty(path) ? null : path;
    }

    /// <summary>True when the partition starts with a FAT32 boot sector (type string "FAT32" at 0x52).</summary>
    public async Task<bool> IsFat32Async(string name, CancellationToken ct = default)
    {
        var r = await AdbAsync(["shell", $"dd if={ByName}/{name} bs=1 skip=82 count=5 2>/dev/null"], Quick, ct).ConfigureAwait(false);
        return r.StdOut.StartsWith("FAT32", StringComparison.Ordinal);
    }

    /// <summary>
    /// Where a partition sits: its start and size and the size of the UFS unit holding it, all in
    /// bytes. sysfs reports 512-byte units whatever the logical block size.
    /// </summary>
    public async Task<(long StartBytes, long SizeBytes, long DiskBytes)> PartitionExtentAsync(string name, CancellationToken ct = default)
    {
        var text = await ShellCheckedAsync(
            $"n=$(basename $(readlink -f {ByName}/{name})) && cat /sys/class/block/$n/start /sys/class/block/$n/size /sys/class/block/$n/../size",
            Quick, ct).ConfigureAwait(false);
        var v = text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        if (v.Length != 3 || !long.TryParse(v[0], out var start) || !long.TryParse(v[1], out var size) || !long.TryParse(v[2], out var disk))
        {
            throw new InvalidOperationException($"Could not read where partition {name} sits: {text.Trim()}");
        }
        return (start * 512, size * 512, disk * 512);
    }

    public async Task<long> PartitionSizeAsync(string name, CancellationToken ct = default)
    {
        var text = (await ShellCheckedAsync($"blockdev --getsize64 {ByName}/{name}", Quick, ct).ConfigureAwait(false)).Trim();
        return long.TryParse(text, out var n) ? n
            : throw new InvalidOperationException($"Could not read the size of partition {name}: {text}");
    }

    /// <summary>SHA-256 of the first <paramref name="bytes"/> of a device path (whole thing if null).</summary>
    public async Task<string> Sha256Async(string devicePath, long? bytes = null, CancellationToken ct = default)
    {
        var cmd = bytes is { } n
            ? $"head -c {n} {devicePath} | sha256sum"
            : $"sha256sum {devicePath}";
        var sum = ParseSha256(await ShellCheckedAsync(cmd, Long, ct).ConfigureAwait(false));
        return sum ?? throw new InvalidOperationException($"Could not hash {devicePath}.");
    }


    public Task DdAsync(string source, string destination, CancellationToken ct = default) =>
        ShellCheckedAsync($"dd if={source} of={destination} bs=4194304 conv=fsync", Long, ct);

    /// <summary>Writes a 1-MiB-aligned device file into a partition at <paramref name="seekMiB"/>.</summary>
    public Task WritePartitionWindowAsync(string deviceFile, string name, long seekMiB, long countMiB, CancellationToken ct = default) =>
        ShellCheckedAsync($"dd if={deviceFile} of={ByName}/{name} bs=1048576 seek={seekMiB} count={countMiB} conv=notrunc,fsync", Long, ct);

    /// <summary>Fills a 1-MiB-aligned window of a partition with zeros on the phone (no USB transfer).</summary>
    public Task ZeroPartitionWindowAsync(string name, long seekMiB, long countMiB, CancellationToken ct = default) =>
        ShellCheckedAsync($"dd if=/dev/zero of={ByName}/{name} bs=1048576 seek={seekMiB} count={countMiB} conv=notrunc,fsync", Long, ct);

    /// <summary>SHA-256 of a 1-MiB-aligned window read back from a partition.</summary>
    public async Task<string> HashPartitionWindowAsync(string name, long skipMiB, long countMiB, CancellationToken ct = default)
    {
        var cmd = $"dd if={ByName}/{name} bs=1048576 skip={skipMiB} count={countMiB} 2>/dev/null | sha256sum";
        return ParseSha256(await ShellCheckedAsync(cmd, Long, ct).ConfigureAwait(false))
            ?? throw new InvalidOperationException($"Could not hash a window of {name}.");
    }

    public async Task PullAsync(string devicePath, string hostPath, CancellationToken ct = default)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(hostPath)!);
        var r = await AdbAsync(["pull", devicePath, hostPath], Long, ct).ConfigureAwait(false);
        if (!r.Succeeded)
        {
            throw new InvalidOperationException($"adb pull {devicePath} failed: {(r.StdErr + r.StdOut).Trim()}");
        }
    }

    public async Task PushAsync(string hostPath, string devicePath, CancellationToken ct = default)
    {
        var r = await AdbAsync(["push", hostPath, devicePath], Long, ct).ConfigureAwait(false);
        if (!r.Succeeded)
        {
            throw new InvalidOperationException($"adb push {Path.GetFileName(hostPath)} failed: {(r.StdErr + r.StdOut).Trim()}");
        }
    }

    public Task RemoveAsync(string devicePath, CancellationToken ct = default) =>
        ShellCheckedAsync($"rm -f {devicePath}", Quick, ct);

    /// <summary>Runs an arbitrary checked shell command in TWRP and returns stdout.</summary>
    public Task<string> ShellAsync(string command, CancellationToken ct = default) =>
        ShellCheckedAsync(command, Long, ct);

    public Task MakeDirAsync(string devicePath, CancellationToken ct = default) =>
        ShellCheckedAsync($"mkdir -p {devicePath}", Quick, ct);

    /// <summary>Mounts a vfat partition node at a mountpoint (created first).</summary>
    public async Task MountVfatAsync(string node, string mountpoint, CancellationToken ct = default)
    {
        await MakeDirAsync(mountpoint, ct).ConfigureAwait(false);
        await ShellCheckedAsync($"mount -t vfat -o rw {node} {mountpoint}", Quick, ct).ConfigureAwait(false);
    }

    public Task UnmountAsync(string mountpoint, CancellationToken ct = default) =>
        ShellCheckedAsync($"umount {mountpoint}", Quick, ct);

    /// <summary>Recursively pushes a host directory's contents into a device directory.</summary>
    public async Task PushTreeAsync(string hostDir, string deviceDir, CancellationToken ct = default)
    {
        var r = await AdbAsync(["push", hostDir.TrimEnd('\\'), deviceDir], Long, ct).ConfigureAwait(false);
        if (!r.Succeeded)
        {
            throw new InvalidOperationException($"adb push {hostDir} failed: {(r.StdErr + r.StdOut).Trim()}");
        }
    }

    /// <summary>True when <paramref name="mountpoint"/> appears in /proc/mounts.</summary>
    public async Task<bool> IsMountedAsync(string mountpoint, CancellationToken ct = default)
    {
        var r = await AdbAsync(["shell", $"grep -q ' {mountpoint} ' /proc/mounts && echo yes || echo no"], Quick, ct).ConfigureAwait(false);
        return r.StdOut.Contains("yes", StringComparison.Ordinal);
    }

    /// <summary>
    /// Ensures adb shell answers, breaking the ntfs-3g FUSE self-deadlock first if it is hung.
    /// Returns true when the shell responds (possibly after the break). Cheap when the shell is
    /// already healthy; the baked-in on-phone watchdog usually breaks the deadlock before the host
    /// even notices, so this is mainly the fallback for a stock TWRP without it.
    /// </summary>
    public async Task<bool> EnsureResponsiveAsync(CancellationToken ct = default)
    {
        if (await ShellAnswersAsync(ct).ConfigureAwait(false))
        {
            return true;
        }
        if (await BreakNtfsDeadlockAsync(ct).ConfigureAwait(false))
        {
            await Task.Delay(TimeSpan.FromSeconds(3), ct).ConfigureAwait(false);
            return await ShellAnswersAsync(ct).ConfigureAwait(false);
        }
        return false;
    }

    private async Task<bool> ShellAnswersAsync(CancellationToken ct)
    {
        try
        {
            var r = await AdbAsync(["shell", "echo s9ok"], DeadlockProbe, ct).ConfigureAwait(false);
            return r.Succeeded && r.StdOut.Contains("s9ok", StringComparison.Ordinal);
        }
        catch (TimeoutException)
        {
            return false;
        }
    }

    /// <summary>
    /// Breaks the ntfs-3g FUSE self-deadlock from the host when adb shell is hung: it enumerates
    /// /proc over the adb sync service (which keeps answering when the shell does not), finds the
    /// wedged <c>mount.ntfs</c> daemon by its cmdline, pins its OOM score to the maximum and trips
    /// the OOM killer with sysrq 'f'. Killing the daemon aborts the FUSE connection so every waiter
    /// (the recovery included) unblocks. Returns true when it acted on a mount.ntfs process.
    /// </summary>
    public async Task<bool> BreakNtfsDeadlockAsync(CancellationToken ct = default)
    {
        try
        {
            var listing = await AdbAsync(["ls", "/proc"], Quick, ct).ConfigureAwait(false);
            if (!listing.Succeeded)
            {
                return false;
            }
            var acted = false;
            foreach (var pid in EnumerateProcPids(listing.StdOut))
            {
                var cmd = await PullTextAsync($"/proc/{pid}/cmdline", ct).ConfigureAwait(false);
                if (cmd is null)
                {
                    continue;
                }
                cmd = cmd.Replace('\0', ' ').Trim();
                if (!cmd.StartsWith("/sbin/mount.ntfs", StringComparison.Ordinal) && !cmd.Contains("ntfs-3g", StringComparison.Ordinal))
                {
                    continue;
                }
                await PushTextAsync("1000", $"/proc/{pid}/oom_score_adj", ct).ConfigureAwait(false);
                await PushTextAsync("f", "/proc/sysrq-trigger", ct).ConfigureAwait(false);
                acted = true;
            }
            return acted;
        }
        catch (TimeoutException)
        {
            return false;
        }
    }

    /// <summary>Numeric process ids from an <c>adb ls /proc</c> listing (mode, size, mtime, name).</summary>
    internal static IEnumerable<string> EnumerateProcPids(string lsOutput)
    {
        foreach (var line in lsOutput.Split('\n'))
        {
            var name = line.TrimEnd('\r').Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries).LastOrDefault();
            if (!string.IsNullOrEmpty(name) && name.All(char.IsAsciiDigit))
            {
                yield return name;
            }
        }
    }

    private async Task<string?> PullTextAsync(string devicePath, CancellationToken ct)
    {
        var local = Path.Combine(Path.GetTempPath(), $"s9woa-proc-{Guid.NewGuid():N}");
        try
        {
            var r = await AdbAsync(["pull", devicePath, local], Quick, ct).ConfigureAwait(false);
            return r.Succeeded && File.Exists(local) ? await File.ReadAllTextAsync(local, ct).ConfigureAwait(false) : null;
        }
        catch (IOException)
        {
            return null;
        }
        finally
        {
            try { File.Delete(local); } catch (IOException) { }
        }
    }

    private async Task PushTextAsync(string content, string devicePath, CancellationToken ct)
    {
        var local = Path.Combine(Path.GetTempPath(), $"s9woa-push-{Guid.NewGuid():N}");
        try
        {
            await File.WriteAllTextAsync(local, content, ct).ConfigureAwait(false);
            await AdbAsync(["push", local, devicePath], Quick, ct).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            // procfs writes over the sync service are best-effort; a timeout must not throw here.
        }
        finally
        {
            try { File.Delete(local); } catch (IOException) { }
        }
    }

    /// <summary>
    /// Writes the install-progress status file the baked-in <c>winre-statuswatch.sh</c> watches
    /// to raise the "Installing Windows" screen, right now (one round trip). Prefer
    /// <see cref="QueueInstallStatus"/>, which rides on the next shell command. Best-effort: a
    /// failure here never aborts the install (the watcher's dd-writer fallback still raises a
    /// generic screen).
    /// </summary>
    public async Task SetInstallStatusAsync(WinReStatus status, CancellationToken ct = default)
    {
        QueueInstallStatus(status);
        try
        {
            await ShellCheckedAsync("true", Quick, ct).ConfigureAwait(false);
        }
        catch (InvalidOperationException)
        {
            // The screen is a courtesy; never let it fail the write.
        }
    }

    /// <summary>Removes the install-progress status file so the "Installing Windows" screen clears.</summary>
    public async Task ClearInstallStatusAsync(CancellationToken ct = default)
    {
        DiscardQueuedInstallStatus();
        try
        {
            await ShellCheckedAsync($"rm -f {WinReStatus.DevicePath} 2>/dev/null; true", Quick, ct).ConfigureAwait(false);
        }
        catch (InvalidOperationException)
        {
        }
    }

    /// <summary>
    /// Neutralises a stale <c>/sdcard/TWRP/theme/ui.zip</c> for the CURRENT boot. TWRP loads that
    /// zip in preference to the theme baked into the recovery image, so a leftover from an earlier
    /// experiment could override the WinRE theme; the baked-in postrecoveryboot.sh only heals the
    /// next boot, so the installer deletes it here (internal storage is /data/media on this build,
    /// not /data/media/0). Best-effort.
    /// </summary>
    public async Task RemoveStaleThemeOverrideAsync(CancellationToken ct = default)
    {
        try
        {
            await ShellCheckedAsync(
                "for z in /data/media/TWRP/theme/ui.zip /data/media/0/TWRP/theme/ui.zip /sdcard/TWRP/theme/ui.zip; "
                + "do rm -f \"$z\" 2>/dev/null; done; true", Quick, ct).ConfigureAwait(false);
        }
        catch (InvalidOperationException)
        {
        }
    }
}
