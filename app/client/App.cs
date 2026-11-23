// SPDX-License-Identifier: GPL-2.0-only
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Security.Principal;
using System.Text;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Threading;
using Forms = System.Windows.Forms;

namespace NekoDrop;

public sealed class App : Application
{
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Contains("--unregister")) { Shell.Unregister(); return 0; }
        var pipeName = "nekodrop-ui-" + WindowsIdentity.GetCurrent().User?.Value;
        using var single = new Mutex(true, @"Local\nekodrop-ui", out var first);
        if (!first && !args.Contains("--snapshot"))
        {
            // Hand the arguments (files to send, a notification's answer) to the running copy.
            try
            {
                using var client = new NamedPipeClientStream(".", pipeName, PipeDirection.Out);
                client.Connect(3000);
                using var w = new StreamWriter(client, new UTF8Encoding(false));
                w.WriteLine(string.Join("\u001f", args));
            }
            catch (Exception) { }
            return 0;
        }
        var app = new App { ShutdownMode = ShutdownMode.OnExplicitShutdown };
        app._pipeName = pipeName;
        app._startArgs = args;
        return app.Run();
    }

    string _pipeName = "";
    string[] _startArgs = [];
    public static App Instance => (App)Current;
    public AppModel Model { get; } = new();
    public Settings Settings { get; } = Settings.Load();
    public ServiceLink Service { get; } = new();
    public Engine Engine { get; } = new();
    readonly Wake _wake = new();
    Forms.NotifyIcon? _tray;
    MainWindow? _window;
    readonly Dictionary<string, Request> _requests = [];
    readonly Dictionary<string, string> _sendTarget = [];

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        L.Use(Settings.Language);
        var snap = Array.IndexOf(_startArgs, "--snapshot");
        if (snap >= 0 && snap + 1 < _startArgs.Length) { Snapshot(_startArgs[snap + 1]); return; }
        Shell.SetIdentity();
        try { Shell.Register(Settings.StartWithWindows); } catch (Exception) { }
        Resources.MergedDictionaries.Add(Theme.Build());
        CreateTray();
        Service.State += o => Dispatcher.BeginInvoke(() => OnServiceState(o));
        Service.Connected += up => Dispatcher.BeginInvoke(() =>
        {
            if (!up) Model.Status = L.T("status.noService");
        });
        Service.Start();
        Engine.Event += o => Dispatcher.BeginInvoke(() => OnEngine(o));
        Engine.Event += o => EngineLog(o.ToJsonString());
        Engine.Log += EngineLog;
        if (Settings.Discoverable) StartEngine();
        _wake.AirDropNearby += () => Dispatcher.BeginInvoke(() =>
        {
            // effectiveMode (0.1.36+): a card that runs on demand although Concurrent is
            // chosen (RTL8852CE) was never woken by a nearby iPhone (#32).
            var mode = (string?)Service.Last?["effectiveMode"] ?? (string?)Service.Last?["mode"];
            if (mode == "ondemand") Service.Wake();
        });
        _wake.Start();
        var watchdog = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
        watchdog.Tick += (_, _) => EngineWatchdog();
        watchdog.Start();
        // Updates: a look a minute after start, then every 6 hours.
        var updates = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromMinutes(1) };
        updates.Tick += (_, _) => { updates.Interval = TimeSpan.FromHours(6); _ = CheckForUpdate(false); };
        updates.Start();
        _ = Task.Run(ListenForArgs);
        HandleArgs(_startArgs, fromStart: true);
    }

    void StartEngine()
    {
        if (Engine.Running) return;
        try { Identity.Ensure(); } catch (Exception e) { Model.Detail = L.T("err.identity") + e.Message; }
        _engineIf = AwdlIndex();
        _notReceivingSince = null;
        if (!Engine.Start(Settings, out var error)) Model.Detail = L.T("err.engine") + error;
    }

    int _engineIf = -1;

    static readonly object EngineLogLock = new();

    /// <summary>The engine's events and stderr, kept in %LocalAppData%\nekodrop\engine.log (1 MB, one old copy).</summary>
    static void EngineLog(string line)
    {
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "nekodrop");
            var path = Path.Combine(dir, "engine.log");
            lock (EngineLogLock)
            {
                Directory.CreateDirectory(dir);
                if (File.Exists(path) && new FileInfo(path).Length > 1024 * 1024) File.Move(path, path + ".old", true);
                File.AppendAllText(path, DateTime.Now.ToString("HH:mm:ss.fff ") + line + Environment.NewLine);
            }
        }
        catch (Exception) { }
    }

    /// <summary>awdl0's interface index (-1 when absent): a new one means awdld re-created it.</summary>
    static int AwdlIndex()
    {
        try
        {
            foreach (var n in System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces())
                if (n.Name == "awdl0" && n.OperationalStatus == System.Net.NetworkInformation.OperationalStatus.Up)
                    return n.GetIPProperties().GetIPv6Properties().Index;
        }
        catch (Exception) { }
        return -1;
    }

    /// <summary>
    /// Keeps AirDrop discoverable without anyone restarting anything: the engine
    /// comes back if it exits, and starts over when awdl0 is re-created (a radio
    /// restart gives it a new interface the old sockets do not belong to).
    /// </summary>
    void EngineWatchdog()
    {
        if (!Settings.Discoverable || (string?)Service.Last?["radio"] != "on") return;
        var now = AwdlIndex();
        if (now < 0) return; /* no awdl0 yet: wait for awdld */
        if (!Engine.Running)
        {
            StartEngine();
        }
        else if (now != _engineIf
                 || _notReceivingSince is { } since && DateTime.UtcNow - since > TimeSpan.FromSeconds(30))
        {
            Engine.Dispose();
            StartEngine();
        }
    }

    DateTime? _notReceivingSince;

    void CreateTray()
    {
        _tray = new Forms.NotifyIcon
        {
            Text = "nekodrop",
            Icon = System.Drawing.Icon.ExtractAssociatedIcon(Environment.ProcessPath!) ?? System.Drawing.SystemIcons.Application,
            Visible = true,
        };
        _tray.MouseClick += (_, a) => { if (a.Button == Forms.MouseButtons.Left) ShowWindow(); };
        _tray.ContextMenuStrip = BuildMenu();
    }

    Forms.ContextMenuStrip BuildMenu()
    {
        var menu = new Forms.ContextMenuStrip();
        menu.Items.Add(L.T("tray.open"), null, (_, _) => ShowWindow());
        menu.Items.Add(new Forms.ToolStripSeparator());
        var receive = new Forms.ToolStripMenuItem(L.T("tray.receive")) { CheckOnClick = true, Checked = Settings.Discoverable };
        receive.CheckedChanged += (_, _) => SetDiscoverable(receive.Checked);
        menu.Items.Add(receive);
        var mode = new Forms.ToolStripMenuItem(L.T("tray.mode"));
        foreach (var (label, value) in new[] { (L.T("mode.concurrent.short"), "concurrent"), (L.T("mode.ondemand.short"), "ondemand"), (L.T("mode.off.short"), "off") })
        {
            var item = new Forms.ToolStripMenuItem(label) { Tag = value };
            item.Click += (_, _) => Service.SetMode(value);
            mode.DropDownItems.Add(item);
        }
        mode.DropDownOpening += (_, _) =>
        {
            var current = (string?)Service.Last?["mode"];
            foreach (Forms.ToolStripMenuItem i in mode.DropDownItems) i.Checked = (string?)i.Tag == current;
        };
        menu.Items.Add(mode);
        menu.Items.Add(L.T("wifi.tray"), null, (_, _) => ShowWifi());
        menu.Items.Add(L.T("tray.settings"), null, (_, _) => new SettingsWindow().Show());
        menu.Items.Add(L.T("tray.report"), null, (_, _) => RunReport());
        menu.Items.Add(new Forms.ToolStripSeparator());
        menu.Items.Add(L.T("tray.quit"), null, (_, _) => Quit());
        return menu;
    }

    /// <summary>
    /// The diagnostic report (report.ps1 beside the app): it asks for
    /// administrator rights itself and writes the redacted report and logs to
    /// the Desktop, so it also works when the service does not.
    /// </summary>
    public static void RunReport()
    {
        var script = Path.Combine(AppContext.BaseDirectory, "report.ps1");
        if (!File.Exists(script))
        {
            MessageBox.Show(L.T("report.missing"), "nekodrop");
            return;
        }
        try
        {
            Process.Start(new ProcessStartInfo("powershell.exe",
                $"-NoProfile -ExecutionPolicy Bypass -File \"{script}\"") { UseShellExecute = true });
        }
        catch (Exception e) { MessageBox.Show(e.Message, "nekodrop"); }
    }

    public MainWindow? Window => _window;

    async Task LiveSnapshot(string dir, (string Name, Window W)[] windows)
    {
        foreach (var (name, w) in windows)
        {
            w.WindowStartupLocation = WindowStartupLocation.CenterScreen;
            w.Topmost = true;
            w.Show();
            w.Activate();
            await Task.Delay(1500);
            var src = PresentationSource.FromVisual(w);
            var m = src?.CompositionTarget?.TransformToDevice ?? System.Windows.Media.Matrix.Identity;
            int x = (int)(w.Left * m.M11), y = (int)(w.Top * m.M22);
            int cw = (int)(w.ActualWidth * m.M11), ch = (int)(w.ActualHeight * m.M22);
            using (var bmp = new System.Drawing.Bitmap(cw, ch))
            {
                using (var g = System.Drawing.Graphics.FromImage(bmp)) g.CopyFromScreen(x, y, 0, 0, bmp.Size);
                bmp.Save(Path.Combine(dir, name + "-live.png"), System.Drawing.Imaging.ImageFormat.Png);
            }
            w.Close();
        }
        Shutdown();
    }

    /// <summary>
    /// --snapshot DIR [--lang zh|en]: renders the send, main and settings windows
    /// with sample devices to DIR\*.png off screen and exits. For checking the
    /// UI without the service, the radio or a visible window.
    /// </summary>
    void Snapshot(string dir)
    {
        var lang = Array.IndexOf(_startArgs, "--lang");
        if (lang >= 0 && lang + 1 < _startArgs.Length) L.Use(_startArgs[lang + 1]);
        Resources.MergedDictionaries.Add(Theme.Build());
        Directory.CreateDirectory(dir);
        Model.Status = L.F("status.on", 44);
        Model.Detail = "Wi-Fi: " + L.T("wifi.connected");
        Model.Devices.Add(new Device { Id = "a", Name = "iPhone", Model = "iPhone15,2" });
        Model.Devices.Add(new Device { Id = "b", Name = "MacBook Air", Model = "Mac14,2", Busy = true, Progress = 0.42, Status = L.F("send.sending", 0.42) });
        Model.Requests.Add(new Request { Id = "r", Sender = "iPhone", Model = "iPhone15,2", What = L.F("what.photos", 2) });
        Model.ReceivedFiles.Add(new Received { Name = "IMG_0042.HEIC", Path = "", From = "iPhone" });
        foreach (var f in _startArgs.SkipWhile(a => a != "--send").Skip(1).TakeWhile(a => !a.StartsWith("--")))
            Model.Files.Add(f);
        var windows = new (string Name, Window W)[] { ("send", new SendWindow()), ("main", new MainWindow()), ("settings", new SettingsWindow()) };
        if (_startArgs.Contains("--live"))
        {
            // On screen, captured from the screen: shows the real backdrop (Mica).
            _ = LiveSnapshot(dir, windows);
            return;
        }
        foreach (var (name, w) in windows)
        {
            w.WindowStartupLocation = WindowStartupLocation.Manual;
            w.Left = -20000; w.Top = -20000; w.ShowActivated = false; w.ShowInTaskbar = false;
            w.Show();
            w.UpdateLayout();
            if (w.Content is not FrameworkElement root) continue;
            Dispatcher.Invoke(() => { }, DispatcherPriority.ApplicationIdle);
            var width = (int)root.ActualWidth; var height = (int)root.ActualHeight;
            var visual = new System.Windows.Media.DrawingVisual();
            using (var dc = visual.RenderOpen())
            {
                dc.DrawRectangle((System.Windows.Media.Brush)Resources["Window"], null, new Rect(0, 0, width, height));
                dc.DrawRectangle(new System.Windows.Media.VisualBrush(root), null, new Rect(0, 0, width, height));
            }
            var bmp = new System.Windows.Media.Imaging.RenderTargetBitmap(width, height, 96, 96, System.Windows.Media.PixelFormats.Pbgra32);
            bmp.Render(visual);
            var png = new System.Windows.Media.Imaging.PngBitmapEncoder();
            png.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bmp));
            using (var fs = File.Create(Path.Combine(dir, name + ".png"))) png.Save(fs);
            w.Close();
        }
        Shutdown();
    }

    SendWindow? _send;
    DateTime _lastSend = DateTime.MinValue;

    /// <summary>The share sheet: what is being sent, and who to send it to.</summary>
    public void ShowSendWindow()
    {
        Service.Wake();
        if (_send is null)
        {
            var w = new SendWindow();
            w.Closed += (_, _) => { if (_send == w) _send = null; if (_window is null) Engine.Look(false); };
            _send = w;
        }
        _send.Refresh();
        _send.Show();
        if (_send.WindowState == WindowState.Minimized) _send.WindowState = WindowState.Normal;
        _send.Activate();
        Engine.Look(true);
    }

    /// <summary>After the language setting changes: menus, Explorer verb and open windows follow.</summary>
    public void ApplyLanguage()
    {
        L.Use(Settings.Language);
        if (_tray is not null) { var old = _tray.ContextMenuStrip; _tray.ContextMenuStrip = BuildMenu(); old?.Dispose(); }
        try { Shell.Register(Settings.StartWithWindows); } catch (Exception) { }
        if (_window is not null) { var w = _window; _window = null; w.Close(); ShowWindow(); }
    }

    public void SetDiscoverable(bool on)
    {
        Settings.Discoverable = on;
        Settings.Save();
        if (on) { StartEngine(); Service.Wake(); }
        else Engine.Dispose();
    }

    public void ShowWindow()
    {
        Service.Wake();
        if (_window is null)
        {
            var w = new MainWindow();
            w.Closed += (_, _) => { if (_window == w) _window = null; };
            _window = w;
        }
        _window.Show();
        if (_window.WindowState == WindowState.Minimized) _window.WindowState = WindowState.Normal;
        _window.Activate();
        Engine.Look(true);
    }

    public void WindowClosed() { if (_send is null) Engine.Look(false); }

    void Quit()
    {
        _wake.Dispose();
        Engine.Dispose();
        Service.Dispose();
        if (_tray is not null) _tray.Visible = false;
        Shutdown();
    }

    async Task ListenForArgs()
    {
        while (true)
        {
            try
            {
                using var server = new NamedPipeServerStream(_pipeName, PipeDirection.In, 1, PipeTransmissionMode.Byte,
                                                             PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                await server.WaitForConnectionAsync();
                using var r = new StreamReader(server, Encoding.UTF8);
                var line = await r.ReadLineAsync() ?? "";
                var args = line.Length == 0 ? [] : line.Split('\u001f');
                await Dispatcher.BeginInvoke(() => HandleArgs(args, fromStart: false));
            }
            catch (Exception) { await Task.Delay(500); }
        }
    }

    /// <summary>--send FILE…, --background, or a nekodrop: URI from a notification.</summary>
    void HandleArgs(string[] args, bool fromStart)
    {
        if (args.Length > 0 && args[0].StartsWith("nekodrop:", StringComparison.OrdinalIgnoreCase))
        {
            HandleUri(new Uri(args[0]));
            return;
        }
        var files = args.SkipWhile(a => a != "--send").Skip(1).Where(a => File.Exists(a) || Directory.Exists(a)).ToList();
        if (files.Count > 0)
        {
            // A new right-click send replaces what was queued, except that Explorer
            // starts one process per selected item for classic verbs: those arrive
            // within moments of each other and add up.
            if (DateTime.UtcNow - _lastSend > TimeSpan.FromSeconds(2)) Model.Files.Clear();
            _lastSend = DateTime.UtcNow;
            foreach (var f in files) if (!Model.Files.Contains(f)) Model.Files.Add(f);
            ShowSendWindow();
        }
        else if (args.Contains("--send")) ShowSendWindow();
        else if (!(fromStart && args.Contains("--background"))) ShowWindow();
    }

    void HandleUri(Uri uri)
    {
        var q = System.Web.HttpUtility.ParseQueryString(uri.Query);
        switch (uri.AbsolutePath.ToLowerInvariant())
        {
            case "answer":
                var id = q["id"] ?? "";
                Answer(id, q["accept"] == "1");
                break;
            case "update":
                _ = InstallUpdate();
                break;
            case "folder":
                var path = q["path"];
                if (path is not null && Directory.Exists(path)) Process.Start("explorer.exe", $"\"{path}\"");
                break;
            default:
                ShowWindow();
                break;
        }
    }

    Updater.Release? _update;

    /// <summary>Looks for a newer release; tells the user once per version. Returns the detailed result.</summary>
    public async Task<Updater.CheckResult> CheckForUpdateDetailed(bool asked)
    {
        if (!asked && !Settings.CheckUpdates) return new Updater.CheckResult(Updater.CheckStatus.UpToDate);
        var res = await Updater.Check(Settings.PreviewUpdates);
        if (res.Status == Updater.CheckStatus.Available && res.Release is { } found)
        {
            var announce = !asked && (_update is null || _update.Version != found.Version) &&
                           Settings.SkipVersion != found.Version.ToString();
            _update = found;
            if (announce) Shell.UpdateToast(found.Version.ToString(), found.Preview);
        }
        return res;
    }

    /// <summary>Looks for a newer release; tells the user once per version. Returns what it found.</summary>
    public async Task<Updater.Release?> CheckForUpdate(bool asked) => (await CheckForUpdateDetailed(asked)).Release;

    /// <summary>Downloads, verifies and runs the installer of the release found. Returns "" or why not.</summary>
    public async Task<string> InstallUpdate(IProgress<int>? progress = null)
    {
        var r = _update ?? await CheckForUpdate(true);
        if (r is null) return L.T("update.none");
        try
        {
            var why = await Updater.Install(r, progress);
            if (why != "") Shell.DoneToast("update", L.T("update.failed"), why, null);
            return why;
        }
        catch (Exception e)
        {
            Shell.DoneToast("update", L.T("update.failed"), e.Message, null);
            return e.Message;
        }
    }

    public void Answer(string id, bool accept)
    {
        Engine.Answer(id, accept);
        if (_requests.TryGetValue(id, out var r)) r.Status = accept ? L.T("req.receiving") : L.T("req.declined");
        if (!accept && _requests.Remove(id, out var gone)) Model.Requests.Remove(gone);
    }

    public void SendTo(Device d)
    {
        if (Model.Files.Count == 0) return;
        var id = Guid.NewGuid().ToString("N")[..12];
        _sendTarget[id] = d.Id;
        d.Busy = true;
        d.Status = L.T("send.asking");
        d.Progress = 0;
        Service.Touch();
        Engine.SendFiles(id, d.Id, Model.Files.ToList());
    }

    string _lastRadio = "", _lastWifi = "";
    WifiWindow? _wifiWindow;

    public void ShowWifi()
    {
        if (_wifiWindow is null)
        {
            _wifiWindow = new WifiWindow();
            _wifiWindow.Closed += (_, _) => _wifiWindow = null;
        }
        _wifiWindow.Show();
        _wifiWindow.Activate();
    }

    /// <summary>
    /// Networks that want a web sign-in first (hotels, campuses): after joining,
    /// ask Windows' connectivity probe; anything but its answer means a portal,
    /// which opens in the browser.
    /// </summary>
    async Task CheckPortal(string ssid)
    {
        await Task.Delay(TimeSpan.FromSeconds(6)); /* DHCP on the nekodrop adapter */
        try
        {
            using var http = new System.Net.Http.HttpClient(new System.Net.Http.HttpClientHandler { AllowAutoRedirect = false })
                { Timeout = TimeSpan.FromSeconds(8) };
            var r = await http.GetAsync("http://www.msftconnecttest.com/connecttest.txt");
            var body = r.IsSuccessStatusCode ? await r.Content.ReadAsStringAsync() : "";
            if (body.Trim() == "Microsoft Connect Test") return;
            Process.Start(new ProcessStartInfo("http://www.msftconnecttest.com/redirect") { UseShellExecute = true });
            Shell.DoneToast("portal", L.F("wifi.portal.title", ssid), L.T("wifi.portal.body"), null);
        }
        catch (Exception) { /* no answer at all: not a portal question */ }
    }


    void OnServiceState(JsonObject o)
    {
        var radio = (string?)o["radio"];
        // A radio (re)start recreates awdl0: the engine's sockets belong to the old
        // interface, so it starts over on the new one.
        if (radio == "on" && _lastRadio != "on" && _lastRadio != "" && Settings.Discoverable)
        {
            Engine.Dispose();
            StartEngine();
        }
        _lastRadio = radio ?? "";
        var wifiNow = (string?)o["wifi"] ?? "";
        if (wifiNow == "connected" && _lastWifi != "connected") _ = CheckPortal((string?)o["ssid"] ?? "");
        _lastWifi = wifiNow;
        var channel = (int?)o["channel"] ?? 0;
        var wifi = (string?)o["wifi"] ?? "";
        var mode = (string?)o["mode"];
        Model.Status = radio switch
        {
            "on" => L.F("status.on", channel),
            "starting" => L.T("status.starting"),
            "error" => L.T("status.error"),
            _ => mode == "off" ? L.T("status.off") : L.T("status.standby"),
        };
        Model.Detail = radio == "error" ? (string?)o["detail"] ?? "" :
            wifi == "" ? "" : wifi.StartsWith("unsupported") ? "Wi-Fi：" + wifi[13..] : "Wi-Fi：" + WifiText(wifi);
        if (_tray is not null) _tray.Text = "nekodrop – " + Model.Status;
    }

    static string WifiText(string s) => s switch
    {
        "connected" => L.T("wifi.connected"),
        "failed" => L.T("wifi.failed"),
        "waiting for a beacon" or "authenticating" or "associating" or "4-way handshake" => L.T("wifi.connecting"),
        _ => s,
    };

    void OnEngine(JsonObject o)
    {
        var ev = (string?)o["event"];
        var id = (string?)o["id"] ?? "";
        switch (ev)
        {
            case "ready":
                /* A new engine (the watchdog starts one over when the radio
                 * moves channel and re-creates awdl0) knows no peers and is not
                 * looking: the old one's devices go, and an open window looks
                 * again (#9: the send window stayed empty after a move to 149). */
                foreach (var stale in Model.Devices.Where(x => !x.Busy).ToList()) Model.Devices.Remove(stale);
                if (_send is not null || _window is not null) Engine.Look(true);
                break;
            case "receiving":
                /* Not receiving (ports or mDNS held, a socket error): the watchdog starts over after a while. */
                _notReceivingSince = (bool?)o["on"] == true ? null : _notReceivingSince ?? DateTime.UtcNow;
                break;
            case "peer":
            {
                var d = Model.Devices.FirstOrDefault(x => x.Id == id);
                if (d is null) Model.Devices.Add(d = new Device { Id = id });
                d.Name = (string?)o["name"] ?? L.T("dev.apple");
                d.Model = (string?)o["model"] ?? "";
                break;
            }
            case "peer_gone":
                var gone = Model.Devices.FirstOrDefault(x => x.Id == id);
                if (gone is not null && !gone.Busy) Model.Devices.Remove(gone);
                break;
            case "asked":
            {
                Service.Touch();
                var sender = (string?)o["sender"] ?? L.T("dev.someone");
                var model = (string?)o["model"] ?? "";
                var files = o["files"] as JsonArray;
                var what = Describe(files);
                if (Settings.AlwaysAccept.Contains($"{sender}|{model}")) { Engine.Answer(id, true); break; }
                var r = new Request { Id = id, Sender = sender, Model = model, What = what };
                _requests[id] = r;
                Model.Requests.Insert(0, r);
                Shell.AskToast(id, sender, what);
                break;
            }
            case "incoming":
                if (_requests.TryGetValue(id, out var inc))
                {
                    var total = (double?)o["total"] ?? 0;
                    inc.Progress = total > 0 ? ((double?)o["done"] ?? 0) / total : 0;
                    inc.Status = L.T("req.receiving");
                }
                Service.Touch();
                break;
            case "stored":
            {
                var folder = (string?)o["folder"] ?? Settings.SaveFolder;
                var from = _requests.TryGetValue(id, out var st) ? st.Sender : "AirDrop";
                if (st is not null) { _requests.Remove(id); Model.Requests.Remove(st); }
                if (o["files"] is JsonArray stored)
                    foreach (var f in stored)
                    {
                        var name = f is JsonObject fo ? (string?)fo["name"] ?? "" : (string?)f ?? "";
                        Model.ReceivedFiles.Insert(0, new Received { Name = name, Path = Path.Combine(folder, name), From = from });
                    }
                Shell.DoneToast(id, L.F("done.received", from), L.F("done.savedTo", folder), folder);
                break;
            }
            case "lost":
                if (_requests.Remove(id, out var lost))
                {
                    Model.Requests.Remove(lost);
                    Shell.DoneToast(id, L.F("done.lost", lost.Sender), (string?)o["reason"] ?? "", null);
                }
                break;
            case "send":
            {
                var target = _sendTarget.GetValueOrDefault(id);
                var d = Model.Devices.FirstOrDefault(x => x.Id == target);
                if (d is null) break;
                var state = (string?)o["state"];
                var total = (double?)o["total"] ?? 0;
                var done = (double?)o["done"] ?? (double?)o["sent"] ?? 0;
                d.Progress = total > 0 ? done / total : d.Progress;
                d.Status = state switch
                {
                    "asking" => L.T("send.asking"),
                    "sending" => L.F("send.sending", d.Progress),
                    "sent" or "done" => L.T("send.sent"),
                    "declined" => L.T("send.declined"),
                    "failed" => L.T("send.failed") + ((string?)o["reason"] ?? ""),
                    _ => state ?? "",
                };
                if (state is "sent" or "done" or "declined" or "failed")
                {
                    d.Busy = false;
                    if (state is "sent" or "done") Shell.DoneToast(id, L.F("done.sent", d.Name), L.F("done.items", Model.Files.Count), null);
                }
                Service.Touch();
                break;
            }
            case "looking":
                Model.Looking = (bool?)o["on"] ?? false;
                break;
            case "link":
                if ((bool?)o["up"] == true) Model.Detail = Model.Detail.Replace(L.T("err.engine"), "");
                break;
            case "fatal":
                Model.Detail = L.T("err.engineStopped") + ((string?)o["detail"] ?? (string?)o["reason"] ?? "");
                break;
        }
    }

    static string Describe(JsonArray? files)
    {
        if (files is null || files.Count == 0) return L.T("what.some");
        int photos = 0, videos = 0, other = 0;
        foreach (var f in files)
        {
            var type = (f as JsonObject)?["type"]?.ToString() ?? "";
            if (type.Contains("image") || type.Contains("jpeg") || type.Contains("png") || type.Contains("heic")) photos++;
            else if (type.Contains("movie") || type.Contains("video") || type.Contains("mpeg")) videos++;
            else other++;
        }
        var parts = new List<string>();
        if (photos > 0) parts.Add(L.F("what.photos", photos));
        if (videos > 0) parts.Add(L.F("what.videos", videos));
        if (other > 0) parts.Add(L.F("what.files", other));
        return string.Join(L.T("what.join"), parts);
    }
}
