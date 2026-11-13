using System.Diagnostics;
using System.IO.Compression;
using System.Net;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using HookshotBuilder;

namespace DigestFixture
{
    // This suite varies the standard-home boundary per case. Portable root/archive algorithms
    // execute separately in test_portable_builder_home.py and test_portable_archive.py.
    static class FakeBuilderHome
    {
        public static string Base => Path.Combine(State.Current.Root, "HookshotBuilder");
        public static bool Portable => false;
        public static void PassTo(ProcessStartInfo info) => info.Environment["HOOKSHOT_SETUP_BASE"] = Base;
    }
    static class UnusedPortableArchive
    {
        public static string Install(string zip, string home, string tag) =>
            throw new InvalidOperationException("A standard update unexpectedly selected the portable archive route.");
    }
    static class FakeEnvironment
    {
        public enum SpecialFolder { LocalApplicationData }
        public static string GetFolderPath(SpecialFolder _) => State.Current.Root;
        public static string[] GetCommandLineArgs() => Array.Empty<string>();
    }

    static class FakeAppContext
    {
        public static string BaseDirectory => Path.Combine(State.Current.Root, "current", "installed") + Path.DirectorySeparatorChar;
    }

    static class FakeProcess
    {
        public static void Start(ProcessStartInfo info) => State.Current.Starts.Add(info);
    }

    sealed class FakeHttpClient : HttpClient
    {
        public FakeHttpClient() : base(State.Current.Handler, disposeHandler: false) { }
    }

