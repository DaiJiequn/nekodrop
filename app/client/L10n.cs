// SPDX-License-Identifier: GPL-2.0-only
using System.Globalization;
using System.Windows.Markup;

namespace NekoDrop;

/// <summary>
/// Chinese and English. The language follows Windows (zh-* gets Chinese,
/// anything else English) unless Settings.Language picks one. XAML uses
/// {local:T key}; code uses L.T("key") or L.F("key", args).
/// </summary>
public static class L
{
    public static bool Zh { get; private set; } = Detect("auto");

    static bool Detect(string choice) => choice switch
    {
        "zh" => true,
        "en" => false,
        _ => CultureInfo.CurrentUICulture.Name.StartsWith("zh", StringComparison.OrdinalIgnoreCase),
    };

    public static void Use(string choice) => Zh = Detect(choice);

    public static string T(string key) =>
        Strings.TryGetValue(key, out var s) ? (Zh ? s.Zh : s.En) : key;

    public static string F(string key, params object[] args) => string.Format(T(key), args);

    static readonly Dictionary<string, (string Zh, string En)> Strings = new()
    {
        // Explorer, notifications
        ["verb"] = ("用 nekodrop 发送", "Send with nekodrop"),
        ["ask.title"] = ("{0} 想通过 AirDrop 发送", "{0} wants to AirDrop"),
        ["accept"] = ("接受", "Accept"),
        ["decline"] = ("拒绝", "Decline"),
        ["done.received"] = ("已收到来自 {0} 的文件", "Received from {0}"),
        ["done.savedTo"] = ("已保存到 {0}", "Saved to {0}"),
        ["done.lost"] = ("来自 {0} 的传输未完成", "The transfer from {0} did not finish"),
        ["done.sent"] = ("已发送到 {0}", "Sent to {0}"),
        ["done.items"] = ("{0} 个项目", "{0} item(s)"),

        // Tray
        ["tray.open"] = ("打开 nekodrop", "Open nekodrop"),
        ["tray.receive"] = ("允许接收", "Allow receiving"),
        ["tray.mode"] = ("联网方式", "Wi-Fi and AirDrop"),
        ["mode.concurrent.short"] = ("同时联网和 AirDrop", "Wi-Fi and AirDrop together"),
        ["mode.ondemand.short"] = ("按需接管网卡", "Take the card on demand"),
        ["mode.off.short"] = ("关闭 AirDrop", "AirDrop off"),
        ["tray.settings"] = ("设置…", "Settings…"),
        ["tray.report"] = ("生成诊断报告…", "Diagnostic report…"),
        ["tray.quit"] = ("退出", "Quit"),

        // Status
        ["status.connecting"] = ("正在连接 nekodrop 服务…", "Connecting to the nekodrop service…"),
        ["status.noService"] = ("nekodrop 服务未运行", "The nekodrop service is not running"),
        ["status.on"] = ("AirDrop 已开启 · 信道 {0}", "AirDrop on · channel {0}"),
        ["status.starting"] = ("正在启动 AirDrop…", "Starting AirDrop…"),
        ["status.error"] = ("AirDrop 出错", "AirDrop error"),
        ["status.off"] = ("AirDrop 已关闭", "AirDrop is off"),
        ["status.standby"] = ("AirDrop 待命（按需开启）", "AirDrop on standby (on demand)"),
        ["wifi.connected"] = ("已连接（与 AirDrop 同时使用）", "connected (alongside AirDrop)"),
        ["wifi.failed"] = ("连接失败，5 秒后重试", "could not connect, retrying in 5 s"),
        ["wifi.connecting"] = ("正在连接…", "connecting…"),
        ["wifi.needConcurrent"] = ("只有「同时联网和 AirDrop」模式能在这里连接 Wi-Fi。当前模式下请在设置里切换模式，或在 AirDrop 空闲时用 Windows 自带的 Wi-Fi 连接。", "Joining Wi-Fi here needs the \"Wi-Fi and AirDrop together\" mode. Switch the mode in Settings, or use Windows' own Wi-Fi while AirDrop is idle."),
        ["err.identity"] = ("无法创建接收证书：", "Cannot create the receiver certificate: "),
        ["err.engine"] = ("AirDrop 引擎无法启动：", "The AirDrop engine could not start: "),
        ["err.engineStopped"] = ("AirDrop 引擎停止：", "The AirDrop engine stopped: "),

        // Devices and transfers
        ["dev.apple"] = ("Apple 设备", "Apple device"),
        ["dev.someone"] = ("某台设备", "A device"),
        ["req.waiting"] = ("等待你的回应", "Waiting for you"),
        ["req.receiving"] = ("正在接收…", "Receiving…"),
        ["req.declined"] = ("已拒绝", "Declined"),
        ["send.asking"] = ("正在等待对方接受…", "Waiting for them to accept…"),
        ["send.sending"] = ("正在发送 {0:P0}", "Sending {0:P0}"),
        ["send.sent"] = ("已发送", "Sent"),
        ["send.declined"] = ("对方拒绝了", "Declined"),
        ["send.failed"] = ("发送失败：", "Failed: "),
        ["what.some"] = ("一些内容", "something"),
        ["what.photos"] = ("{0} 张照片", "{0} photo(s)"),
        ["what.videos"] = ("{0} 个视频", "{0} video(s)"),
        ["what.files"] = ("{0} 个文件", "{0} file(s)"),
        ["what.join"] = ("、", ", "),

        // Main window
        ["main.wants"] = (" 想发送 ", " wants to send "),
        ["main.nearby"] = ("附近的设备", "Nearby devices"),
        ["main.looking"] = ("正在查找…  在 iPhone 上把 AirDrop 设为“所有人（10 分钟）”，它就会出现在这里。",
                            "Looking…  Set AirDrop on the iPhone to “Everyone for 10 Minutes” and it shows up here."),
        ["main.deviceTip"] = ("点击发送下面的文件", "Click to send the files below"),
        ["main.files"] = ("要发送的文件", "Files to send"),
        ["main.add"] = ("添加文件…", "Add files…"),
        ["main.clear"] = ("清空", "Clear"),
        ["main.dropHint"] = ("把文件拖到这里，或在资源管理器里右键 → “用 nekodrop 发送”",
                             "Drop files here, or right-click them in Explorer → “Send with nekodrop”"),
        ["main.received"] = ("最近收到", "Recently received"),
        ["main.from"] = ("来自 ", "from "),
        ["main.saveTo"] = ("收到的文件保存在", "Received files are saved in"),
        ["main.change"] = ("更改…", "Change…"),
        ["main.openFolder"] = ("打开接收文件夹", "Open the folder"),
        ["main.settings"] = ("设置", "Settings"),
        ["main.report"] = ("诊断报告", "Diagnostic report"),
        ["report.missing"] = ("找不到诊断脚本 report.ps1，请重新安装 nekodrop。", "report.ps1 is missing; reinstall nekodrop."),
        ["pick.files"] = ("选择要通过 AirDrop 发送的文件", "Choose files to AirDrop"),
        ["pick.folder"] = ("选择接收文件的保存位置", "Choose where received files are saved"),

        // Send window
        ["send.title"] = ("用 AirDrop 发送", "AirDrop"),
        ["send.choose"] = ("选择要发送到的设备", "Choose who to send to"),
        ["send.none"] = ("正在查找附近的设备…\n在 iPhone 上打开“控制中心 → AirDrop → 所有人（10 分钟）”。",
                         "Looking for nearby devices…\nOn the iPhone: Control Center → AirDrop → Everyone for 10 Minutes."),
        ["send.more"] = ("打开 nekodrop", "Open nekodrop"),
        ["send.close"] = ("关闭", "Close"),
        ["send.summary"] = ("{0} 等 {1} 项", "{0} and {2} more"),

        // Wi-Fi window
        ["wifi.title"] = ("Wi-Fi（nekodrop）", "Wi-Fi (nekodrop)"),
        ["wifi.tray"] = ("Wi-Fi…", "Wi-Fi…"),
        ["wifi.refresh"] = ("刷新", "Refresh"),
        ["wifi.disconnect"] = ("断开", "Disconnect"),
        ["wifi.nearby"] = ("附近的网络", "Networks nearby"),
        ["wifi.empty"] = ("还没有扫描结果。点“刷新”。", "No scan results yet. Click Refresh."),
        ["wifi.hidden"] = ("连接隐藏的网络…", "Connect to a hidden network…"),
        ["wifi.ssid"] = ("网络名称（SSID）", "Network name (SSID)"),
        ["wifi.password"] = ("密码", "Password"),
        ["wifi.connect"] = ("连接", "Connect"),
        ["wifi.join"] = ("加入", "Join"),
        ["wifi.connected"] = ("已连接", "Connected"),
        ["wifi.saved"] = ("已保存的网络", "Saved networks"),
        ["wifi.savedTag"] = ("已保存", "saved"),
        ["wifi.forget"] = ("忘记", "Forget"),
        ["wifi.auto"] = ("自动连接", "Auto"),
        ["wifi.autoConnect"] = ("自动连接", "Connect automatically"),
        ["wifi.note"] = ("nekodrop 接管网卡时，Windows 的 Wi-Fi 列表不可用，在这里管理 Wi-Fi；上网走名为 nekodrop 的网卡。",
                         "While nekodrop drives the card, Windows' Wi-Fi list is unavailable: manage Wi-Fi here. Traffic goes through the adapter named nekodrop."),
        ["wifi.notConcurrent"] = ("当前不是“同时联网和 AirDrop”模式", "Not in Wi-Fi and AirDrop together mode"),
        ["wifi.notConcurrentHint"] = ("在设置里选择该模式后，这里可以管理 Wi-Fi。", "Choose that mode in Settings to manage Wi-Fi here."),
        ["wifi.connectedTo"] = ("已连接到 {0}", "Connected to {0}"),
        ["wifi.connectingTo"] = ("正在连接 {0}", "Connecting to {0}"),
        ["wifi.notConnected"] = ("未连接", "Not connected"),
        ["wifi.pick"] = ("从下面选择一个网络。", "Pick a network below."),
        ["wifi.radioOff"] = ("nekodrop 的无线电未开启。", "nekodrop's radio is not on."),
        ["wifi.gettingAddress"] = ("正在获取 IP 地址…", "Getting an address…"),
        ["wifi.address"] = ("IP {0} · 网关 {1}", "IP {0} · gateway {1}"),
        ["wifi.scanning"] = ("正在扫描…", "Scanning…"),
        ["wifi.sec.open"] = ("开放", "Open"),
        ["wifi.sec.owe"] = ("增强型开放（OWE）", "Enhanced Open (OWE)"),
        ["wifi.sec.enterprise"] = ("企业（802.1X）", "Enterprise (802.1X)"),
        ["wifi.portal.title"] = ("{0} 需要登录", "{0} needs you to sign in"),
        ["wifi.portal.body"] = ("已在浏览器中打开登录页面。", "The sign-in page is open in your browser."),

        // Settings
        ["set.title"] = ("nekodrop 设置", "nekodrop settings"),
        ["set.heading"] = ("设置", "Settings"),
        ["set.name"] = ("设备名称", "Device name"),
        ["set.nameHint"] = ("iPhone 的 AirDrop 列表里显示的名字", "The name iPhones show in their AirDrop list"),
        ["set.folder"] = ("保存位置", "Save received files to"),
        ["set.ask"] = ("接收前先询问我", "Ask me before receiving"),
        ["set.network"] = ("网络与 AirDrop", "Wi-Fi and AirDrop"),
        ["mode.concurrent"] = ("同时联网和 AirDrop（推荐）", "Wi-Fi and AirDrop together (recommended)"),
        ["mode.concurrent.hint"] = ("nekodrop 自己连接你当前的 Wi-Fi，同时保持 AirDrop 可用。支持 WPA2 个人版和开放网络；WPA3、企业网会自动改用“按需”。",
                                    "nekodrop joins your current Wi-Fi itself and keeps AirDrop available. Works with WPA2-Personal and open networks; WPA3 and enterprise networks fall back to on demand."),
        ["mode.ondemand"] = ("按需接管网卡", "Take the card on demand"),
        ["mode.ondemand.hint"] = ("平时由 Intel 驱动正常上网；附近的 iPhone 打开 AirDrop 或你发送文件时才接管网卡，传输期间会短暂断网。",
                                  "Intel's driver runs Wi-Fi as usual; nekodrop takes the card when an iPhone nearby opens AirDrop or you send something, and Wi-Fi drops during the transfer."),
        ["mode.off"] = ("关闭 AirDrop", "AirDrop off"),
        ["set.trusted"] = ("总是接受的设备", "Always accept from"),
        ["set.trustedHint"] = ("这些设备发来的文件不再询问。设备只按它自报的名称和型号识别。",
                               "Files from these devices are accepted without asking. A device is known only by the name and model it gives."),
        ["set.remove"] = ("移除所选", "Remove"),
        ["set.startup"] = ("登录 Windows 时启动 nekodrop", "Start nekodrop when I sign in"),
        ["set.card"] = ("网卡", "Wi-Fi card"),
        ["set.card.detected"] = ("自动识别：{0}", "Detected: {0}"),
        ["set.card.manual"] = ("手动选择：{0}", "Chosen by hand: {0}"),
        ["set.card.none"] = ("没有自动识别出支持的网卡。可以在下面手动选择网卡和型号。", "No supported card was detected. You can choose the card and its model below."),
        ["set.card.hint"] = ("只在自动识别失败时使用。选错型号可能无法启动 AirDrop，点「自动识别」即可恢复。", "Only for a card that is not detected. A wrong model may keep AirDrop from starting; choose Automatic to undo."),
        ["set.card.apply"] = ("使用这个型号", "Use this model"),
        ["set.card.auto"] = ("自动识别", "Automatic"),
        ["set.card.applied"] = ("已保存，AirDrop 会重启几秒", "Saved; AirDrop restarts for a few seconds"),
        ["set.card.mismatch"] = ("这个型号和网卡的厂商对不上", "This model does not fit this card's vendor"),
        ["set.card.pick"] = ("请先选网卡和型号", "Choose a card and a model first"),
        ["set.card.nolist"] = ("没有找到 Intel 或 Realtek 的网卡", "No Intel or Realtek network card found"),
        ["set.card.recognized"] = ("已识别", "detected"),
        ["set.card.unrecognized"] = ("未识别", "not detected"),
        ["set.experimental"] = ("实验性功能", "Experimental"),
        ["set.aggregate"] = ("聚合发送（A-MPDU）", "Aggregated sending (A-MPDU)"),
        ["set.aggregate.hint"] = ("大文件发给 iPhone 时和它协商 Block Ack，让网卡把多帧打包成一次发出，并在 iPhone 离开信道时暂停，可能快很多。照 ForbiddenItlwm 的做法移植，还没在真机上测过；发送出问题请关掉。切换时 AirDrop 会重启几秒。",
                                  "For large sends to an iPhone, sets up Block Ack with it so the card sends many frames in one burst, and pauses while the iPhone is off our channel. Can be much faster. Ported from ForbiddenItlwm and not yet tested on hardware: turn it off if sending misbehaves. AirDrop restarts for a few seconds when this changes."),
        ["set.coexPlan"] = ("Wi-Fi 和 AirDrop 不在同一信道时按苹果的方式分时", "Apple-style time sharing when Wi-Fi and AirDrop use different channels"),
        ["set.coexPlan.hint"] = ("只在 nekodrop 连着 5 GHz Wi-Fi、而且和 AirDrop 不在同一个信道时起作用。照 macOS 的做法，把路由器写进 AirDrop 的 16 个时隙里（平时 4 个，传输时 1–6 个，按路由器流量决定），这些时隙里电脑不发 AirDrop 帧，让 iPhone 知道电脑什么时候在路由器那边。移植自 ForbiddenItlwm PR #30，提速效果还没有实测；传输变差请关掉。切换时 AirDrop 会重启几秒。",
                                 "Only when nekodrop is on 5 GHz Wi-Fi on another channel than AirDrop. As macOS does, the router goes into some of AirDrop's 16 schedule entries (4 when idle, 1 to 6 while transferring, by the router's traffic), and the PC sends no AirDrop frames in them, so the iPhone knows when the PC is with the router. Ported from ForbiddenItlwm PR #30; the speed benefit is not yet measured. Turn it off if transfers get worse. AirDrop restarts for a few seconds when this changes."),
        ["set.fastUnlock"] = ("更快解锁被动 5 GHz 信道", "Faster unlock of a passive 5 GHz channel"),
        ["set.fastUnlock.hint"] = ("网卡不接受国家码时，5 GHz AirDrop 信道开头是被动的，听到 iPhone 或附近路由器之前电脑一帧都不发，iPhone 也就看不到这台电脑。打开后会更早、更频繁（1.5、12、30 秒）地只扫 11 个常用信道，让网卡按自己的规则从附近路由器学到国家码。不会在被动信道上强行发射。还在测试；切换时 AirDrop 会重启几秒。",
                                   "Where the card refuses the country, the 5 GHz AirDrop channel starts passive: the PC sends nothing until it hears an iPhone or a nearby router, so iPhones cannot see it. This scans sooner and more often (at 1.5, 12 and 30 s), only the 11 common channels, so the card learns the country from nearby routers by its own rules. It never transmits on a passive channel. Still being tested. AirDrop restarts for a few seconds when this changes."),
        ["set.btCoex"] = ("蓝牙与 Wi-Fi 均衡共存（Intel）", "Balanced Bluetooth coexistence (Intel)"),
        ["set.btCoex.hint"] = ("发送时，电脑用网卡自带的蓝牙发出唤醒信号，附近的 iPhone 和 Mac 才会出现。默认固件让 Wi-Fi 一直优先，AirDrop 监听时蓝牙几乎发不出去，可能要等很久甚至找不到设备。打开后固件均衡分配，在一台 AX200 上设备几秒内就出现了。和 ForbiddenItlwm 的 itlawdlbtcoex=1 相同。还在测试；切换时 AirDrop 会重启几秒。",
                                "To send, the PC wakes nearby iPhones and Macs with a Bluetooth beacon from the card's own Bluetooth. By default the firmware always favours Wi-Fi, so while AirDrop listens that beacon barely gets out and devices can take long to appear, or never do. This balances the two; on one AX200 devices then appeared within seconds. Same as ForbiddenItlwm's itlawdlbtcoex=1. Still being tested. AirDrop restarts for a few seconds when this changes."),
        ["set.updates"] = ("更新", "Updates"),
        ["set.checkUpdates"] = ("自动检查新版本", "Check for new versions automatically"),
        ["set.previewUpdates"] = ("包括预览版", "Include preview releases"),
        ["set.updates.hint"] = ("只提示，不会自行安装。安装前会核对 GitHub 公布的 SHA-256。", "Only tells you; never installs on its own. The download is checked against the SHA-256 GitHub publishes."),
        ["set.checkNow"] = ("检查更新", "Check now"),
        ["update.title"] = ("nekodrop {0} 可用", "nekodrop {0} is available"),
        ["update.body"] = ("点「更新」下载并安装，AirDrop 会重启一会儿。", "Choose Update to download and install it; AirDrop restarts for a moment."),
        ["update.bodyPreview"] = ("预览版。点「更新」下载并安装，AirDrop 会重启一会儿。", "A preview release. Choose Update to download and install it; AirDrop restarts for a moment."),
        ["update.now"] = ("更新", "Update"),
        ["update.later"] = ("以后", "Later"),
        ["update.checking"] = ("正在检查更新…", "Checking for updates…"),
        ["update.none"] = ("已经是最新版本", "You have the latest version"),
        ["update.found"] = ("发现 {0}，正在下载…", "Found {0}, downloading…"),
        ["update.failed"] = ("更新失败", "Update failed"),
        ["update.checkFailed"] = ("检查更新失败", "Update check failed"),
        ["update.rateLimited"] = ("GitHub API 限流（代理共享配额），请稍后再试或手动下载", "GitHub API rate-limited (shared proxy IP); try later or download manually"),
        ["update.rateLimitedFound"] = ("发现 {0}，但 GitHub API 限流，请前往下载页手动安装", "Found {0}, but GitHub API is rate-limited; download from Releases"),
        ["update.openReleases"] = ("打开下载页", "Open Releases"),
        ["set.language"] = ("语言 / Language", "Language / 语言"),
        ["lang.auto"] = ("跟随系统", "Same as Windows"),
        ["set.save"] = ("保存", "Save"),
        ["set.cancel"] = ("取消", "Cancel"),
    };
}

/// <summary>{local:T key} in XAML.</summary>
[MarkupExtensionReturnType(typeof(string))]
public sealed class TExtension(string key) : MarkupExtension
{
    public TExtension() : this("") { }
    public string Key { get; set; } = key;
    public override object ProvideValue(IServiceProvider serviceProvider) => L.T(Key);
}
