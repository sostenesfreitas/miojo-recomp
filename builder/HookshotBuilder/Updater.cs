using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows;

namespace HookshotBuilder;

/// The game's update offer (host/hos_update.c) starts this app with --update <game folder> --tag <release>. When these tools are
/// not that release, the release's HookshotBuilder.zip is downloaded and unpacked beside this app, and the new app is started with
/// --fetched to build the folder; when they are, the folder is built again here (nx2/make_game.py --update: from the game files the
/// folder keeps).
static class Updater
{
    const string Latest = "https://api.github.com/repos/NotRexed/Hookshot/releases/latest";
    const string Asset = "HookshotBuilder.zip";
    const string SetupAsset = "HookshotBuilder.exe";            // (the setup, builder/setup/setup.c: a release holds it and its list)
    const string SetupList = "hookshot-setup.txt";              // (scripts/make_setup_parts.py)
    static readonly TimeSpan[] RetryDelays = { TimeSpan.FromSeconds(3), TimeSpan.FromSeconds(6), TimeSpan.FromSeconds(12), TimeSpan.FromSeconds(20) };

    public static void FromCommandLine(MainWindow w)
    {
        var a = Environment.GetCommandLineArgs();
        string? folder = null, tag = null;
        var fetched = false;
        for (var i = 1; i < a.Length; i++)
        {
            if (a[i] == "--update" && i + 1 < a.Length) folder = a[++i];
            else if (a[i] == "--tag" && i + 1 < a.Length) tag = a[++i];
            else if (a[i] == "--fetched") fetched = true;
        }
        if (folder == null) return;
        UpdateFolder(w, folder, tag, fetched);
    }

    // ---- the update tracks (the Track menu; by default the track of these tools). host/hos_update.c and
    // builder/linux/linux_update.py have the same rules ----
    public const string Release = "release", Candidate = "rc", Nightly = "nightly";
    const string Releases = "https://api.github.com/repos/NotRexed/Hookshot/releases";
    static readonly System.Text.RegularExpressions.Regex NightlyName = new(@"-\d+-g[0-9a-f]+(-dirty)?$");
    static readonly System.Text.RegularExpressions.Regex ReleaseName = new(@"^v(\d+)\.(\d+)\.(\d+)(?:-rc\.(\d+))?$");

    /// A build's track from its version: git describe --long's v2.1.0-rc.1-3-g1a2b3c4 is the nightly, v2.1.0-rc.1 a release
    /// candidate, anything else a release
    public static string TrackOf(string? version) =>
        version == null ? Release : NightlyName.IsMatch(version) ? Nightly : ReleaseName.IsMatch(version) && version.Contains('-') ? Candidate : Release;

    /// The release that holds a version's files: the nightly's are on the rolling nightly release
    static string ReleaseOf(string tag) => TrackOf(tag) == Nightly ? Nightly : tag;

    /// a newer than b, both vX.Y.Z[-rc.N] in semver order (v2.1.0-rc.1 < v2.1.0-rc.2 < v2.1.0); a nightly counts as just after
    /// the version it names; false when either is not a version
    public static bool Newer(string? a, string? b)
    {
        static long[]? Key(string? v)
        {
            if (v == null) return null;
            var after = NightlyName.Match(v);
            var m = ReleaseName.Match(after.Success ? v[..after.Index] : v);
            if (!m.Success) return null;
            return new[] { long.Parse(m.Groups[1].Value), long.Parse(m.Groups[2].Value), long.Parse(m.Groups[3].Value),
                           m.Groups[4].Success ? long.Parse(m.Groups[4].Value) : long.MaxValue, after.Success ? 1 : 0 };
        }
        var x = Key(a);
        var y = Key(b);
        if (x == null || y == null) return false;
        for (var i = 0; i < x.Length; i++)
            if (x[i] != y[i]) return x[i] > y[i];
        return false;
    }

    /// What to update to: the track's newest (latest) for a release as before, a nightly, or after a switch of track (even
    /// an older version); for a release candidate only when it is newer, so it is never taken back to an older release
    public static string? Target(string? latest, string? tools, string track)
    {
        if (latest == null) return tools;
        if (tools == null || track != Candidate || TrackOf(tools) != Candidate) return latest;
        return Newer(latest, tools) ? latest : tools;
    }

