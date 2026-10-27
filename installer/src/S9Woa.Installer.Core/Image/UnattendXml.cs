// SPDX-License-Identifier: BSD-2-Clause-Patent
using System.Xml.Linq;

namespace S9Woa.Installer.Core.Image;

/// <summary>Choices that shape the generated OOBE answer file.</summary>
public sealed record UnattendOptions
{
    public string Username { get; init; } = "S9";
    public string? Password { get; init; }
    public string Locale { get; init; } = "en-US";
    public string TimeZone { get; init; } = "UTC";
    public string ComputerName { get; init; } = "GALAXY-S9";
    public bool AutoLogon { get; init; } = true;

    /// <summary>
    /// Display scaling in DPI (96 = 100%). The phone's panel reports no physical size, so Windows
    /// would start at 100% - unreadably small on a 6.2" 1440x2960 screen; 275 is about 286%.
    /// Null leaves Windows' own choice.
    /// </summary>
    public int? Dpi { get; init; } = 275;
}

/// <summary>
/// Generates a Windows Setup answer file that drives OOBE to the desktop without
/// user input: it creates a local administrator, skips the EULA, privacy,
/// Microsoft-account and wireless screens, and sets the locale, time zone and
/// computer name. In the specialize pass it grows the Windows volume to fill USERDATA
/// (<see cref="ExtendVolumeCommand"/>) and sets the display scaling (<see cref="UnattendOptions.Dpi"/>).
/// Uses only the public unattend schema — no product internals.
/// Written into the offline image at <c>Windows\Panther\unattend.xml</c>, which
/// Windows processes during the specialize and oobeSystem passes.
/// </summary>
public static class UnattendXml
{
    private static readonly XNamespace Ns = "urn:schemas-microsoft-com:unattend";
    private static readonly XNamespace Wcm = "http://schemas.microsoft.com/WMIConfig/2002/State";

    private const string Arch = "arm64";
    private const string PublicKeyToken = "31bf3856ad364e35"; // Well-known Microsoft component token.

    /// <summary>Where the default user profile's hive is loaded while the scaling is written.</summary>
    private const string DefaultUserHive = @"HKU\S9WoaDefaultUser";

    public const int MinimumDpi = 96;
    public const int MaximumDpi = 480;

    public static string RelativePath => @"Windows\Panther\unattend.xml";

    public static string Build(UnattendOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        if (string.IsNullOrWhiteSpace(options.Username))
        {
            throw new ArgumentException("A local account name is required.", nameof(options));
        }
        if (options.Dpi is < MinimumDpi or > MaximumDpi)
        {
            throw new ArgumentOutOfRangeException(nameof(options), options.Dpi,
                $"Display scaling must be between {MinimumDpi} and {MaximumDpi} DPI.");
        }

        var doc = new XDocument(
            new XDeclaration("1.0", "utf-8", null),
            new XElement(Ns + "unattend",
                new XAttribute(XNamespace.Xmlns + "wcm", Wcm.NamespaceName),
                Specialize(options.Dpi),
                OobeSystem(options)));
        // The file is written as UTF-8, so the declaration must say UTF-8. Saving into a plain
        // StringWriter would declare "utf-16" (its own encoding) over UTF-8 bytes, which Windows
        // Setup cannot parse: "internal error while loading or searching for an unattend answer file".
        using var writer = new Utf8StringWriter();
        doc.Save(writer);
        return writer.ToString();
    }

    /// <summary>The UTF-8 bytes to write to <see cref="RelativePath"/> (no byte order mark).</summary>
    public static byte[] BuildBytes(UnattendOptions options) => new System.Text.UTF8Encoding(false).GetBytes(Build(options));

    private sealed class Utf8StringWriter : StringWriter
    {
        public Utf8StringWriter() : base(System.Globalization.CultureInfo.InvariantCulture)
        {
        }

        public override System.Text.Encoding Encoding => new System.Text.UTF8Encoding(false);
    }

    private static XElement Component(string name, params object[] content) =>
        new(Ns + "component",
            new XAttribute("name", name),
            new XAttribute("processorArchitecture", Arch),
            new XAttribute("publicKeyToken", PublicKeyToken),
            new XAttribute("language", "neutral"),
            new XAttribute("versionScope", "nonSxS"),
            content);

