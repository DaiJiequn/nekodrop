// SPDX-License-Identifier: GPL-2.0-only
using System.Windows;
using System.Windows.Media;

namespace NekoDrop;

/// <summary>
/// Colors after the Windows 11 design (Fluent): follow the system light/dark
/// setting, with the user's accent color as the only accent; Segoe UI
/// Variable for text and Segoe Fluent Icons for glyphs.
/// </summary>
public static class Theme
{
    public static ResourceDictionary Build()
    {
        var dark = Native.AppsUseDarkTheme();
        var accent = Native.Accent();
        Color C(uint argb) => Color.FromArgb((byte)(argb >> 24), (byte)(argb >> 16), (byte)(argb >> 8), (byte)argb);
        SolidColorBrush B(Color c) { var b = new SolidColorBrush(c); b.Freeze(); return b; }
        var d = new ResourceDictionary
        {
            ["Accent"] = B(accent),
            ["AccentText"] = B(dark ? Lighten(accent, 0.45) : Darken(accent, 0.15)),
            ["OnAccent"] = B(Colors.White),
            ["Text"] = B(dark ? C(0xFFFFFFFF) : C(0xE4000000)),
            ["TextSecondary"] = B(dark ? C(0xC5FFFFFF) : C(0x9E000000)),
            ["Window"] = B(dark ? C(0xFF202020) : C(0xFFF3F3F3)),
            ["Card"] = B(dark ? C(0x0DFFFFFF) : C(0xB3FFFFFF)),
            ["CardHover"] = B(dark ? C(0x15FFFFFF) : C(0xFFF9F9F9)),
            ["CardStroke"] = B(dark ? C(0x19000000) : C(0x0F000000)),
            ["Divider"] = B(dark ? C(0x15FFFFFF) : C(0x0F000000)),
            ["DropWell"] = B(dark ? C(0x08FFFFFF) : C(0x06000000)),
            ["UiFont"] = new FontFamily("Segoe UI Variable Text, Segoe UI, Microsoft YaHei UI"),
            ["IconFont"] = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"),
        };
        return d;
    }

    static Color Lighten(Color c, double t) =>
        Color.FromRgb((byte)(c.R + (255 - c.R) * t), (byte)(c.G + (255 - c.G) * t), (byte)(c.B + (255 - c.B) * t));
    static Color Darken(Color c, double t) =>
        Color.FromRgb((byte)(c.R * (1 - t)), (byte)(c.G * (1 - t)), (byte)(c.B * (1 - t)));
}
