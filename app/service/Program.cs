// SPDX-License-Identifier: GPL-2.0-only
// NekoDropService: runs as LocalSystem, so taking the Wi-Fi card, locking DMA
// memory, creating the AWDL adapter and reading the Wi-Fi profile need no UAC.
using NekoDrop.Service;

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
