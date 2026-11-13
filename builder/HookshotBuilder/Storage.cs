using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace HookshotBuilder;

/// What the builder keeps on the PC, and letting it go. In %LOCALAPPDATA%\HookshotBuilder: the builder itself (versions\: the one
/// installed last, and the one before it), the setup's downloaded parts (the next builder update downloads only those that changed).
/// In the build files folder (that same folder, or one the player chose: make_game.py --work): the translated code and its compiled
/// objects (sa\, objcache\: the next update compiles only what changed). None of it is needed to play: the game folder holds all the
/// game uses, and its copy of the game files (.hookshot_exefs) is what updates build from
static class Storage
{
    public static string Base => BuilderHome.Base;

    /// About what a build leaves in the build files folder (the translated code and its compiled objects): said before the build
    public const long BuildFilesBytes = 4L << 30;

    /// HOOKSHOT_KEEP_BUILD_FILES=1: the build files stay after a build (quick updates); otherwise they go when the game is ready
    public static bool KeepBuildFiles => Environment.GetEnvironmentVariable("HOOKSHOT_KEEP_BUILD_FILES") is { Length: > 0 } v && v != "0";

    /// Where builds keep their files: the folder the player chose (work.txt), else the builder's own
    public static string Work
    {
        get
        {
            try
            {
                var chosen = BuilderHome.LoadPath(File.ReadAllText(Path.Combine(Base, "work.txt")).Trim()) ?? "";
                if (chosen.Length > 2 && Path.IsPathFullyQualified(chosen)) return chosen;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            return Base;
        }
    }

    public static bool WorkChosen => !Same(Work, Base);

    /// (NVIDIA DLSS, only when ticked) NVIDIA's DLSS SDK, downloaded under NVIDIA's license by the build (make_game.py
    /// --fetch-ngx-sdk, scripts/ngx_sdk.py): HOOKSHOT_NGX_SDK. NVIDIA's files, never part of the builder or its downloads
    public static string NgxSdk => Path.Combine(Base, "ngx-sdk");

    /// (AMD FSR 4, only when ticked) AMD's FSR runtime, downloaded under AMD's license by the build (make_game.py
    /// --fetch-fsr-runtime, scripts/ffx_sdk.py): HOOKSHOT_FFX_SDK. AMD's files, never part of the builder or its downloads
    public static string FfxSdk => Path.Combine(Base, "ffx-sdk");

    /// The build files go to a folder of their own in the one the player picked (null: back to the builder's own). What the last
    /// builds left at the old place goes: the next build would not look for it there
    public static void MoveWork(string? picked)
    {
        var old = Work;
        var to = picked == null ? Base : Path.Combine(picked, "Hookshot build files");
        if (Same(old, to)) return;
        Directory.CreateDirectory(to);
        Directory.CreateDirectory(Base);
        if (picked == null) File.Delete(Path.Combine(Base, "work.txt"));
        else File.WriteAllText(Path.Combine(Base, "work.txt"), BuilderHome.SavePath(to));
        foreach (var d in WorkParts(old)) Delete(d);
        if (!Same(old, Base))
            try { Directory.Delete(old); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }   // (only when empty)
    }

    /// What a build makes in its work folder (make_game.py, lift.py, build.py)
    static IEnumerable<string> WorkParts(string work) =>
        new[] { "objcache", "sa", "exefs", "probe", "image", "system_headers.txt", "hot_funcs.txt", "hot_order.txt", "hookshot_pieces.prof", "hookshot.res" }
            .Select(n => Path.Combine(work, n));

    /// The builder versions that stay: the one installed last (current.txt) and the one running
    static bool Kept(string versionDir)
    {
        try
        {
            if (File.ReadAllText(Path.Combine(Base, "versions", "current.txt")).Trim() == Path.GetFileName(versionDir)) return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        var running = Path.GetFullPath(AppContext.BaseDirectory).TrimEnd('\\') + "\\";
        return running.StartsWith(Path.GetFullPath(versionDir).TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase);
    }

    /// make_game.py keep_translation left the lift's small files in sa (its .lift_key marked "kept"): the next update with the same
    /// tools skips the lift with them. Not let go after a build (Free up space still lets them go)
    static bool KeptTranslation(string sa)
    {
        try
        {
            var words = File.ReadAllText(Path.Combine(sa, "gen", ".lift_key")).Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            return words.Length >= 3 && words[2] == "kept";
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }

    /// What Free lets go. afterBuild: what goes once the game is ready, the compiled objects of the build (objcache, which make_game.py
    /// prunes to them) and their system headers' stamp kept, so that the next update compiles only what changed (issue 1287: every
    /// update compiled all ~600 files again, an hour and more). The Free up space button lets those go too
    static IEnumerable<string> Freeable(bool afterBuild = false)
    {
        foreach (var d in afterBuild ? new[] { "sa", "exefs", "probe" } : new[] { "objcache", "sa", "exefs", "probe" })
            if (!(afterBuild && d == "sa" && KeptTranslation(Path.Combine(Work, d))))
                yield return Path.Combine(Work, d);
        foreach (var d in new[] { "downloads", "update" })
            yield return Path.Combine(Base, d);
        var versions = Path.Combine(Base, "versions");
        if (Directory.Exists(versions))
            foreach (var v in Directory.EnumerateDirectories(versions))
                if (!Kept(v)) yield return v;
    }

    /// Bytes in the builder's folder and the build files folder, and in what Free lets go (an object hard-linked in both sa\build and
    /// objcache counted once)
    public static (long total, long freeable) Measure()
    {
        var seen = new HashSet<(uint, ulong)>();
        long freeable = 0;
        foreach (var d in Freeable()) freeable += Size(d, seen);
        seen.Clear();
        var total = Size(Base, seen) + (WorkChosen ? Size(Work, seen) : 0);
        return (Math.Max(total, freeable), freeable);
    }

    /// Lets go of what Measure counts as freeable; returns the bytes it could not delete (a file in use)
    public static long Free(bool afterBuild = false)
    {
        foreach (var d in Freeable(afterBuild)) Delete(d);
        if (!afterBuild) Delete(Path.Combine(Work, "system_headers.txt"));   // (the stamp of the compiled objects' system headers goes with them)
        if (!afterBuild) return Measure().freeable;
        var seen = new HashSet<(uint, ulong)>();   // (what of the after-build set is still there: a file in use)
        long left = 0;
        foreach (var d in Freeable(afterBuild: true)) left += Size(d, seen);
        return left;
    }

    public static string Gb(long bytes) => bytes >= 1L << 30 ? $"{bytes / (double)(1L << 30):0.0} GB" : $"{bytes >> 20} MB";

    static bool Same(string a, string b) =>
        string.Equals(Path.GetFullPath(a).TrimEnd('\\'), Path.GetFullPath(b).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase);

    static void Delete(string path)
    {
        try
        {
            if (Directory.Exists(path)) Directory.Delete(path, recursive: true);
            else if (File.Exists(path)) File.Delete(path);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    static long Size(string dir, HashSet<(uint, ulong)> seen)
    {
        if (!Directory.Exists(dir)) return 0;
        long n = 0;
        IEnumerable<FileInfo> files;
        try
        {
            files = new DirectoryInfo(dir).EnumerateFiles("*", new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true });
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return 0; }
        foreach (var f in files)
        {
            try
            {
                if (f.Length == 0) continue;
                if (Identity(f.FullName) is { } id && !seen.Add(id)) continue;
                n += f.Length;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        return n;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]   // (BY_HANDLE_FILE_INFORMATION: its FILETIMEs are two DWORDs, no padding)
    struct FileInfoByHandle
    {
        public uint Attributes;
        public long Created, Accessed, Written;
        public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandle(SafeFileHandle h, out FileInfoByHandle info);

    /// The file's volume and index (the same for each of its hard links); null for one with a single link, which needs no check
    static (uint, ulong)? Identity(string path)
    {
        using var h = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        if (!GetFileInformationByHandle(h, out var i) || i.Links < 2) return null;
        return (i.Volume, ((ulong)i.IndexHigh << 32) | i.IndexLow);
    }
}
