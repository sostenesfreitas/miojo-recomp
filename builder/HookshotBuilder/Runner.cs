using System.Diagnostics;
using System.IO;

namespace HookshotBuilder;

/// Where nx2/make_game.py and its Python are: a "tools" folder beside the app when packaged, or the source tree it sits in.
sealed record Tools(string Root, string Python, string[]? PythonArgs = null)
{
    /// Which release these tools are (tools/version.txt, written when the release is packed); null from the source tree
    public string? Version => File.Exists(Path.Combine(Root, "version.txt")) ? File.ReadAllText(Path.Combine(Root, "version.txt")).Trim() : null;

    /// The package's own Python is gone: an antivirus took it, most often, after a false alarm on the download
    public bool OwnPythonMissing => !File.Exists(Python) && Directory.Exists(Path.Combine(Root, "python"));

    public const string OwnPythonMissingText =
        "The builder's own Python (tools/python/python.exe) is missing. Your antivirus probably removed it: restore it " +
        "from quarantine, or unpack the zip again into a folder your antivirus allows, then try again.";

    public static Tools? Find()
    {
        var at = AppContext.BaseDirectory;
        var bundled = Path.Combine(at, "tools");
        if (File.Exists(Path.Combine(bundled, "nx2", "make_game.py")))
        {
            // (a package with a Python of its own runs no other. With its python.exe gone, the first "python" Windows found
            // ran the tools instead: another version, without the modules the tools need, or not one the player chose to
            // trust with the game files. Its start now fails and says how to get the package's Python back. Only a
            // package with no python folder at all uses the PC's)
            var py = Path.Combine(bundled, "python", "python.exe");
            return new Tools(bundled, File.Exists(py) || Directory.Exists(Path.Combine(bundled, "python")) ? py : "python");
        }
        // (the source tree the app was built in: its builder\ folder holds the app. Not any parent folder with an nx2\
        // folder, such as one another user of the PC made at the drive's root)
        for (var d = new DirectoryInfo(at); d != null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "nx2", "make_game.py")) &&
                at.StartsWith(Path.Combine(d.FullName, "builder") + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            {
                var (exe, args) = PythonOnPath();
                return new Tools(d.FullName, exe, args);
            }
        return null;
    }

    /// The Python for a source tree: Windows' Store shortcut is not an interpreter (it only opens the Store), so the py
    /// launcher is tried when "python" is that. CONTRIBUTING: Python 3 from python.org, not the Microsoft Store build.
    static (string Exe, string[]? Args) PythonOnPath()
    {
        var python = FirstOnPath("python.exe");
        if (python != null && !IsStoreShortcut(python)) return (python, null);
        var py = FirstOnPath("py.exe");
        if (py != null) return (py, new[] { "-3" });
        var later = AllOnPath("python.exe").FirstOrDefault(p => !IsStoreShortcut(p));
        return (later ?? python ?? "python", null);
    }

    static IEnumerable<string> AllOnPath(string name)
    {
        foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator))
            if (!string.IsNullOrWhiteSpace(dir))
            {
                var path = Path.Combine(dir.Trim('"'), name);
                if (File.Exists(path)) yield return path;
            }
    }

    static string? FirstOnPath(string name) => AllOnPath(name).FirstOrDefault();

    static bool IsStoreShortcut(string path) => path.Contains(@"\Microsoft\WindowsApps\", StringComparison.OrdinalIgnoreCase);
}

/// nx2/make_game.py as a child process: its lines as they come, and how it ended.
sealed class Runner
{
    readonly Tools tools;
    readonly List<string> args;
    readonly int? workers;
    readonly string? ngxSdk;
    readonly int? memoryGb;
    readonly string? ffxSdk;
    Process? process;
    string? error;
    bool cancelled;

    public event Action<string>? Line;
    public event Action<string?>? Finished;

    /// ngxSdk: NVIDIA DLSS (the builder's tick): the folder of NVIDIA's DLSS SDK, as HOOKSHOT_NGX_SDK; null builds without it.
    /// ffxSdk: AMD FSR 4 (the builder's tick): where AMD's FSR runtime is kept between builds, as HOOKSHOT_FFX_SDK
    public Runner(Tools tools, List<string> args, int? workers = null, string? ngxSdk = null, int? memoryGb = null,
                  string? ffxSdk = null)
    {
        this.tools = tools;
        this.args = args;
        if (workers is <= 0) throw new ArgumentOutOfRangeException(nameof(workers), "Build workers must be positive.");
        if (memoryGb is <= 0) throw new ArgumentOutOfRangeException(nameof(memoryGb), "Build memory must be positive.");
        this.workers = workers;
        this.ngxSdk = ngxSdk;
        this.memoryGb = memoryGb;
        this.ffxSdk = ffxSdk;
    }

