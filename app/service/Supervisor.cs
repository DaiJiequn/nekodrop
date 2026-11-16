// SPDX-License-Identifier: GPL-2.0-only
using System.Diagnostics;
using System.Text.Json.Nodes;

namespace NekoDrop.Service;

/// <summary>
/// Owns the card and the two daemons: nekodrop-radio (the user-mode Intel
/// driver) and awdld (winawdl's AWDL daemon). Channel policy, after
/// ForbiddenItlwm 0.6.0: start on 6, where every country allows AWDL; when a
/// peer's channel sequence names 44 or 149 and its country is known, restart
/// there (the move waits until nothing is transferring); fall back to 6 if
/// the 5 GHz channel stays passive.
/// </summary>
public sealed class Supervisor : IDisposable
{
    public enum RadioState { Off, Starting, On, Error }

    readonly string _root = AppContext.BaseDirectory;
    readonly object _lock = new();
    readonly ServiceConfig _cfg = ServiceConfig.Load();
    Process? _radio, _awdld;
    int _channel;
    /// <summary>The network last read from Windows; once the card is ours the WLAN service sees nothing.</summary>
    Wlan.Network? _net;
    string _wifi = "";
    string _detail = "";
    bool _wanted;
    DateTime _lastUse = DateTime.UtcNow, _lastData = DateTime.UtcNow, _startedAt;
    long _rxToUs;
    int _restarts;
    CancellationTokenSource _cts = new();

    public RadioState State { get; private set; } = RadioState.Off;
    public event Action<JsonObject>? Changed;
    public event Action<string>? Log;

    public NetMode Mode => _cfg.Mode;

    public JsonObject Snapshot()
    {
        lock (_lock)
        {
            return new JsonObject
            {
                ["event"] = "state",
                ["radio"] = State.ToString().ToLowerInvariant(),
                ["mode"] = _cfg.Mode.ToString().ToLowerInvariant(),
                ["channel"] = _channel,
                ["country"] = _cfg.Country,
                ["wifi"] = _wifi,
                ["detail"] = _detail,
            };
        }
    }

    void Publish() => Changed?.Invoke(Snapshot());