    /// The newest build of a track, quietly (null: GitHub did not answer). release: the latest release, as before; rc: the
    /// newest of the release candidates and releases, else the latest release; nightly: the nightly's setup list's tag
    public static async Task<string?> LatestTag(string track)
    {
        if (track == Release) return await LatestTag();
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(8) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("HookshotBuilder");
        if (track == Nightly)
        {
            for (var attempt = 0; attempt < 2; attempt++)
            {
                try
                {
                    var list = await http.GetStringAsync("https://github.com/NotRexed/Hookshot/releases/download/nightly/" + SetupList);
                    foreach (var line in list.Split('\n'))
                        if (line.StartsWith("tag ")) return line[4..].Trim();
                }
                catch (Exception) { }
                await Task.Delay(TimeSpan.FromSeconds(2));
            }
            return null;
        }
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, Releases + "?per_page=20");
            request.Headers.Accept.ParseAdd("application/vnd.github+json");
            using var response = await http.SendAsync(request);
            if (response.IsSuccessStatusCode)
            {
                using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
                string? newest = null;
                foreach (var r in doc.RootElement.EnumerateArray())
                    if (r.GetProperty("tag_name").GetString() is { } t && ReleaseName.IsMatch(t) && (newest == null || Newer(t, newest)))
                        newest = t;
                if (newest != null) return newest;
            }
        }
        catch (Exception) { }
        return await LatestTag();
    }

    /// The API address of the release a target is on: the latest release for a release (as before), else that tag's
    static string ReleaseApi(string? tag) => tag == null || TrackOf(tag) == Release ? Latest : Releases + "/tags/" + ReleaseOf(tag);

    /// The latest release's tag, quietly (null: GitHub did not answer); a few tries, 20 s at most
    public static async Task<string?> LatestTag()
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(8) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("HookshotBuilder");
        for (var attempt = 0; attempt < 3; attempt++)
        {
            try
            {
                using var request = new HttpRequestMessage(HttpMethod.Get, Latest);
                request.Headers.Accept.ParseAdd("application/vnd.github+json");
                using var response = await http.SendAsync(request);
                if (response.IsSuccessStatusCode)
                {
                    using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
                    return doc.RootElement.GetProperty("tag_name").GetString();
                }
                if ((int)response.StatusCode < 500) break;   // (the API's limit, 60 an hour a PC: the list below instead)
            }
            catch (Exception) { }
            await Task.Delay(TimeSpan.FromSeconds(2 << attempt));
        }
        // (v2.0.0: when the API did not answer, the builder said a game was up to date. The release's setup list names its tag in
        // its first line, from GitHub's download service, not the API)
        for (var attempt = 0; attempt < 2; attempt++)
        {
            try
            {
                var list = await http.GetStringAsync("https://github.com/NotRexed/Hookshot/releases/latest/download/" + SetupList);
                foreach (var line in list.Split('\n'))
                    if (line.StartsWith("tag ")) return line[4..].Trim();
            }
            catch (Exception) { }
            await Task.Delay(TimeSpan.FromSeconds(2));
        }
        return null;
    }

    /// The latest release's setup (HookshotBuilder.exe) downloaded and started with args: it installs the new builder (only the
    /// parts that changed) and starts it with the same args; this one closes. (no args: the new builder opens as it does) A tag of
    /// another track (or a release asked for by a switch of track): that release's setup, told to install it (--tag)
    public static async Task RunLatestSetup(MainWindow w, IEnumerable<string> args, string? tag = null, bool named = false)
    {
        if (tag != null && (named || TrackOf(tag) != Release)) args = args.Concat(new[] { "--tag", tag });
        else tag = null;
        var token = w.DownloadToken;
        using var http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("HookshotBuilder");
        w.Note("Asking GitHub for the latest release");
        string? setupUrl = null, setupApiUrl = null, setupDigest = null;
        long setupSize = 0;
        using (var response = await GetWithRetries(w, http, tag == null ? Latest : Releases + "/tags/" + ReleaseOf(tag), token,
                                                   "application/vnd.github+json", TimeSpan.FromSeconds(30)))
        using (var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync(token)))
            foreach (var asset in doc.RootElement.GetProperty("assets").EnumerateArray())
                if (asset.GetProperty("name").GetString() == SetupAsset)
                {
                    setupUrl = asset.GetProperty("browser_download_url").GetString();
                    setupApiUrl = asset.GetProperty("url").GetString();
                    setupSize = asset.GetProperty("size").GetInt64();
                    setupDigest = DigestText(asset);
                }
        if (setupUrl == null) throw new InvalidOperationException("The latest release has no " + SetupAsset + ".");
        var cache = Path.Combine(BuilderHome.Base, "update");
        Directory.CreateDirectory(cache);
        var setup = Path.Combine(cache, SetupAsset);
        w.Note("Downloading the new Hookshot Builder");
        await DownloadWithRetries(w, http, setupUrl, setupApiUrl, setup, setupSize, setupDigest, token);
        var si = new ProcessStartInfo(setup) { UseShellExecute = false, WorkingDirectory = cache };
        foreach (var arg in args) si.ArgumentList.Add(arg);
        BuilderHome.PassTo(si);
        Process.Start(si);
        Application.Current.Shutdown();
    }

    /// The game in folder built again with the latest release: here when these tools are it (or fetched), else after the setup
    /// installed it (it starts the new builder with --update folder --tag t --fetched)
    public static async void UpdateFolder(MainWindow w, string folder, string? tag, bool fetched, bool named = false)
    {
        w.BeginUpdate(folder, tag);
        var tools = Tools.Find();
        if (tools == null)
        {
            w.DownloadFailed("The Hookshot tools were not found next to this app.");
            return;
        }
        // (just downloaded, but another release than asked for: the setup installed the release before it (GitHub's latest link lags
        // right after a publish), and building here kept players on the old version while the builder said it updated)
        if (fetched && tag != null && tools.Version != null && tools.Version != tag)
        {
            w.DownloadFailed($"The update installed {tools.Version}, not {tag}: GitHub's download links can lag right after a release. Try Update again in a few minutes.");
            return;
        }
        // (already the new release, or just downloaded: no second download)
        if (fetched || (tag != null && tools.Version == tag))
        {
            w.UpdateBuild(tools, folder);
            return;
        }
        try
        {
            var token = w.DownloadToken;
            using var http = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
            http.DefaultRequestHeaders.UserAgent.ParseAdd("HookshotBuilder");
            w.Note("Asking GitHub for the latest release");
            string? newTag = null, url = null, setupUrl = null;
            string? apiUrl = null, setupApiUrl = null;
            string? digest = null, setupDigest = null;
            long size = 0, setupSize = 0;
            var hasList = false;
            // (another track's build, or one a switch of track asked for: that release; the nightly's is named by its list's tag)
            var api = named && tag != null ? Releases + "/tags/" + ReleaseOf(tag) : ReleaseApi(tag);
            using (var response = await GetWithRetries(w, http, api, token, "application/vnd.github+json", TimeSpan.FromSeconds(30)))
            using (var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync(token)))
            {
                newTag = doc.RootElement.GetProperty("tag_name").GetString();
                if (newTag == Nightly && tag != null && TrackOf(tag) == Nightly) newTag = tag;
                foreach (var asset in doc.RootElement.GetProperty("assets").EnumerateArray())
                {
                    var assetName = asset.GetProperty("name").GetString();
                    if (assetName == Asset)
                    {
                        url = asset.GetProperty("browser_download_url").GetString();
                        apiUrl = asset.GetProperty("url").GetString();
                        size = asset.GetProperty("size").GetInt64();
                        digest = DigestText(asset);
                    }
                    else if (assetName == SetupAsset)
                    {
                        setupUrl = asset.GetProperty("browser_download_url").GetString();
                        setupApiUrl = asset.GetProperty("url").GetString();
                        setupSize = asset.GetProperty("size").GetInt64();
                        setupDigest = DigestText(asset);
                    }
                    else if (assetName == SetupList) hasList = true;
                }
            }
            if (newTag == null || (url == null && setupUrl == null)) throw new InvalidOperationException("The latest release has no " + Asset + ".");
            if (newTag == tools.Version)
            {
                w.UpdateBuild(tools, folder);
                return;
            }
            var name = string.Concat(newTag.Where(c => char.IsLetterOrDigit(c) || c == '-' || c == '_' || c == '.'));
            // (hookshot) the name is a folder under builders\ that the update replaces: "..", "." or a name with a leading dot
            // would be the HookshotBuilder folder itself or not a version (the same rule as the setup's and Linux's lists)
            if (name.Length == 0 || name.Length > 128 || name[0] == '.')
                throw new InvalidOperationException("The latest release's tag is not a version name.");
            var cache = Path.Combine(BuilderHome.Base, "update");
            Directory.CreateDirectory(cache);
            // (a release with the setup and its list: the setup downloads only the parts that changed since the version installed (a few MB
            // when only the builder's code did), checks them and starts the new builder with these arguments. The whole zip is the fallback)
            if (setupUrl != null && hasList)
            {
                w.Note($"Downloading the update for {newTag}");
                var setup = Path.Combine(cache, SetupAsset);
                await DownloadWithRetries(w, http, setupUrl, setupApiUrl, setup, setupSize, setupDigest, token);
                var ssi = new ProcessStartInfo(setup) { UseShellExecute = false, WorkingDirectory = cache };
                foreach (var arg in new[] { "--update", folder, "--tag", newTag, "--fetched" }) ssi.ArgumentList.Add(arg);
                BuilderHome.PassTo(ssi);
                Process.Start(ssi);
                Application.Current.Shutdown();
                return;
            }
            if (url == null) throw new InvalidOperationException("The latest release has no " + Asset + ".");
            var zip = Path.Combine(cache, "HookshotBuilder-" + name + ".zip");
            w.Note($"Downloading {Asset} of {newTag} ({size >> 20} MB)");
            await DownloadWithRetries(w, http, url, apiUrl, zip, size, digest, token, w.DownloadProgress);
            w.Note("Unpacking the new version");
            // (beside this app when that folder takes writing, else in the app data folder)
            var parent = Directory.GetParent(AppContext.BaseDirectory.TrimEnd('\\', '/'))!.FullName;
            string target;
            if (BuilderHome.Portable)
            {
                target = await Task.Run(() => PortableArchive.Install(zip, BuilderHome.Base, newTag));
            }
            else try
            {
                target = await Unpack(zip, Path.Combine(parent, "HookshotBuilder " + name));
            }
            catch (Exception e) when (e is UnauthorizedAccessException or IOException)
            {
                target = await Unpack(zip, Path.Combine(Path.GetDirectoryName(cache)!, "builders", name));
            }
            try { File.Delete(zip); } catch (Exception) { }
            var exe = Path.Combine(target, "HookshotBuilder.exe");
            if (!File.Exists(exe)) throw new FileNotFoundException("The download has no HookshotBuilder.exe (your antivirus may have removed it).");
            var si = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = target };
            foreach (var arg in new[] { "--update", folder, "--tag", newTag, "--fetched" }) si.ArgumentList.Add(arg);
            BuilderHome.PassTo(si);
            Process.Start(si);
            Application.Current.Shutdown();
        }
        catch (OperationCanceledException)
        {
            w.DownloadFailed("The download was cancelled. Your game folder is unchanged.");
        }
        catch (Exception e)
        {
            w.DownloadFailed(e.Message + " Your game folder is unchanged; you can also download the new version from GitHub yourself.");
        }
    }

    static async Task<HttpResponseMessage> GetWithRetries(MainWindow w, HttpClient http, string url, CancellationToken token,
        string? accept = null, TimeSpan? timeout = null)
    {
        Exception? last = null;
        for (var attempt = 0; attempt <= RetryDelays.Length; attempt++)
        {
            try
            {
                using var request = new HttpRequestMessage(HttpMethod.Get, url);
                if (accept != null) request.Headers.Accept.ParseAdd(accept);
                using var ask = timeout is null ? null : new CancellationTokenSource(timeout.Value);
                using var both = ask is null ? null : CancellationTokenSource.CreateLinkedTokenSource(token, ask.Token);
                var response = await http.SendAsync(request, HttpCompletionOption.ResponseContentRead, both?.Token ?? token);
                if (response.IsSuccessStatusCode) return response;

                var failure = HttpFailure(response);
                response.Dispose();
                if (!IsTransient(failure)) throw failure;
                last = failure;
            }
            catch (OperationCanceledException e) when (!token.IsCancellationRequested && timeout is not null)
            {
                last = e;
            }
            catch (HttpRequestException e) when (IsTransient(e))
            {
                last = e;
            }

            if (attempt == RetryDelays.Length) break;
            w.Note($"GitHub did not answer{StatusDetail(last)}. Retrying in {(int)RetryDelays[attempt].TotalSeconds} s...");
            await Task.Delay(RetryDelays[attempt], token);
        }
        throw new HttpRequestException($"GitHub's release API did not respond after {RetryDelays.Length + 1} attempts: {last?.Message}",
            last, (last as HttpRequestException)?.StatusCode);
    }

    static string? DigestText(JsonElement asset) =>
        asset.TryGetProperty("digest", out var digest) && digest.ValueKind == JsonValueKind.String ? digest.GetString() : null;

    static byte[] ParseDigest(string? digest)
    {
        if (digest == null || !digest.StartsWith("sha256:", StringComparison.Ordinal) || digest.Length != 71)
            throw new InvalidDataException("GitHub did not provide a valid SHA-256 digest for this download.");
        try { return Convert.FromHexString(digest[7..]); }
        catch (FormatException e) { throw new InvalidDataException("GitHub did not provide a valid SHA-256 digest for this download.", e); }
    }

    static async Task DownloadWithRetries(MainWindow w, HttpClient http, string url, string? apiUrl, string path, long size,
        string? digest, CancellationToken token, Action<double>? progress = null)
    {
        var expectedHash = ParseDigest(digest);   // Only the selected asset needs a digest; before any payload request.
        Exception? last = null;
        for (var attempt = 0; attempt <= RetryDelays.Length; attempt++)
        {
            try
            {
                await DownloadOnce(http, url, path, size, expectedHash, token, progress, apiAsset: false);
                return;
            }
            catch (Exception e) when (IsTransient(e))
            {
                last = e;
                if (apiUrl != null) w.Note($"Release download failed{StatusDetail(e)}; trying GitHub's API asset URL.");
            }

            if (apiUrl != null)
            {
                try
                {
                    await DownloadOnce(http, apiUrl, path, size, expectedHash, token, progress, apiAsset: true);
                    return;
                }
                catch (Exception e) when (IsTransient(e))
                {
                    last = e;
                }
            }

            if (attempt == RetryDelays.Length) break;
            w.Note($"GitHub's download servers did not answer{StatusDetail(last)}. Retrying in {(int)RetryDelays[attempt].TotalSeconds} s...");
            await Task.Delay(RetryDelays[attempt], token);
        }
        throw new HttpRequestException($"GitHub's asset download failed after {RetryDelays.Length + 1} attempts: {last?.Message}",
            last, (last as HttpRequestException)?.StatusCode);
    }

    static async Task DownloadOnce(HttpClient http, string url, string path, long size, byte[] expectedHash,
        CancellationToken token, Action<double>? progress, bool apiAsset)
    {
        var partial = path + ".part-" + Guid.NewGuid().ToString("N");
        var owned = false;
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, url);
            if (apiAsset) request.Headers.Accept.ParseAdd("application/octet-stream");
            using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, token);
            if (!response.IsSuccessStatusCode) throw HttpFailure(response);

            var total = response.Content.Headers.ContentLength ?? size;
            await using (var from = await response.Content.ReadAsStreamAsync(token))
            await using (var to = new FileStream(partial, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                owned = true;
                using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                var buffer = new byte[1 << 20];
                long have = 0;
                var shown = DateTime.MinValue;
                int n;
                while (true)
                {
                    try { n = await from.ReadAsync(buffer, token); }
                    catch (IOException e) { throw new HttpRequestException("The download connection ended unexpectedly.", e); }
                    if (n == 0) break;
                    await to.WriteAsync(buffer.AsMemory(0, n), token);
                    hash.AppendData(buffer, 0, n);
                    have += n;
                    if (progress != null && total > 0 && DateTime.UtcNow - shown > TimeSpan.FromMilliseconds(200))
                    {
                        shown = DateTime.UtcNow;
                        progress(Math.Min(1.0, (double)have / total));
                    }
                }
                if (total > 0 && have != total) throw new HttpRequestException($"The download ended early ({have >> 20} of {total >> 20} MB).");
                if (!CryptographicOperations.FixedTimeEquals(hash.GetHashAndReset(), expectedHash))
                    throw new InvalidDataException("The download did not match GitHub's SHA-256 digest.");
            }
            File.Move(partial, path, overwrite: true);
        }
        finally
        {
            if (owned)
                try { File.Delete(partial); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
    }

    static HttpRequestException HttpFailure(HttpResponseMessage response) =>
        new($"GitHub returned HTTP {(int)response.StatusCode} ({response.ReasonPhrase}).", null, response.StatusCode);

    static bool IsTransient(Exception error) =>
        error is HttpRequestException request && (request.StatusCode is null || (int)request.StatusCode >= 500);

    static string StatusDetail(Exception? error) =>
        error is HttpRequestException { StatusCode: { } status } ? $" (HTTP {(int)status})" : "";

    /// The zip's HookshotBuilder folder, as target (an older one there replaced)
    static async Task<string> Unpack(string zip, string target)
    {
        var part = target + ".part";
        if (Directory.Exists(part)) Directory.Delete(part, true);
        await Task.Run(() => ZipFile.ExtractToDirectory(zip, part));
        var inner = Path.Combine(part, "HookshotBuilder");
        if (Directory.Exists(target)) Directory.Delete(target, true);
        Directory.Move(Directory.Exists(inner) ? inner : part, target);
        if (Directory.Exists(part)) Directory.Delete(part, true);
        return target;
    }
}
