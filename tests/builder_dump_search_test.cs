using HookshotBuilder;
using System.Diagnostics;

// DumpSearch.Search with a place that never answers (a stand-in for a drive asleep or a cloud drive that hangs): the search still
// ends near its time limit with what the other places had, and DumpSearch.Look finds .nup files in an authored folder.
static class Program
{
    static int checks;
    static void Check(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
        checks++;
    }

    static void Main()
    {
        var root = Path.Combine(Path.GetTempPath(), "hookshot-dumps-" + Guid.NewGuid().ToString("N"));
        var downloads = Path.Combine(root, "Downloads é");
        Directory.CreateDirectory(Path.Combine(downloads, "OoT"));
        Directory.CreateDirectory(Path.Combine(downloads, ".hidden"));
        try
        {
            File.WriteAllText(Path.Combine(downloads, "a.nup"), "x");
            File.WriteAllText(Path.Combine(downloads, "OoT", "b.nup"), "xy");
            File.WriteAllText(Path.Combine(downloads, ".hidden", "c.nup"), "xyz");
            File.WriteAllText(Path.Combine(downloads, "notes.txt"), "");
            File.SetLastWriteTime(Path.Combine(downloads, "a.nup"), new DateTime(2026, 1, 1));
            File.SetLastWriteTime(Path.Combine(downloads, "OoT", "b.nup"), new DateTime(2026, 2, 1));

            var looked = DumpSearch.Look(downloads, "Downloads");
            Check(looked.Count == 2, $"Look found {looked.Count} files, not 2 (one at the top, one a folder down, none hidden)");
            Check(DumpSearch.Look(Path.Combine(root, "nowhere"), "X").Count == 0, "a missing folder gives no files");

            using var never = new ManualResetEventSlim(false);
            List<Dump> Hangs(string dir, string name) { never.Wait(); return new List<Dump>(); }
            List<Dump> Look(string dir, string name) => dir == "Z:\\" ? Hangs(dir, name) : DumpSearch.Look(dir, name);
            List<Dump> Throws(string dir, string name) => throw new IOException("the drive went away");

            var clock = Stopwatch.StartNew();
            var found = DumpSearch.Search(new List<(string, string)> { (downloads, "Downloads"), ("Z:\\", "Z:"), (downloads, "Home") }, 1500, Look);
            var took = clock.ElapsedMilliseconds;
            Check(took < 5000, $"the search waited {took} ms on a place that never answers (limit 1500 ms)");
            Check(found.Count == 2, $"{found.Count} files found, not the 2 in the place that answered");
            Check(found[0].Path.EndsWith("b.nup") && found[1].Path.EndsWith("a.nup"), "the files are not newest first");
            Check(found.All(d => d.Where == "Downloads"), "a folder listed twice kept the second place's name");

            found = DumpSearch.Search(new List<(string, string)> { ("Y:\\", "Y:"), (downloads, "Downloads") }, 5000,
                                      (d, n) => d == "Y:\\" ? Throws(d, n) : DumpSearch.Look(d, n));
            Check(found.Count == 2, "a place whose search threw took the others' files with it");

            clock.Restart();
            DumpSearch.Search(new List<(string, string)> { (downloads, "Downloads") }, 60000, DumpSearch.Look);
            Check(clock.ElapsedMilliseconds < 5000, "the search waited out its limit though every place had answered");
            never.Set();
        }
        finally
        {
            Directory.Delete(root, true);
        }
        Console.WriteLine($"builder dump search: {checks} checks passed");
    }
}