    void Say(string line)
    {
        Log?.Invoke(line);
        try
        {
            Directory.CreateDirectory(ServiceConfig.Folder);
            File.AppendAllText(Path.Combine(ServiceConfig.Folder, "service.log"),
                $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {line}{Environment.NewLine}");
        }
        catch (IOException) { }
    }

    /// <summary>Called once when the service starts.</summary>
    public void Boot()
    {
        // A config the radio never read (it deletes it once read) holds the
        // Wi-Fi key: never leave one behind across a restart.
        try { File.Delete(Path.Combine(ServiceConfig.Folder, "run", "sta.conf")); } catch (Exception) { }
        if (KernelDriver) Start("boot (kernel driver: Windows keeps Wi-Fi)");
        else if (_cfg.Mode == NetMode.Concurrent) Start("boot (concurrent mode)");
        else EnsureVendorDriver();
        _ = Task.Run(() => Watchdog(_cts.Token));
    }

    public void SetMode(NetMode mode)
    {
        lock (_lock) { _cfg.Mode = mode; _cfg.Save(); }
        Say($"mode: {mode}");
        if (mode == NetMode.Concurrent) Start("mode changed");
        else Stop("mode changed", giveBack: true);
        Publish();
    }

    /// <summary>The client wants AirDrop now (opened the window, a phone nearby is sharing).</summary>
    public void Wake()
    {
        _lastUse = DateTime.UtcNow;
        if (_cfg.Mode != NetMode.Off) Start("woken");
    }

    public void Touch() => _lastUse = DateTime.UtcNow;

    public void Start(string why)
    {
        lock (_lock)
        {
            if (_wanted && State is RadioState.On or RadioState.Starting) return;
            _wanted = true;
            _restarts = 0;
        }
        Say($"starting: {why}");
        _ = Task.Run(() => Launch(_cfg.PeerChannel != 0 && _cfg.Country != "" ? _cfg.PeerChannel : _cfg.StartChannel));
    }

    public void Stop(string why, bool giveBack)
    {
        lock (_lock) _wanted = false;
        Say($"stopping: {why}");
        KillDaemons();
        if (giveBack) EnsureVendorDriver();
        lock (_lock) { State = RadioState.Off; _wifi = ""; _channel = 0; }
        Publish();
    }

    /// <summary>One launch at a time: the watchdog, a wake, a mode change and a channel move
    /// used to start radios concurrently and kill each other's (hardware, 2026-11-17).</summary>
    readonly SemaphoreSlim _launchGate = new(1, 1);

    void Launch(int channel)
    {
        _launchGate.Wait();
        try { LaunchLocked(channel); }
        finally { _launchGate.Release(); }
    }

    void LaunchLocked(int channel)
    {
        if (!_wanted) return;
        lock (_lock) { State = RadioState.Starting; _channel = channel; _detail = ""; }
        Publish();
        try
        {
            KillDaemons();
            string? staConf = null;
            var kernel = KernelDriver;
            if (!kernel && _cfg.Mode == NetMode.Concurrent)
            {
                var why = "";
                var cardFree = !(Card.Find() is { } c && Card.TakenByProxy(c));
                var fresh = cardFree ? Wlan.Current(out why) : null;
                // Not connected (USB tethering, or Intel's driver just got the card back):
                // let Windows join a saved network once, to read what the station needs.
                for (int i = 0; cardFree && fresh is null && _net is null && i < 4; i++)
                {
                    if (i == 0) Say("Wi-Fi: Windows is not connected; joining a saved network to read it");
                    fresh = Wlan.ConnectSaved(out why, TimeSpan.FromSeconds(15));
                    if (fresh is null && why == "no WLAN interface") Thread.Sleep(5000);
                    else break;
                }
                if (fresh is not null) _net = fresh;
                var net = fresh ?? _net;
                if (net is not null && (net.Open || net.Passphrase is not null || net.PskHex is not null))
                {
                    staConf = Wlan.WriteStaConfig(net);
                    Say($"Wi-Fi: joining \"{net.Ssid}\" ({net.Security}, channel {net.Channel}) beside AirDrop");
                }
                else if (!string.IsNullOrEmpty(why))
                {
                    Say($"Wi-Fi: {why}; AirDrop runs without it");
                    lock (_lock) _wifi = "unsupported: " + why;
                }
            }
            // With nekodrop-wlan.sys the card stays where it is: Windows runs
            // the station, the radio only drives AWDL through the driver.
            if (!kernel) TakeCard();
            var width = channel <= 14 ? 20 : 80;
            var args = new List<string> { channel.ToString(), "--width", width.ToString() };
            if (kernel) args.Add("--kernel");
            if (_cfg.Country != "") args.AddRange(["--country", _cfg.Country]);
            if (staConf is not null) args.AddRange(["--sta-config", staConf]);
            _radio = Spawn(Path.Combine(_root, "radio", "nekodrop-radio.exe"), args, OnRadioLine);
            _startedAt = DateTime.UtcNow;
            // awdld connects to the radio's pipe; give the firmware time to boot.
            for (int i = 0; i < 100 && State == RadioState.Starting && _radio is { HasExited: false }; i++) Thread.Sleep(100);
            if (_radio is null || _radio.HasExited) throw new InvalidOperationException("the radio exited during bring-up: " + _detail);
            _awdld = Spawn(Path.Combine(_root, "awdl", "awdld.exe"), ["-c", channel.ToString()], line => Say("awdld: " + line));
        }
        catch (Exception e)
        {
            try { File.Delete(Path.Combine(ServiceConfig.Folder, "run", "sta.conf")); } catch (Exception) { }
            Say("start failed: " + e.Message);
            lock (_lock) { State = RadioState.Error; _detail = e.Message; }
            Publish();
        }
    }

    void OnRadioLine(string line)
    {
        Say("radio: " + line);
        var at = line.IndexOf("radio: ", StringComparison.Ordinal);
        var text = at >= 0 ? line[(at + 7)..] : line;
        if (text.StartsWith("ready on channel", StringComparison.Ordinal))
        {
            lock (_lock) State = RadioState.On;
            Publish();
        }
        else if (text.StartsWith("bring-up failed: ", StringComparison.Ordinal))
        {
            lock (_lock) _detail = text[17..];
        }
        else if (text.StartsWith("rx: ", StringComparison.Ordinal))
        {
            // "... N addressed to us": transfers keep a channel move waiting.
            var parts = text.Split(' ');
            var i = Array.IndexOf(parts, "addressed");
            if (i > 0 && long.TryParse(parts[i - 1], out var n))
            {
                if (n != _rxToUs) { _lastData = DateTime.UtcNow; _lastUse = DateTime.UtcNow; }
                _rxToUs = n;
            }
        }
        else if (text.StartsWith("event: ", StringComparison.Ordinal))
        {
            OnEvent(text[7..].Split(' ', 2));
        }
    }

    void OnEvent(string[] e)
    {
        var name = e[0];
        var value = e.Length > 1 ? e[1] : "";
        switch (name)
        {
            case "peer-country":
            case "country":
                if (value.Length == 2 && value != _cfg.Country)
                {
                    lock (_lock) { _cfg.Country = value; _cfg.Save(); }
                    Publish();
                }
                break;
            case "peer-channel":
                if (int.TryParse(value, out var ch) && (ch == 44 || ch == 149))
                {
                    lock (_lock) { _cfg.PeerChannel = ch; _cfg.Save(); }
                    if (_channel != ch) _ = Task.Run(() => MoveWhenIdle(ch));
                }
                break;
            case "channel":
                // "44 passive": no transmitting until a beacon is heard; if none comes, go back to 6.
                if (value.EndsWith("passive", StringComparison.Ordinal))
                    _ = Task.Run(async () =>
                    {
                        await Task.Delay(TimeSpan.FromSeconds(30));
                        // A connected station proves the channel is usable: no bouncing back to 6.
                        if (State == RadioState.On && _channel > 14 && _rxToUs == 0 && _wifi != "connected")
                        {
                            Say($"channel {_channel} stayed passive: back to {_cfg.StartChannel}");
                            lock (_lock) _cfg.PeerChannel = 0;
                            Relaunch(_cfg.StartChannel);
                        }
                    });
                break;
            case "wifi":
                lock (_lock) _wifi = value;
                Publish();
                break;
        }
    }

    async Task MoveWhenIdle(int channel)
    {
        // Upstream waits 10 s without data before a move (kChannelMoveIdleUS).
        for (int i = 0; i < 120; i++)
        {
            if (!_wanted || _channel == channel) return;
            if (DateTime.UtcNow - _lastData > TimeSpan.FromSeconds(10)) break;
            await Task.Delay(1000);
        }
        if (_wanted && _channel != channel)
        {
            Say($"peer uses channel {channel}: moving there (country {_cfg.Country})");
            Relaunch(channel);
        }
    }

    void Relaunch(int channel)
    {
        _launchGate.Wait();
        try
        {
            KillDaemons();
            LaunchLocked(channel);
        }
        finally { _launchGate.Release(); }
    }

    async Task Watchdog(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            await Task.Delay(2000, ct).ContinueWith(_ => { });
            if (!_wanted) continue;
            var radioDown = _radio is null || _radio.HasExited;
            var awdldDown = State == RadioState.On && (_awdld is null || _awdld.HasExited);
            if ((radioDown || awdldDown) && State != RadioState.Starting && _launchGate.CurrentCount > 0)
            {
                if (++_restarts > 5)
                {
                    Say("the radio keeps failing; giving the card back");
                    Stop("too many failures", giveBack: true);
                    lock (_lock) { State = RadioState.Error; _detail = "the radio keeps failing"; }
                    Publish();
                    continue;
                }
                Say("a daemon stopped; restarting");
                Relaunch(_channel == 0 ? _cfg.StartChannel : _channel);
            }
            else if (State == RadioState.On && DateTime.UtcNow - _startedAt > TimeSpan.FromMinutes(2))
            {
                _restarts = 0;
            }
            if (_cfg.Mode == NetMode.OnDemand && State == RadioState.On &&
                DateTime.UtcNow - _lastUse > TimeSpan.FromSeconds(_cfg.IdleSeconds))
                Stop("idle", giveBack: true);
        }
    }

