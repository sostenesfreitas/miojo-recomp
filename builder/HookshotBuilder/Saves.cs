using System.Diagnostics;
using System.IO;
using System.Text.Json;

namespace HookshotBuilder;

/// A save found on this PC that a build can start from: an emulator's or another port's (scripts/oot_saves.py finds them)
sealed record FoundSave(string Name, string Shown, string Profile, string Path, string Slots, bool Waiting);

/// The form's "Bring in a save": scripts/oot_saves.py finds the Ocarina of Time saves on this PC (list --json) and, once a
/// build is done, copies the chosen one into its game folder (import ... --json: a backup of the folder's save first)
static class Saves
{
    public static List<FoundSave> Find(Tools tools)
    {
        var (doc, _) = Run(tools, "list", "--json");
        var found = new List<FoundSave>();
        if (doc == null) return found;
        using (doc)
            foreach (var source in doc.RootElement.GetProperty("sources").EnumerateArray())
            {
                var name = source.GetProperty("name").GetString()!;
                // (emulators that share one save folder, Eden's forks: one row, its programs said under it)
                var programs = source.GetProperty("display").GetString()!.Split(" / ").Where(n => n != "NX2navi").ToArray();
                var shown = name == "nx2navi" ? "another PC port" : programs.Length > 1 ? "your emulator's save" : programs[0];
                var who = programs.Length > 1 ? string.Join(", ", programs[..^1]) + " and " + programs[^1] + " keep their saves here · " : "";
                foreach (var save in source.GetProperty("saves").EnumerateArray())
                {
                    var slots = save.GetProperty("slots").EnumerateArray().Select(s =>
                    {
                        var slot = s.GetProperty("slot").GetString()!.Replace("slot_", "Slot ");
                        return s.GetProperty("saved").ValueKind == JsonValueKind.String && DateTime.TryParse(s.GetProperty("saved").GetString(), out var when)
                            ? $"{slot}, saved {Finder.Ago(when)}" : slot;
                    });
                    found.Add(new FoundSave(name, shown, save.GetProperty("profile").GetString()!,
                                            save.GetProperty("path").GetString()!, who + string.Join(" · ", slots),
                                            save.TryGetProperty("pending", out var p) && p.ValueKind == JsonValueKind.String));
                }
            }
        return found;
    }

    /// The save into gameFolder: what to tell the player (Ok false: why it did not happen; the game folder is unchanged)
    public static (bool Ok, string Text) Import(Tools tools, FoundSave save, string gameFolder)
    {
        var (doc, error) = Run(tools, "import", save.Name, "--profile", save.Profile, "--hookshot", gameFolder, "--json");
        if (doc == null) return (false, $"Your save from {save.Shown} was not brought in: {error}");
        using (doc)
            return (true, $"Your save from {save.Shown} is in the game ({doc.RootElement.GetProperty("slots").GetArrayLength()} slot(s)).");
    }

    /// scripts/oot_saves.py with these arguments: its JSON answer, or what went wrong in words
    static (JsonDocument? Doc, string Error) Run(Tools tools, params string[] args)
    {
        if (tools.OwnPythonMissing) return (null, Tools.OwnPythonMissingText);
        var script = System.IO.Path.Combine(tools.Root, "scripts", "oot_saves.py");
        if (!File.Exists(script)) return (null, "this builder has no save tool (scripts/oot_saves.py)");
        var si = new ProcessStartInfo(tools.Python)
        {
            WorkingDirectory = tools.Root, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true, StandardOutputEncoding = System.Text.Encoding.UTF8,
        };
        si.Environment["PYTHONIOENCODING"] = "utf-8";
        foreach (var a in tools.PythonArgs ?? Array.Empty<string>()) si.ArgumentList.Add(a);
        si.ArgumentList.Add("-I");
        si.ArgumentList.Add(script);
        foreach (var a in args) si.ArgumentList.Add(a);
        try
        {
            using var proc = Process.Start(si)!;
            var stderr = proc.StandardError.ReadToEndAsync();
            var line = proc.StandardOutput.ReadToEnd().Trim().Split('\n').LastOrDefault()?.Trim() ?? "";
            proc.WaitForExit();
            if (!line.StartsWith("{"))
                return (null, stderr.Result.Trim().Split('\n').LastOrDefault()?.Trim() ?? "the save tool gave no answer");
            var doc = JsonDocument.Parse(line);
            if (doc.RootElement.TryGetProperty("error", out var e))
            {
                doc.Dispose();
                return (null, e.GetString() ?? "the save tool failed");
            }
            return (doc, "");
        }
        catch (Exception ex)
        {
            return (null, "could not run the save tool: " + ex.Message);
        }
    }
}
