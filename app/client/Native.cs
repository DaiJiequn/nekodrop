// SPDX-License-Identifier: GPL-2.0-only
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using Microsoft.Win32;

namespace NekoDrop;

/// <summary>Windows 11 window chrome: Mica backdrop, dark title bar, rounded corners.</summary>
public static class Native
{
    const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
    const int DWMWA_SYSTEMBACKDROP_TYPE = 38;

    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    public static bool AppsUseDarkTheme()
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
        return key?.GetValue("AppsUseLightTheme") is int v && v == 0;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct Margins { public int Left, Right, Top, Bottom; }

    [DllImport("dwmapi.dll")]
    static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd, ref Margins margins);

    static bool TransparencyEnabled()
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
        return key?.GetValue("EnableTransparency") is not int v || v != 0;
    }

    /// <summary>
    /// Dark title bar, rounded corners, and Mica on Windows 11 22H2+ with
    /// transparency effects on. Mica needs the DWM frame over the whole client
    /// area and a transparent WPF surface; without both the window turns black,
    /// so either all of it applies (true: make the background transparent) or
    /// the plain theme background stays (false).
    /// </summary>
    public static bool ApplyMica(Window w)
    {
        var hwnd = new WindowInteropHelper(w).EnsureHandle();
        int dark = AppsUseDarkTheme() ? 1 : 0;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref dark, sizeof(int));
        int round = 2;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, ref round, sizeof(int));
        if (Environment.OSVersion.Version.Build < 22621 || !TransparencyEnabled()) return false;
        if (HwndSource.FromHwnd(hwnd) is not { CompositionTarget: { } target }) return false;
        var margins = new Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        if (DwmExtendFrameIntoClientArea(hwnd, ref margins) != 0) return false;
        int mica = 2;
        if (DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ref mica, sizeof(int)) != 0)
        {
            var none = new Margins();
            DwmExtendFrameIntoClientArea(hwnd, ref none);
            return false;
        }
        target.BackgroundColor = System.Windows.Media.Colors.Transparent;
        return true;
    }

    /// <summary>The user's accent color (DWM colorization), opaque.</summary>
    public static System.Windows.Media.Color Accent()
    {
        using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\DWM");
        if (key?.GetValue("AccentColor") is int abgr)
            return System.Windows.Media.Color.FromRgb((byte)abgr, (byte)(abgr >> 8), (byte)(abgr >> 16));
        return System.Windows.Media.Color.FromRgb(0x00, 0x67, 0xC0);
    }
}