    void TakeCard()
    {
        var card = Card.Find() ?? throw new InvalidOperationException("no Intel AX201-family card found");
        if (Card.TakenByProxy(card)) return;
        if (card.InfPath != "" && !card.InfPath.Contains("regproxy", StringComparison.OrdinalIgnoreCase))
        {
            lock (_lock) { _cfg.VendorInf = card.InfPath; _cfg.Save(); }
        }
        var inf = Path.Combine(_root, "driver", "winawdl_regproxy.inf");
        Say($"taking the card from {Path.GetFileName(card.InfPath)}");
        Card.Bind(card.HardwareId, inf);
        // The proxy's device interface appears once the UMDF host has started.
        for (int i = 0; i < 300; i++)
        {
            var now = Card.Find();
            if (now is not null && Card.TakenByProxy(now)) return;
            Thread.Sleep(100);
        }
    }

    /// <summary>The card runs nekodrop's kernel Wi-Fi driver (nekodrop-wlan.sys).</summary>
    static bool KernelDriver => Card.Find() is { } k && Card.ByKernelDriver(k);

    void EnsureVendorDriver()
    {
        var card = Card.Find();
        if (card is null || !Card.TakenByProxy(card)) return;
        if (_cfg.VendorInf == "" || !File.Exists(_cfg.VendorInf))
        {
            Say("cannot give the card back: Intel's INF is not known; run give-card.ps1");
            return;
        }
        try
        {
            Say($"giving the card back to {Path.GetFileName(_cfg.VendorInf)}");
            Card.Bind(card.HardwareId, _cfg.VendorInf);
        }
        catch (Exception e) { Say("giving the card back failed: " + e.Message); }
    }

    Process Spawn(string exe, IEnumerable<string> args, Action<string> onLine)
    {
        var psi = new ProcessStartInfo(exe)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            WorkingDirectory = Path.GetDirectoryName(exe)!,
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        var p = Process.Start(psi) ?? throw new InvalidOperationException("cannot start " + exe);
        p.ErrorDataReceived += (_, e) => { if (e.Data is not null) onLine(e.Data); };
        p.OutputDataReceived += (_, e) => { if (e.Data is not null) onLine(e.Data); };
        p.BeginErrorReadLine();
        p.BeginOutputReadLine();
        return p;
    }

    void KillDaemons()
    {
        foreach (var p in new[] { _awdld, _radio })
        {
            try
            {
                if (p is { HasExited: false })
                {
                    p.Kill();
                    p.WaitForExit(5000);
                }
            }
            catch (InvalidOperationException) { }
        }
        _awdld = _radio = null;
    }

    public void Dispose()
    {
        _cts.Cancel();
        KillDaemons();
        if (_cfg.Mode != NetMode.Concurrent) EnsureVendorDriver();
    }
}
