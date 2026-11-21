// SPDX-License-Identifier: GPL-2.0-only
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json.Nodes;

namespace NekoDrop;

/// <summary>
/// Software update from the project's GitHub releases: finds a newer
/// nekodrop-X.Y.Z-setup.exe, downloads it, checks its SHA-256 against the
/// digest GitHub publishes for the asset, and runs it (Windows asks for
/// administrator rights). Nothing installs without the user choosing to.
/// </summary>
public static class Updater
{
    const string ReleasesApi = "https://api.github.com/repos/zheznanohana/nekodrop/releases?per_page=15";

    public sealed record Release(Version Version, string Tag, string Url, string Sha256, bool Preview, string Page);

    static readonly HttpClient Http = MakeClient();

    static HttpClient MakeClient()
    {
        var c = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        c.DefaultRequestHeaders.UserAgent.ParseAdd("nekodrop-updater");
        c.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        return c;
    }

    /// <summary>This build's version (0.1.16 of "0.1.16+commit").</summary>
    public static Version Current
    {
        get
        {
            var info = Assembly.GetEntryAssembly()?.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
            var text = (info ?? "0.0.0").Split('+')[0];
            return Version.TryParse(text, out var v) ? v : new Version(0, 0, 0);
        }
    }

    /// <summary>The newest release above this build with an installer and a digest; null if none.</summary>
    public static async Task<Release?> FindNewer(bool includePreview)
    {
        var json = await Http.GetStringAsync(ReleasesApi);
        Release? best = null;
        foreach (var r in JsonNode.Parse(json)?.AsArray() ?? [])
        {
            if (r is null || (bool?)r["draft"] == true) continue;
            var preview = (bool?)r["prerelease"] == true;
            if (preview && !includePreview) continue;
            var tag = (string?)r["tag_name"] ?? "";
            if (!Version.TryParse(tag.TrimStart('v'), out var v) || v <= Current) continue;
            foreach (var a in r["assets"]?.AsArray() ?? [])
            {
                var name = (string?)a?["name"] ?? "";
                var digest = (string?)a?["digest"] ?? "";
                if (!name.StartsWith("nekodrop-", StringComparison.Ordinal) || !name.EndsWith("-setup.exe", StringComparison.Ordinal)) continue;
                // An installer without a published digest is never run.
                if (!digest.StartsWith("sha256:", StringComparison.Ordinal)) continue;
                var url = (string?)a?["browser_download_url"] ?? "";
                if (!url.StartsWith("https://github.com/zheznanohana/nekodrop/releases/download/", StringComparison.Ordinal)) continue;
                if (best is null || v > best.Version)
                    best = new Release(v, tag, url, digest["sha256:".Length..].ToLowerInvariant(), preview, (string?)r["html_url"] ?? "");
            }
        }
        return best;
    }

    /// <summary>Downloads, verifies and starts the installer. Returns "" or why not.</summary>
    public static async Task<string> Install(Release r, IProgress<int>? progress = null)
    {
        var dir = Path.Combine(Path.GetTempPath(), "nekodrop-update");
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, $"nekodrop-{r.Version}-setup.exe");
        using (var resp = await Http.GetAsync(r.Url, HttpCompletionOption.ResponseHeadersRead))
        {
            resp.EnsureSuccessStatusCode();
            var total = resp.Content.Headers.ContentLength ?? 0;
            await using var src = await resp.Content.ReadAsStreamAsync();
            await using var dst = File.Create(path);
            var buf = new byte[1 << 16];
            long done = 0;
            int n;
            while ((n = await src.ReadAsync(buf)) > 0)
            {
                await dst.WriteAsync(buf.AsMemory(0, n));
                done += n;
                if (total > 0) progress?.Report((int)(done * 100 / total));
            }
        }
        string hash;
        await using (var f = File.OpenRead(path))
            hash = Convert.ToHexString(await SHA256.HashDataAsync(f)).ToLowerInvariant();
        if (hash != r.Sha256)
        {
            File.Delete(path);
            return "the download does not match the published SHA-256";
        }
        // The installer closes this app itself (CloseApplications) and starts it again afterwards.
        Process.Start(new ProcessStartInfo(path, "/SILENT /NORESTART") { UseShellExecute = true });
        return "";
    }
}
