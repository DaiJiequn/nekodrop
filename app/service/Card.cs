// SPDX-License-Identifier: GPL-2.0-only
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace NekoDrop.Service;

/// <summary>
/// Which driver owns the Wi-Fi card. Taking it binds the register-proxy driver
/// (what devcon's "update" does: UpdateDriverForPlugAndPlayDevices with
/// INSTALLFLAG_FORCE, since Intel's driver ranks higher); giving it back binds
/// Intel's own INF the same way, so no package is ever deleted.
/// </summary>
public static class Card
{
    /// <summary>The Qu/QuZ CNVi parts the radio can drive (AX201 and its Killer twins).</summary>
    public static readonly string[] DeviceIds = ["43F0", "A0F0", "02F0", "06F0", "34F0", "3DF0", "4DF0"];
    const string ProxyService = "WUDFRd";

    public sealed record Info(string InstanceId, string HardwareId, string Service, string InfPath);

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
                foreach (var dev in DeviceIds)
                {
                    if (!id.StartsWith($"PCI\\VEN_8086&DEV_{dev}", StringComparison.OrdinalIgnoreCase)) continue;
                    var service = RegistryProperty(set, ref data, SPDRP_SERVICE);
                    var inf = DriverInf(id);
                    return new Info(id, $"PCI\\VEN_8086&DEV_{dev}", service, inf);
                }
            }
        }
        finally { SetupDiDestroyDeviceInfoList(set); }
        return null;
    }

    public static bool TakenByProxy(Info card) =>
        string.Equals(card.Service, ProxyService, StringComparison.OrdinalIgnoreCase);

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
                throw new Win32Exception(Marshal.GetLastWin32Error(), $"binding {Path.GetFileName(inf)} failed");
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

    /// <summary>The INF the card is bound to, as a full path (C:\Windows\INF\oemNN.inf).</summary>
    static string DriverInf(string instanceId)
    {
        using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(
            $@"SYSTEM\CurrentControlSet\Enum\{instanceId}");
        var driver = key?.GetValue("Driver") as string;
        if (driver is null) return "";
        using var cls = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(
            $@"SYSTEM\CurrentControlSet\Control\Class\{driver}");
        var inf = cls?.GetValue("InfPath") as string;
        return inf is null ? "" : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "INF", inf);
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
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hardwareId, string infPath,
        uint flags, out bool reboot);
}
