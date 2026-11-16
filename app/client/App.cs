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
        if (!first)
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
        Shell.SetIdentity();
        try { Shell.Register(Settings.StartWithWindows); } catch (Exception) { }
        Resources.MergedDictionaries.Add(Theme.Build());
        CreateTray();
        Service.State += o => Dispatcher.BeginInvoke(() => OnServiceState(o));
        Service.Connected += up => Dispatcher.BeginInvoke(() =>
        {
            if (!up) Model.Status = "nekodrop 服务未运行";
        });
        Service.Start();
        Engine.Event += o => Dispatcher.BeginInvoke(() => OnEngine(o));
        if (Settings.Discoverable) StartEngine();
        _wake.AirDropNearby += () => Dispatcher.BeginInvoke(() =>
        {
            if ((string?)Service.Last?["mode"] == "ondemand") Service.Wake();
        });
        _wake.Start();
        _ = Task.Run(ListenForArgs);
        HandleArgs(_startArgs, fromStart: true);
    }

    void StartEngine()
    {
        if (Engine.Running) return;
        try { Identity.Ensure(); } catch (Exception e) { Model.Detail = "无法创建接收证书：" + e.Message; }
        if (!Engine.Start(Settings, out var error)) Model.Detail = "AirDrop 引擎无法启动：" + error;
    }

    void CreateTray()
    {
        _tray = new Forms.NotifyIcon
        {
            Text = "nekodrop",
            Icon = System.Drawing.Icon.ExtractAssociatedIcon(Environment.ProcessPath!) ?? System.Drawing.SystemIcons.Application,
            Visible = true,
        };
        _tray.MouseClick += (_, a) => { if (a.Button == Forms.MouseButtons.Left) ShowWindow(); };
        var menu = new Forms.ContextMenuStrip();
        menu.Items.Add("打开 nekodrop", null, (_, _) => ShowWindow());
        menu.Items.Add(new Forms.ToolStripSeparator());
        var receive = new Forms.ToolStripMenuItem("允许接收") { CheckOnClick = true, Checked = Settings.Discoverable };
        receive.CheckedChanged += (_, _) => SetDiscoverable(receive.Checked);
        menu.Items.Add(receive);
        var mode = new Forms.ToolStripMenuItem("联网方式");
        foreach (var (label, value) in new[] { ("同时联网和 AirDrop", "concurrent"), ("按需接管网卡", "ondemand"), ("关闭 AirDrop", "off") })
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
        menu.Items.Add("设置…", null, (_, _) => new SettingsWindow().Show());
        menu.Items.Add(new Forms.ToolStripSeparator());
        menu.Items.Add("退出", null, (_, _) => Quit());
        _tray.ContextMenuStrip = menu;
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
        _window ??= new MainWindow();
        _window.Closed += (_, _) => _window = null;
        _window.Show();
        if (_window.WindowState == WindowState.Minimized) _window.WindowState = WindowState.Normal;
        _window.Activate();
        Engine.Look(true);
    }

    public void WindowClosed() => Engine.Look(false);

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
                                                             PipeOptions.Asynchronous);
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
        foreach (var f in files) if (!Model.Files.Contains(f)) Model.Files.Add(f);
        if (files.Count > 0 || !(fromStart && args.Contains("--background"))) ShowWindow();
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
            case "folder":
                var path = q["path"];
                if (path is not null && Directory.Exists(path)) Process.Start("explorer.exe", $"\"{path}\"");
                break;
            default:
                ShowWindow();
                break;
        }
    }

    public void Answer(string id, bool accept)
    {
        Engine.Answer(id, accept);
        if (_requests.TryGetValue(id, out var r)) r.Status = accept ? "正在接收…" : "已拒绝";
        if (!accept && _requests.Remove(id, out var gone)) Model.Requests.Remove(gone);
    }

    public void SendTo(Device d)
    {
        if (Model.Files.Count == 0) return;
        var id = Guid.NewGuid().ToString("N")[..12];
        _sendTarget[id] = d.Id;
        d.Busy = true;
        d.Status = "正在等待对方接受…";
        d.Progress = 0;
        Service.Touch();
        Engine.SendFiles(id, d.Id, Model.Files.ToList());
    }

    void OnServiceState(JsonObject o)
    {
        var radio = (string?)o["radio"];
        var channel = (int?)o["channel"] ?? 0;
        var wifi = (string?)o["wifi"] ?? "";
        var mode = (string?)o["mode"];
        Model.Status = radio switch
        {
            "on" => $"AirDrop 已开启 · 信道 {channel}",
            "starting" => "正在启动 AirDrop…",
            "error" => "AirDrop 出错",
            _ => mode == "off" ? "AirDrop 已关闭" : "AirDrop 待命（按需开启）",
        };
        Model.Detail = radio == "error" ? (string?)o["detail"] ?? "" :
            wifi == "" ? "" : wifi.StartsWith("unsupported") ? "Wi-Fi：" + wifi[13..] : "Wi-Fi：" + WifiText(wifi);
        if (_tray is not null) _tray.Text = "nekodrop – " + Model.Status;
    }

    static string WifiText(string s) => s switch
    {
        "connected" => "已连接（与 AirDrop 同时使用）",
        "failed" => "连接失败，5 秒后重试",
        "waiting for a beacon" or "authenticating" or "associating" or "4-way handshake" => "正在连接…",
        _ => s,
    };

    void OnEngine(JsonObject o)
    {
        var ev = (string?)o["event"];
        var id = (string?)o["id"] ?? "";
        switch (ev)
        {
            case "peer":
            {
                var d = Model.Devices.FirstOrDefault(x => x.Id == id);
                if (d is null) Model.Devices.Add(d = new Device { Id = id });
                d.Name = (string?)o["name"] ?? "Apple 设备";
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
                var sender = (string?)o["sender"] ?? "某台设备";
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
                    inc.Status = "正在接收…";
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
                Shell.DoneToast(id, $"已收到来自 {from} 的文件", "已保存到 " + folder, folder);
                break;
            }
            case "lost":
                if (_requests.Remove(id, out var lost))
                {
                    Model.Requests.Remove(lost);
                    Shell.DoneToast(id, $"来自 {lost.Sender} 的传输未完成", (string?)o["reason"] ?? "", null);
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
                    "asking" => "正在等待对方接受…",
                    "sending" => $"正在发送 {d.Progress:P0}",
                    "sent" or "done" => "已发送",
                    "declined" => "对方拒绝了",
                    "failed" => "发送失败：" + ((string?)o["reason"] ?? ""),
                    _ => state ?? "",
                };
                if (state is "sent" or "done" or "declined" or "failed")
                {
                    d.Busy = false;
                    if (state is "sent" or "done") Shell.DoneToast(id, $"已发送到 {d.Name}", $"{Model.Files.Count} 个项目", null);
                }
                Service.Touch();
                break;
            }
            case "looking":
                Model.Looking = (bool?)o["on"] ?? false;
                break;
            case "link":
                if ((bool?)o["up"] == true) Model.Detail = Model.Detail.Replace("AirDrop 引擎无法启动", "");
                break;
            case "fatal":
                Model.Detail = "AirDrop 引擎停止：" + ((string?)o["detail"] ?? (string?)o["reason"] ?? "");
                break;
        }
    }

    static string Describe(JsonArray? files)
    {
        if (files is null || files.Count == 0) return "一些内容";
        int photos = 0, videos = 0, other = 0;
        foreach (var f in files)
        {
            var type = (f as JsonObject)?["type"]?.ToString() ?? "";
            if (type.Contains("image") || type.Contains("jpeg") || type.Contains("png") || type.Contains("heic")) photos++;
            else if (type.Contains("movie") || type.Contains("video") || type.Contains("mpeg")) videos++;
            else other++;
        }
        var parts = new List<string>();
        if (photos > 0) parts.Add($"{photos} 张照片");
        if (videos > 0) parts.Add($"{videos} 个视频");
        if (other > 0) parts.Add($"{other} 个文件");
        return string.Join("、", parts);
    }
}