    /// <summary>
    /// Display scaling, written the way Settings' custom scaling writes it (<c>LogPixels</c> with
    /// <c>Win8DpiScaling</c> = 1): into the default user profile, which the account created in
    /// OOBE is copied from, and into <c>.DEFAULT</c>, which the sign-in screen uses. The unattend
    /// <c>Display/DPI</c> setting is no longer honoured, so the specialize pass - before any
    /// account exists - loads the default hive and sets the values with reg.exe.
    /// </summary>
    internal static IReadOnlyList<string> DpiCommands(int dpi)
    {
        string[] SetScaling(string key) =>
        [
            $"reg.exe add \"{key}\\Control Panel\\Desktop\" /v LogPixels /t REG_DWORD /d {dpi} /f",
            $"reg.exe add \"{key}\\Control Panel\\Desktop\" /v Win8DpiScaling /t REG_DWORD /d 1 /f",
        ];
        return
        [
            $"cmd.exe /c reg.exe load {DefaultUserHive} \"%SystemDrive%\\Users\\Default\\NTUSER.DAT\"",
            .. SetScaling(DefaultUserHive),
            $"reg.exe unload {DefaultUserHive}",
            .. SetScaling(@"HKU\.DEFAULT"),
        ];
    }

    /// <summary>
    /// The image is built to the 64 GB phone's USERDATA. On the 128 GB and 256 GB phones USERDATA
    /// starts at the same place but is larger, so the NTFS volume is grown to fill it before any
    /// account exists. diskpart's <c>extend filesystem</c> grows only the file system, inside the
    /// partition it is already on, so it never touches the partition table; where there is nothing
    /// to grow (the 64 GB phone) it fails harmlessly, and the step always reports success so it
    /// cannot hold up setup.
    /// </summary>
    internal const string ExtendVolumeCommand =
        @"cmd.exe /c (echo select volume %SystemDrive:~0,1%& echo extend filesystem)>""%WINDIR%\Temp\s9extend.txt"""
        + @" & diskpart.exe /s ""%WINDIR%\Temp\s9extend.txt"" & del ""%WINDIR%\Temp\s9extend.txt"" & exit /b 0";

    private static XElement Specialize(int? dpi)
    {
        var commands = new List<(string Description, string Path)> { ("Grow Windows to fill USERDATA", ExtendVolumeCommand) };
        if (dpi is { } d)
        {
            commands.AddRange(DpiCommands(d).Select((command, i) => ($"Display scaling {d} DPI ({i + 1})", command)));
        }
        return new(Ns + "settings",
            new XAttribute("pass", "specialize"),
            Component("Microsoft-Windows-Deployment",
                new XElement(Ns + "RunSynchronous",
                    commands.Select((c, i) =>
                        new XElement(Ns + "RunSynchronousCommand",
                            new XAttribute(Wcm + "action", "add"),
                            new XElement(Ns + "Order", i + 1),
                            new XElement(Ns + "Description", c.Description),
                            new XElement(Ns + "Path", c.Path))))));
    }

    private static XElement OobeSystem(UnattendOptions o)
    {
        var shell = Component("Microsoft-Windows-Shell-Setup",
            new XElement(Ns + "ComputerName", o.ComputerName),
            new XElement(Ns + "TimeZone", o.TimeZone),
            new XElement(Ns + "OOBE",
                new XElement(Ns + "HideEULAPage", "true"),
                new XElement(Ns + "HideOEMRegistrationScreen", "true"),
                new XElement(Ns + "HideOnlineAccountScreens", "true"),
                new XElement(Ns + "HideWirelessSetupInOOBE", "true"),
                new XElement(Ns + "ProtectYourPC", "3"),
                new XElement(Ns + "SkipMachineOOBE", "true"),
                new XElement(Ns + "SkipUserOOBE", "true")),
            UserAccounts(o));
        if (o.AutoLogon)
        {
            shell.Add(AutoLogon(o));
        }

        return new XElement(Ns + "settings",
            new XAttribute("pass", "oobeSystem"),
            Component("Microsoft-Windows-International-Core",
                new XElement(Ns + "InputLocale", o.Locale),
                new XElement(Ns + "SystemLocale", o.Locale),
                new XElement(Ns + "UILanguage", o.Locale),
                new XElement(Ns + "UserLocale", o.Locale)),
            shell);
    }

    private static XElement UserAccounts(UnattendOptions o) =>
        new(Ns + "UserAccounts",
            new XElement(Ns + "LocalAccounts",
                new XElement(Ns + "LocalAccount",
                    new XAttribute(Wcm + "action", "add"),
                    new XElement(Ns + "Name", o.Username),
                    new XElement(Ns + "DisplayName", o.Username),
                    new XElement(Ns + "Group", "Administrators"),
                    Password(o.Password))));

    private static XElement AutoLogon(UnattendOptions o) =>
        new(Ns + "AutoLogon",
            new XElement(Ns + "Enabled", "true"),
            new XElement(Ns + "LogonCount", "1"),
            new XElement(Ns + "Username", o.Username),
            Password(o.Password));

    private static XElement Password(string? value) =>
        new(Ns + "Password",
            new XElement(Ns + "Value", value ?? ""),
            new XElement(Ns + "PlainText", "true"));
}