    static ProcessStartInfo Info(Tools tools, IEnumerable<string> args, int? workers = null, string? ngxSdk = null,
                                 int? memoryGb = null, string? ffxSdk = null)
    {
        var si = new ProcessStartInfo(tools.Python)
        {
            WorkingDirectory = tools.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        si.Environment["PYTHONIOENCODING"] = "utf-8";
        BuildWorkers.Apply(si, workers);
        BuildMemory.Apply(si, memoryGb);
        // (written into the game folder's hookshot_version.txt: the game starts this app again to update, host/hos_update.c)
        BuilderHome.PassTo(si);
        if (Environment.ProcessPath != null) si.Environment["HOOKSHOT_BUILDER_EXE"] = Environment.ProcessPath;
        // (NVIDIA DLSS: only when ticked. Without it, one set on the PC is not passed on either: a build is with DLSS only when asked)
        if (ngxSdk != null) si.Environment["HOOKSHOT_NGX_SDK"] = ngxSdk;
        else si.Environment.Remove("HOOKSHOT_NGX_SDK");
        // (AMD FSR 4, the same: AMD's runtime kept in the builder's own folder only when ticked)
        if (ffxSdk != null) si.Environment["HOOKSHOT_FFX_SDK"] = ffxSdk;
        else si.Environment.Remove("HOOKSHOT_FFX_SDK");
        // (the folder's own compiler, headers and libraries, when it has them: nothing of the PC's but Windows' own)
        var sets = new[] { ("EMSDK", "emsdk"), ("RX_CPM", "cpm"), ("WGPU_NATIVE", "wgpu-native") };
        foreach (var (name, folder) in sets)
        {
            var path = Path.Combine(tools.Root, folder);
            if (Directory.Exists(path)) si.Environment[name] = path;
        }
        foreach (var a in tools.PythonArgs ?? Array.Empty<string>()) si.ArgumentList.Add(a);
        si.ArgumentList.Add("-I");
        si.ArgumentList.Add(Path.Combine(tools.Root, "nx2", "make_game.py"));
        foreach (var a in args) si.ArgumentList.Add(a);
        return si;
    }

    public void Start()
    {
        Task.Run(() =>
        {
            try
            {
                using var p = Process.Start(Info(tools, args, workers, ngxSdk, memoryGb, ffxSdk))!;
                process = p;
                p.ErrorDataReceived += (_, e) => { if (e.Data != null) Line?.Invoke(e.Data); };
                p.BeginErrorReadLine();
                try { p.PriorityClass = ProcessPriorityClass.BelowNormal; } catch (Exception) { }
                while (p.StandardOutput.ReadLine() is { } line)
                {
                    if (line.StartsWith("@@error ")) error = line[8..];
                    Line?.Invoke(line);
                }
                p.WaitForExit();
                Finished?.Invoke(cancelled ? "cancelled" : p.ExitCode == 0 ? null : error ?? "The build tools stopped without saying why. See the details.");
            }
            catch (Exception e)
            {
                Finished?.Invoke(tools.OwnPythonMissing ? Tools.OwnPythonMissingText : "The build tools could not start: " + e.Message);
            }
        });
    }

    public void Cancel()
    {
        cancelled = true;
        try { process?.Kill(entireProcessTree: true); } catch (Exception) { }
    }

    /// Is this the supported game and version? The answer in plain words.
    public static async Task<(bool Ok, string Text)> Check(Tools tools, IEnumerable<string> args)
    {
        try
        {
            using var p = Process.Start(Info(tools, args))!;
            string? lastError = null;
            p.ErrorDataReceived += (_, e) => { if (!string.IsNullOrWhiteSpace(e.Data)) lastError = e.Data; };
            p.BeginErrorReadLine();
            var result = (false, "The game files could not be read.");
            var answered = false;
            while (await p.StandardOutput.ReadLineAsync() is { } line)
            {
                if (line.StartsWith("@@check ok ")) { result = (true, line[11..]); answered = true; }
                else if (line.StartsWith("@@check bad ")) { result = (false, line[12..]); answered = true; }
                else if (line.StartsWith("@@error ")) { result = (false, line[8..]); answered = true; }
            }
            await p.WaitForExitAsync();
            // (a step that stopped before it could say why: the reason is on stderr, which used to be dropped here)
            if (!answered)
            {
                if (lastError != null && lastError.Contains("Microsoft Store", StringComparison.OrdinalIgnoreCase))
                    return (false, "Python 3 was not found: the 'python' command on this PC is the Microsoft Store shortcut, which only opens " +
                                   "the Store. Install Python 3 from python.org (not the Store version), or use the full Hookshot Builder package.");
                return (false, lastError ?? "The game tools stopped without saying why (exit code " + p.ExitCode + ").");
            }
            return result;
        }
        catch (Exception)
        {
            if (tools.OwnPythonMissing) return (false, Tools.OwnPythonMissingText);
            return (false, "Python 3 was not found. Install it from python.org (not the Microsoft Store build), or use the full Hookshot Builder package.");
        }
    }
}