    sealed class Handler : HttpMessageHandler
    {
        public readonly List<string> Requests = new();
        public readonly List<string> Accepts = new();
        public Func<HttpRequestMessage, HttpResponseMessage> Respond = _ => throw new Exception("Unconfigured authored HTTP response.");
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken token)
        {
            token.ThrowIfCancellationRequested();
            Requests.Add(request.RequestUri!.AbsoluteUri);
            Accepts.Add(string.Join(",", request.Headers.Accept));
            return Task.FromResult(Respond(request));
        }
    }

    // Nonseekable authored stream: no Content-Length; optional transport/cancellation failure after one chunk.
    sealed class Bytes : Stream
    {
        readonly byte[] data;
        readonly Action? afterFirst;
        int at;
        public Bytes(byte[] value, Action? failure = null) { data = value; afterFirst = failure; }
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => at; set => throw new NotSupportedException(); }
        public override int Read(byte[] buffer, int offset, int count)
        {
            if (at != 0) afterFirst?.Invoke();
            var n = Math.Min(Math.Min(count, 3), data.Length - at);
            Array.Copy(data, at, buffer, offset, n); at += n; return n;
        }
        public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken token = default)
        {
            if (at != 0) afterFirst?.Invoke();
            token.ThrowIfCancellationRequested();
            var n = Math.Min(Math.Min(buffer.Length, 3), data.Length - at);
            data.AsMemory(at, n).CopyTo(buffer); at += n;
            return ValueTask.FromResult(n);
        }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    }

    sealed class State : IDisposable
    {
        public static State Current = null!;
        public readonly string Root = Path.Combine(Path.GetTempPath(), "hookshot-updater-digest-" + Guid.NewGuid().ToString("N"));
        public readonly Handler Handler = new();
        public readonly List<ProcessStartInfo> Starts = new();
        public readonly CancellationTokenSource Cancel = new();
        public readonly TaskCompletionSource<string> Outcome = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public readonly MainWindow Window = new();
        public readonly byte[] Good = Encoding.ASCII.GetBytes("authored harmless setup bytes");
        public readonly byte[] Old = Encoding.ASCII.GetBytes("keep previous cached file");
        public readonly List<double> Progress = new();
        public int MetadataRequests;
        public string Cache => Path.Combine(Root, "HookshotBuilder", "update");
        public string Setup => Path.Combine(Cache, "HookshotBuilder.exe");
        public string Game => Path.Combine(Root, "authored-game-folder");

        public State()
        {
            Current = this;
            Directory.CreateDirectory(Cache);
            Directory.CreateDirectory(FakeAppContext.BaseDirectory);
            Directory.CreateDirectory(Game);
            File.WriteAllBytes(Setup, Old);
            File.WriteAllText(Path.Combine(Game, "keep.txt"), "unchanged authored sentinel");
        }

        public static string Hash(byte[] bytes) => "sha256:" + Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

        public void Release(byte[] served, object? digest, bool zip = false, bool list = true,
                            bool primaryFails = false, bool length = true, bool unusedBadZip = false, bool omitDigest = false)
        {
            var assets = new List<Dictionary<string, object?>>();
            Dictionary<string, object?> Asset(string name, object? hash) => new()
            {
                ["name"] = name, ["browser_download_url"] = "https://fixture.invalid/browser",
                ["url"] = "https://fixture.invalid/api", ["size"] = served.Length, ["digest"] = hash
            };
            assets.Add(Asset(zip ? "HookshotBuilder.zip" : "HookshotBuilder.exe", digest));
            if (omitDigest) assets[0].Remove("digest");
            if (list) assets.Add(new() { ["name"] = "hookshot-setup.txt" });
            if (unusedBadZip) assets.Add(Asset("HookshotBuilder.zip", null));
            var json = JsonSerializer.SerializeToUtf8Bytes(new { tag_name = "v9.9.9", assets });
            Handler.Respond = request =>
            {
                if (request.RequestUri!.Host == "api.github.com")
                {
                    MetadataRequests++;
                    return new(HttpStatusCode.OK) { Content = new ByteArrayContent(json) };
                }
                if (primaryFails && request.RequestUri.AbsolutePath == "/browser")
                    return new(HttpStatusCode.ServiceUnavailable);
                HttpContent content = length ? new ByteArrayContent(served) : new StreamContent(new Bytes(served));
                return new(HttpStatusCode.OK) { Content = content };
            };
        }

        public Task SetupRun() => Updater.RunLatestSetup(Window, Array.Empty<string>());
        public async Task<string> FolderRun()
        {
            Updater.UpdateFolder(Window, Game, "v9.9.9", fetched: false);
            return await Outcome.Task.WaitAsync(TimeSpan.FromSeconds(10));
        }

        public void NoUse()
        {
            Program.Check(Starts.Count == 0, "Unverified payload reached process-launch spy.");
            Program.Check(!Directory.Exists(Path.Combine(Root, "current", "HookshotBuilder v9.9.9")), "Unverified ZIP was extracted.");
            Program.Check(File.ReadAllText(Path.Combine(Game, "keep.txt")) == "unchanged authored sentinel", "Game sentinel changed.");
        }

        public void NoPartial() => Program.Check(!Directory.EnumerateFiles(Cache, "*.part-*").Any(), "Owned partial download remains.");

        public void Dispose()
        {
            Cancel.Dispose(); Handler.Dispose();
            var full = Path.GetFullPath(Root);
            Program.Check(Path.GetDirectoryName(full) == Path.GetFullPath(Path.GetTempPath()).TrimEnd(Path.DirectorySeparatorChar)
                && Path.GetFileName(full).StartsWith("hookshot-updater-digest-", StringComparison.Ordinal), "Temporary cleanup escaped fixture root.");
            Directory.Delete(full, recursive: true);
        }
    }

    static class Program
    {
        static int cases, failures;
        public static void Check(bool ok, string why) { if (!ok) throw new Exception(why); }

        static async Task<bool> Rejects<T>(Func<Task> action) where T : Exception
        {
            try { await action(); return false; }
            catch (T) { return true; }
        }

        static byte[] Zip(string text)
        {
            using var bytes = new MemoryStream();
            using (var zip = new ZipArchive(bytes, ZipArchiveMode.Create, leaveOpen: true))
            {
                var entry = zip.CreateEntry("HookshotBuilder/HookshotBuilder.exe", CompressionLevel.NoCompression);
                entry.LastWriteTime = new DateTimeOffset(2000, 1, 1, 0, 0, 0, TimeSpan.Zero);
                using var writer = new StreamWriter(entry.Open(), Encoding.ASCII);
                writer.Write(text);
            }
            return bytes.ToArray();
        }

        static async Task Once(State s, HttpContent content, long size, CancellationToken token)
        {
            s.Handler.Respond = _ => new(HttpStatusCode.OK) { Content = content };
            using var http = new FakeHttpClient();
            var values = new Dictionary<string, object?>
            {
                ["http"] = http, ["url"] = "https://fixture.invalid/once", ["path"] = s.Setup,
                ["size"] = size, ["expectedHash"] = SHA256.HashData(s.Good),
                ["token"] = token, ["progress"] = new Action<double>(s.Progress.Add), ["apiAsset"] = false
            };
            var method = typeof(Updater).GetMethod("DownloadOnce", BindingFlags.NonPublic | BindingFlags.Static)!;
            // Bind the real old/new signature by parameter name; all behavioral assertions stay identical.
            var args = method.GetParameters().Select(p => values[p.Name!]).ToArray();
            await (Task)method.Invoke(null, args)!;
        }

        static async Task Case(string name, Func<State, Task> test)
        {
            cases++;
            try { using var state = new State(); await test(state); Console.WriteLine("PASS " + name); }
            catch (Exception error) { failures++; Console.WriteLine("FAIL " + name + ": " + error.Message); }
        }

        static async Task Main()
        {
            await Case("setup-corrupt-no-use", async s =>
            {
                var bad = (byte[])s.Good.Clone(); bad[0] ^= 1;
                s.Release(bad, State.Hash(s.Good));
                var rejected = await Rejects<InvalidDataException>(s.SetupRun);
                Check(rejected, "Same-length corruption accepted; launch-spy calls=" + s.Starts.Count);
                Check(s.Handler.Requests.Count == 2, "Digest mismatch retried through the API.");
                s.NoUse(); s.NoPartial(); Check(File.ReadAllBytes(s.Setup).SequenceEqual(s.Old), "Previous cached setup was replaced on failure.");
            });
            await Case("missing-digest", async s =>
            {
                s.Release(s.Good, null, omitDigest: true);
                Check(await Rejects<InvalidDataException>(s.SetupRun), "Missing digest accepted.");
                Check(s.Handler.Requests.Count == 1 && s.MetadataRequests == 1, "Missing digest fetched payload.");
                s.NoUse(); s.NoPartial();
            });
            await Case("folder-setup-corrupt-no-use", async s =>
            {
                var bad = (byte[])s.Good.Clone(); bad[0] ^= 1;
                s.Release(bad, State.Hash(s.Good));
                Check((await s.FolderRun()) != "shutdown", "Corruption accepted by game-folder setup route.");
                s.NoUse(); s.NoPartial();
            });
            await Case("zip-corrupt-no-extract", async s =>
            {
                var good = Zip("authored A");
                var bad = Zip("authored B");
                Check(good.Length == bad.Length, "Authored ZIP lengths differ.");
                s.Release(bad, State.Hash(good), zip: true, list: false);
                Check((await s.FolderRun()) != "shutdown", "Same-length alternate ZIP accepted.");
                s.NoUse(); s.NoPartial();
            });
            foreach (var pair in new (string, object?)[] {
                ("null-digest", null), ("malformed-digest", "sha256:abc"),
                ("nonhex-digest", "sha256:" + new string('g', 64)),
                ("unsupported-digest", "sha512:" + new string('0', 128)), ("nonstring-digest", 123) })
                await Case(pair.Item1, async s =>
                {
                    s.Release(s.Good, pair.Item2);
                    Check(await Rejects<InvalidDataException>(s.SetupRun), "Invalid digest accepted.");
                    Check(s.Handler.Requests.Count == 1 && s.MetadataRequests == 1, "Invalid digest fetched payload.");
                    s.NoUse(); s.NoPartial();
                });
            await Case("api-corrupt-terminal", async s =>
            {
                var bad = (byte[])s.Good.Clone(); bad[0] ^= 1;
                s.Release(bad, State.Hash(s.Good), primaryFails: true);
                Check(await Rejects<InvalidDataException>(s.SetupRun), "API fallback corruption accepted.");
                Check(s.Handler.Requests.Count == 3, "Digest mismatch retried or fell back again.");
                Check(s.Handler.Accepts.Last().Contains("application/octet-stream"), "API asset route lost binary Accept header.");
                s.NoUse(); s.NoPartial();
            });
            await Case("valid-setup", async s =>
            {
                s.Release(s.Good, State.Hash(s.Good));
                await s.SetupRun(); Check(s.Starts.Count == 1, "Valid setup not handed off.");
                Check(File.ReadAllBytes(s.Setup).SequenceEqual(s.Good), "Valid payload changed."); s.NoPartial();
            });
            await Case("valid-folder-setup-unused-bad-zip", async s =>
            {
                s.Release(s.Good, State.Hash(s.Good), unusedBadZip: true);
                Check(await s.FolderRun() == "shutdown" && s.Starts.Count == 1, "Unused ZIP digest blocked selected setup."); s.NoPartial();
            });
            await Case("valid-zip", async s =>
            {
                var zip = Zip("authored harmless text");
                s.Release(zip, State.Hash(zip), zip: true, list: false);
                Check(await s.FolderRun() == "shutdown" && s.Starts.Count == 1, "Valid ZIP not handed off.");
                Check(File.ReadAllText(s.Starts[0].FileName) == "authored harmless text", "Authored ZIP contents changed.");
                Check(s.Progress.Count > 0 && s.Progress.All(x => x > 0 && x <= 1), "ZIP progress changed."); s.NoPartial();
            });
            await Case("valid-api-fallback", async s =>
            {
                s.Release(s.Good, State.Hash(s.Good), primaryFails: true);
                await s.SetupRun(); Check(s.Starts.Count == 1 && s.Handler.Requests.Count == 3, "Transient/API fallback changed."); s.NoPartial();
            });
            await Case("chunked-valid", async s =>
            {
                s.Release(s.Good, State.Hash(s.Good), length: false);
                await s.SetupRun(); Check(s.Starts.Count == 1, "No-Content-Length valid stream rejected."); s.NoPartial();
            });
            await Case("chunked-corrupt", async s =>
            {
                var bad = (byte[])s.Good.Clone(); bad[^1] ^= 1;
                s.Release(bad, State.Hash(s.Good), length: false);
                Check(await Rejects<InvalidDataException>(s.SetupRun), "Chunked same-length corruption accepted."); s.NoUse(); s.NoPartial();
            });
            await Case("http403-not-retried", async s =>
            {
                s.Release(s.Good, State.Hash(s.Good));
                var normal = s.Handler.Respond;
                s.Handler.Respond = r => r.RequestUri!.Host == "api.github.com" ? normal(r) : new(HttpStatusCode.Forbidden);
                Check(await Rejects<HttpRequestException>(s.SetupRun), "HTTP403 accepted.");
                Check(s.Handler.Requests.Count == 2, "Authorization failure retried/fell back."); s.NoUse();
            });
            await Case("early-eof", async s =>
            {
                var content = new ByteArrayContent(s.Good);
                content.Headers.ContentLength = s.Good.Length + 1;
                Check(await Rejects<HttpRequestException>(() => Once(s, content, s.Good.Length + 1, CancellationToken.None)), "Length failure changed."); s.NoPartial();
            });
            await Case("stream-failure", async s =>
            {
                var content = new StreamContent(new Bytes(s.Good, () => throw new IOException("authored stream failure")));
                Check(await Rejects<HttpRequestException>(() => Once(s, content, s.Good.Length, CancellationToken.None)), "Stream failure changed."); s.NoPartial();
            });
            await Case("midstream-cancel", async s =>
            {
                var content = new StreamContent(new Bytes(s.Good, () => s.Cancel.Cancel()));
                Check(await Rejects<OperationCanceledException>(() => Once(s, content, s.Good.Length, s.Cancel.Token)), "Cancellation changed."); s.NoPartial();
            });
            Console.WriteLine($"Updater digest cases: {cases - failures} passed, {failures} failed.");
            if (failures != 0) System.Environment.ExitCode = 1;
        }
    }
}

namespace HookshotBuilder
{
    sealed class Tools
    {
        public string Version => "v0.0.0";
        public static Tools Find() => new();
    }

    sealed class MainWindow
    {
        public CancellationToken DownloadToken => DigestFixture.State.Current.Cancel.Token;
        public void Note(string _) { }
        public void BeginUpdate(string folder, string? tag) { }
        public void UpdateBuild(Tools tools, string folder) => throw new Exception("Unexpected real build path.");
        public void DownloadProgress(double value) => DigestFixture.State.Current.Progress.Add(value);
        public void DownloadFailed(string reason) => DigestFixture.State.Current.Outcome.TrySetResult(reason);
    }
}

namespace System.Windows
{
    sealed class Application
    {
        public static Application Current => new();
        public void Shutdown() => DigestFixture.State.Current.Outcome.TrySetResult("shutdown");
    }
}
