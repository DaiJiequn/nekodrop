// SPDX-License-Identifier: GPL-2.0-only
using System.Runtime.InteropServices;
using System.Windows.Interop;

namespace NekoDrop;

/// <summary>
/// The global shortcut Win+Shift+D (RegisterHotKey on a message-only window).
/// Registering fails quietly when another program holds the combination.
/// </summary>
public sealed class Hotkey : IDisposable
{
    const int WM_HOTKEY = 0x0312, Id = 0x6E6B; // "nk"
    const uint MOD_SHIFT = 0x4, MOD_WIN = 0x8, MOD_NOREPEAT = 0x4000, VK_D = 0x44;

    [DllImport("user32.dll", SetLastError = true)]
    static extern bool RegisterHotKey(IntPtr hWnd, int id, uint modifiers, uint vk);
    [DllImport("user32.dll")]
    static extern bool UnregisterHotKey(IntPtr hWnd, int id);

    readonly HwndSource _window;
    readonly Action _pressed;

    Hotkey(HwndSource window, Action pressed)
    {
        _window = window;
        _pressed = pressed;
        _window.AddHook(Hook);
    }

    public static Hotkey? Register(Action pressed)
    {
        var p = new HwndSourceParameters("nekodrop-hotkey") { ParentWindow = new IntPtr(-3) /* HWND_MESSAGE */ };
        var window = new HwndSource(p);
        if (RegisterHotKey(window.Handle, Id, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, VK_D)) return new Hotkey(window, pressed);
        window.Dispose();
        return null;
    }

    IntPtr Hook(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WM_HOTKEY && wParam.ToInt32() == Id) { handled = true; _pressed(); }
        return IntPtr.Zero;
    }

    public void Dispose()
    {
        UnregisterHotKey(_window.Handle, Id);
        _window.RemoveHook(Hook);
        _window.Dispose();
    }
}
