// SPDX-License-Identifier: GPL-2.0-only
using System.IO.Pipes;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using System.Text.Json.Nodes;

namespace NekoDrop.Service;

/// <summary>
/// \\.\pipe\nekodrop: one JSON object a line in each direction. Commands:
/// {"do":"status"}, {"do":"mode","mode":"concurrent|ondemand|off"},
/// {"do":"wake"}, {"do":"touch"}, {"do":"stop"}. Events: "state", "log".
/// Interactive users and SYSTEM may connect; nothing from the network can.
/// </summary>
public sealed class ControlPipe(Supervisor sup)
{
    const string Name = "nekodrop";
    readonly List<StreamWriter> _clients = [];

    public async Task Run(CancellationToken ct)
    {
        sup.Changed += Broadcast;
        sup.Log += line => Broadcast(new JsonObject { ["event"] = "log", ["line"] = line });
        while (!ct.IsCancellationRequested)
        {
            var server = Create();
            try { await server.WaitForConnectionAsync(ct); }
            catch (OperationCanceledException) { server.Dispose(); break; }
            _ = Task.Run(() => Serve(server, ct), ct);
        }
    }

    static NamedPipeServerStream Create()
    {
        var security = new PipeSecurity();
        security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.InteractiveSid, null),
            PipeAccessRights.ReadWrite, AccessControlType.Allow));
        security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
            PipeAccessRights.FullControl, AccessControlType.Allow));
        security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.NetworkSid, null),
            PipeAccessRights.FullControl, AccessControlType.Deny));
        return NamedPipeServerStreamAcl.Create(Name, PipeDirection.InOut, NamedPipeServerStream.MaxAllowedServerInstances,
            PipeTransmissionMode.Byte, PipeOptions.Asynchronous, 4096, 4096, security);
    }

    async Task Serve(NamedPipeServerStream pipe, CancellationToken ct)
    {
        using var _ = pipe;
        var reader = new StreamReader(pipe, Encoding.UTF8);
        var writer = new StreamWriter(pipe, new UTF8Encoding(false)) { AutoFlush = true };
        lock (_clients) _clients.Add(writer);
        try
        {
            await Send(writer, sup.Snapshot());
            while (!ct.IsCancellationRequested)
            {
                var line = await reader.ReadLineAsync(ct);
                if (line is null) break;
                JsonObject? cmd;
                try { cmd = JsonNode.Parse(line) as JsonObject; }
                catch (System.Text.Json.JsonException) { continue; }
                switch ((string?)cmd?["do"])
                {
                    case "status": await Send(writer, sup.Snapshot()); break;
                    case "wake": sup.Wake(); break;
                    case "touch": sup.Touch(); break;
                    case "stop": sup.Stop("asked by the client", giveBack: true); break;
                    case "mode":
                        if (Enum.TryParse<NetMode>((string?)cmd?["mode"], true, out var m)) sup.SetMode(m);
                        break;
                    case "wifi-scan": sup.WifiScan((string?)cmd?["ssid"]); break;
                    case "wifi-connect":
                    {
                        var why = sup.WifiConnect((string?)cmd?["ssid"] ?? "", (string?)cmd?["password"],
                                                  (bool?)cmd?["auto"], (bool?)cmd?["hidden"] ?? false);
                        await Send(writer, new JsonObject { ["event"] = "wifi-connect", ["ssid"] = (string?)cmd?["ssid"], ["error"] = why });
                        break;
                    }
                    case "wifi-disconnect": sup.WifiDisconnect(); break;
                    case "wifi-forget": sup.WifiForget((string?)cmd?["ssid"] ?? ""); break;
                    case "wifi-auto": sup.WifiAuto((string?)cmd?["ssid"] ?? "", (bool?)cmd?["on"] ?? true); break;
                }
            }
        }
        catch (IOException) { }
        catch (OperationCanceledException) { }
        finally { lock (_clients) _clients.Remove(writer); }
    }

    static async Task Send(StreamWriter w, JsonObject o)
    {
        try { await w.WriteLineAsync(o.ToJsonString()); }
        catch (IOException) { }
        catch (ObjectDisposedException) { }
    }

    void Broadcast(JsonObject o)
    {
        StreamWriter[] all;
        lock (_clients) all = [.. _clients];
        foreach (var w in all) _ = Send(w, (JsonObject)o.DeepClone());
    }
}
