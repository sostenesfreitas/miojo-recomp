using System.IO;

namespace HookshotBuilder;

/// A .nup found on this PC
sealed record Dump(string Path, string Where, long Size, DateTime Modified);

/// The search behind Finder.FindDumps, on its own so a test can run it without WPF: every place searched at once, and what was
/// found handed back after a time limit even when a place never answers.
static class DumpSearch
{
    /// (long enough for a sleeping USB drive to spin up; a drive slower than this is left out, and its file can still be picked
    /// or dropped on the page)
    public const int Budget = 8000;

    /// The dumps in <places>, newest first, as far as the search got in <budgetMs>. A place still being searched then is left
    /// out (its search goes on unseen and ends with the app). A file in two places keeps the first place's name, in the list's order
    public static List<Dump> Search(IReadOnlyList<(string Dir, string Name)> places, int budgetMs, Func<string, string, List<Dump>> look)
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var unique = new List<(string Dir, string Name)>();
        foreach (var (dir, name) in places)
        {
            try
            {
                if (!string.IsNullOrEmpty(dir) && seen.Add(Path.GetFullPath(dir))) unique.Add((dir, name));
            }
            catch (Exception) { }
        }
        var searches = unique.Select(p => Task.Run(() => look(p.Dir, p.Name))).ToArray();
        try { Task.WaitAll(searches, budgetMs); } catch (Exception) { }   // (a search that threw is left out below)
        var files = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var found = new List<Dump>();
        foreach (var s in searches)
            if (s.IsCompletedSuccessfully)
                foreach (var d in s.Result)
                    if (files.Add(d.Path)) found.Add(d);
        return found.OrderByDescending(d => d.Modified).ToList();
    }

    /// Whether the file (or folder) is there, asked with a time limit: false when its drive does not answer within ExistsBudget.
    /// (The last build's files, ApplySettings: on a USB drive asleep, or a network drive gone, the check held the start-up page)
    public static bool Exists(string path, bool folder)
    {
        var check = Task.Run(() => folder ? Directory.Exists(path) : File.Exists(path));
        try { return check.Wait(ExistsBudget) && check.Result; }
        catch (Exception) { return false; }
    }

    public const int ExistsBudget = 3000;

    /// The .nup files in <dir> and the folders just inside it (not hidden ones, Windows or Program Files), up to 200 folders
    public static List<Dump> Look(string dir, string name)
    {
        var found = new List<Dump>();
        if (!Directory.Exists(dir)) return found;
        var dirs = new List<string> { dir };
        try
        {
            dirs.AddRange(Directory.EnumerateDirectories(dir)
                .Where(s => !Path.GetFileName(s).StartsWith('.') && !Path.GetFileName(s).StartsWith('$')
                            && !string.Equals(Path.GetFileName(s), "Windows", StringComparison.OrdinalIgnoreCase)
                            && !Path.GetFileName(s).StartsWith("Program", StringComparison.OrdinalIgnoreCase))
                .Take(200));
        }
        catch (Exception) { }
        foreach (var sub in dirs)
        {
            try
            {
                foreach (var f in Directory.EnumerateFiles(sub, "*.nup"))
                {
                    var info = new FileInfo(f);
                    found.Add(new Dump(info.FullName, name, info.Length, info.LastWriteTime));
                }
            }
            catch (Exception) { }
        }
        return found;
    }
}
