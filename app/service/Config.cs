// SPDX-License-Identifier: GPL-2.0-only
using System.Text.Json;
using System.Text.Json.Serialization;

namespace NekoDrop.Service;

/// <summary>How Wi-Fi and AirDrop share the card.</summary>
public enum NetMode
{
    /// <summary>Our driver joins the Wi-Fi network beside AWDL (WPA2-Personal or open).</summary>
    Concurrent,
    /// <summary>Intel's driver keeps Wi-Fi; the card is taken only while AirDrop is in use.</summary>
    OnDemand,
    /// <summary>The card always stays with Intel's driver.</summary>
    Off,
}

/// <summary>Machine-wide settings, in %ProgramData%\nekodrop\service.json.</summary>
public sealed class ServiceConfig
{
    [JsonConverter(typeof(JsonStringEnumConverter))]
    public NetMode Mode { get; set; } = NetMode.Concurrent;
    /// <summary>The AWDL channel to start on; the peer's channel is followed from there.</summary>
    public int StartChannel { get; set; } = 6;
    /// <summary>Last country learned from a peer or a network, "" if none.</summary>
    public string Country { get; set; } = "";
    /// <summary>Last 5 GHz channel a peer used (44 or 149), 0 if none.</summary>
    public int PeerChannel { get; set; }
    /// <summary>Seconds without AirDrop use before on-demand mode gives the card back.</summary>
    public int IdleSeconds { get; set; } = 90;
    /// <summary>Intel's own INF (oemNN.inf), remembered when the card is first taken.</summary>
    public string VendorInf { get; set; } = "";

    public static string Folder =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "nekodrop");
    static string FilePath => Path.Combine(Folder, "service.json");
    static readonly JsonSerializerOptions Options = new() { WriteIndented = true };

    public static ServiceConfig Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<ServiceConfig>(File.ReadAllText(FilePath), Options) ?? new();
        }
        catch (Exception) { }
        return new();
    }

    public void Save()
    {
        Directory.CreateDirectory(Folder);
        var tmp = FilePath + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(this, Options));
        File.Move(tmp, FilePath, true);
    }
}
