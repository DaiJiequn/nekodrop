// SPDX-License-Identifier: GPL-2.0-only
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace NekoDrop.Service;

/// <summary>Windows refused to bind a driver to the card (with its error code).</summary>
public sealed class CardBindException(string message, Exception inner) : Exception(message, inner);

/// <summary>
/// Which driver owns the Wi-Fi card. Taking it binds the register-proxy driver
/// (what devcon's "update" does: UpdateDriverForPlugAndPlayDevices with
/// INSTALLFLAG_FORCE, since the vendor's driver ranks higher); giving it back
/// binds the vendor's own INF the same way, so no package is ever deleted.
/// Which cards, and their proxy and driver names, come from CardCatalog.
/// </summary>
public static class Card
{
    /// <summary>
    /// The PCI device IDs of the cards the service takes, without the vendor.
    /// Kept for callers of the earlier list; new code uses CardCatalog, where a
    /// device ID is only meaningful with its vendor ID.
    /// </summary>
    public static readonly string[] DeviceIds = CardCatalog.ActiveProfiles.Select(p => p.DeviceId).ToArray();

    /// <param name="Profile">the catalog entry the card matched: vendor, radio, capabilities, proxy names</param>
    /// <param name="Provider">the bound driver's provider (class key ProviderName)</param>
    /// <param name="Problem">Device Manager's problem code (0: none, or the device is not started yet)</param>
    public sealed record Info(string InstanceId, string HardwareId, string Service, string InfPath,
                              AdapterProfile Profile, string Provider = "", uint Problem = 0);

    public static Info? Find()
    {
        var set = SetupDiGetClassDevsW(IntPtr.Zero, "PCI", IntPtr.Zero, DIGCF_PRESENT | DIGCF_ALLCLASSES);
        if (set == new IntPtr(-1)) return null;
        try
        {
            var data = new SP_DEVINFO_DATA { cbSize = (uint)Marshal.SizeOf<SP_DEVINFO_DATA>() };
            for (uint i = 0; SetupDiEnumDeviceInfo(set, i, ref data); i++)
            {
                var id = InstanceId(set, ref data);
                // Only active profiles: a card listed for diagnostics (RTL8852CE) is never taken.
                if (CardCatalog.MatchActive(id) is not { } profile) continue;
                var service = RegistryProperty(set, ref data, SPDRP_SERVICE);
                var (inf, provider) = DriverInf(id);
                uint problem = 0;
                if (CM_Get_DevNode_Status(out _, out var p, data.DevInst, 0) == 0) problem = p;
                return new Info(id, profile.HardwareId, service, inf, profile, provider, problem);
            }
        }
        finally { SetupDiDestroyDeviceInfoList(set); }
        return null;
    }

    public static bool TakenByProxy(Info card) =>
        string.Equals(card.Service, card.Profile.ProxyService, StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// Bound to nekodrop's proxy, running or not. The INF name says nothing
    /// (Windows renames it oemNN.inf), so the proxy INF's Provider tells it
    /// apart: a proxy that failed to start was taken for the vendor's driver
    /// and recorded as the one to give the card back to (issue #12).
    /// </summary>
    public static bool BoundToProxy(Info card) =>
        TakenByProxy(card) || string.Equals(card.Provider, card.Profile.ProxyProvider, StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// The card is bound to nekodrop-wlan.sys: Windows' Wi-Fi and AWDL on one
    /// driver. Only for cards that driver supports (all the Intel ones).
    /// </summary>
    public static bool ByKernelDriver(Info card) =>
        card.Profile.Capabilities.SupportsKernelDriver &&
        string.Equals(card.Service, "nekodrop-wlan", StringComparison.OrdinalIgnoreCase);

    /// <summary>Binds `inf` to the card, whatever ranks higher.</summary>
    /// <summary>
    /// Binds `inf` to the card, whatever ranks higher. The call has been seen to
    /// block for minutes (hardware, 2026-11-17); after `timeout` the caller goes
    /// on and checks which driver owns the card.
    /// </summary>
    public static void Bind(string hardwareId, string inf, TimeSpan? timeout = null)
    {
        var call = Task.Run(() =>
        {
            if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, hardwareId, inf, INSTALLFLAG_FORCE, out _))
            {
                var w = new Win32Exception(Marshal.GetLastWin32Error());
                throw new CardBindException(
                    $"binding {Path.GetFileName(inf)} failed: 0x{w.NativeErrorCode:x8} {w.Message}", w);
            }
        });
        if (!call.Wait(timeout ?? TimeSpan.FromSeconds(45)))
            return; /* still running; the device state decides */
        if (call.Exception is { } e) throw e.InnerException ?? e;
    }

    static string InstanceId(IntPtr set, ref SP_DEVINFO_DATA data)
    {
        var sb = new StringBuilder(512);
        return SetupDiGetDeviceInstanceIdW(set, ref data, sb, sb.Capacity, out _) ? sb.ToString() : "";
    }

    static string RegistryProperty(IntPtr set, ref SP_DEVINFO_DATA data, uint property)
    {
        var buf = new byte[1024];
        return SetupDiGetDeviceRegistryPropertyW(set, ref data, property, out _, buf, (uint)buf.Length, out var size)
            ? Encoding.Unicode.GetString(buf, 0, (int)Math.Max(0, size - 2)).TrimEnd('\0')
            : "";
    }

    /// <summary>The INF the card is bound to, as a full path (C:\Windows\INF\oemNN.inf), and its provider.</summary>
    static (string Inf, string Provider) DriverInf(string instanceId)
    {
        using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(
            $@"SYSTEM\CurrentControlSet\Enum\{instanceId}");
        var driver = key?.GetValue("Driver") as string;
        if (driver is null) return ("", "");
        using var cls = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(
            $@"SYSTEM\CurrentControlSet\Control\Class\{driver}");
        var inf = cls?.GetValue("InfPath") as string;
        var provider = cls?.GetValue("ProviderName") as string ?? "";
        return (inf is null ? "" : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "INF", inf), provider);
    }

    const uint DIGCF_PRESENT = 0x2, DIGCF_ALLCLASSES = 0x4, SPDRP_SERVICE = 0x4, INSTALLFLAG_FORCE = 0x1;

    [StructLayout(LayoutKind.Sequential)]
    struct SP_DEVINFO_DATA { public uint cbSize; public Guid ClassGuid; public uint DevInst; public IntPtr Reserved; }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr SetupDiGetClassDevsW(IntPtr classGuid, string enumerator, IntPtr parent, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiEnumDeviceInfo(IntPtr set, uint index, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiGetDeviceInstanceIdW(IntPtr set, ref SP_DEVINFO_DATA data, StringBuilder id, int size, out int required);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA data, uint property,
        out uint type, byte[] buffer, uint size, out uint required);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("cfgmgr32.dll")]
    static extern uint CM_Get_DevNode_Status(out uint status, out uint problem, uint devInst, uint flags);
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hardwareId, string infPath,
        uint flags, out bool reboot);
}
