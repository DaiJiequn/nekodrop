// SPDX-License-Identifier: GPL-2.0-only
// NekoDropService: runs as LocalSystem, so taking the Wi-Fi card, locking DMA
// memory, creating the AWDL adapter and reading the Wi-Fi profile need no UAC.
using NekoDrop.Service;

// --give-back: hand the card to Intel's driver and exit (uninstall, recovery).
if (args.Contains("--give-back"))
{
    using var s = new Supervisor();
    s.Stop("--give-back", giveBack: true);
    // The process ends here: wait for Intel's interface and restore in line.
    WlanBackup.Restore(Console.WriteLine, TimeSpan.FromSeconds(60));
    return;
}

// --status: what the service would work with (no keys are read or printed).
if (args.Contains("--status"))
{
    var card = Card.Find();
    Console.WriteLine(card is null ? "card: none found"
        : $"card: {card.InstanceId}{Environment.NewLine}  driver service {card.Service}, INF {card.InfPath}, taken by nekodrop: {Card.TakenByProxy(card)}");
    var net = Wlan.Current(out var why, readKey: false);
    Console.WriteLine(net is null ? $"wifi: {why}"
        : $"wifi: \"{net.Ssid}\" {net.Bssid} channel {net.Channel}, {net.Security}{(why == "" ? "" : " - " + why)}");
    var cfg = ServiceConfig.Load();
    Console.WriteLine($"mode {cfg.Mode}, start channel {cfg.StartChannel}, country \"{cfg.Country}\", peer channel {cfg.PeerChannel}");
    return;
}

// The service owns the card and reads the Wi-Fi key: only as LocalSystem.
if (!System.Security.Principal.WindowsIdentity.GetCurrent().IsSystem)
{
    Console.Error.WriteLine("NekoDropService runs as a Windows service (LocalSystem). Use --status or --give-back here.");
    return;
}

var builder = Host.CreateApplicationBuilder(args);
builder.Services.AddWindowsService(o => o.ServiceName = "NekoDropService");
builder.Services.AddSingleton<Supervisor>();
builder.Services.AddHostedService<Worker>();
builder.Build().Run();

sealed class Worker(Supervisor sup) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        sup.Boot();
        await new ControlPipe(sup).Run(stoppingToken);
    }

    public override Task StopAsync(CancellationToken cancellationToken)
    {
        sup.Dispose();
        return base.StopAsync(cancellationToken);
    }
}
