using System.Diagnostics;
using System.Text;
using HookshotBuilder;

static class Program
{
    static int checks;
    static void Check(bool value, string why)
    {
        checks++;
        if (!value) throw new Exception(why);
    }
    static void Refuses(Action action, string why)
    {
        try { action(); }
        catch (Exception e) when (e is IOException or ArgumentException) { checks++; return; }
        throw new Exception(why);
    }
    static void Main(string[] args)
    {
        var fixture = Path.GetFullPath(args[0]);
        var volume = Path.Combine(fixture, "authored volume é, one");
        var root = Path.Combine(volume, "HookshotBuilder");
        var app = Path.Combine(root, "versions", "v-authored", "HookshotBuilder");
        var standard = Path.Combine(fixture, "standard");
        Directory.CreateDirectory(app);
        File.WriteAllText(Path.Combine(root, BuilderHome.Marker), BuilderHome.MarkerText + "\n", new UTF8Encoding(false));
        var home = BuilderHome.Resolve(null, app, standard);
        if (args.Length > 1)
            Check(BuilderHome.Resolve(args[1], app, standard, true).Portable, "C# reader rejected the native marker.");
        Check(home.Root == root && home.Portable, "Direct app start lost the portable home.");
        Check(BuilderHome.Resolve(root, app, standard, true) == home, "Child root handoff disagrees with direct start.");
        Check(BuilderHome.Resolve(root, app, standard) == home, "An explicit marked root lost its mode.");
        Check(BuilderHome.Resolve(standard, app, standard) == new BuilderHome.Home(standard, false), "Explicit root did not win.");
        Check(BuilderHome.Resolve(null, Path.Combine(fixture, "normal app"), standard) == new BuilderHome.Home(standard, false), "Default install changed.");
        var otherCwd = Path.Combine(fixture, "unrelated cwd");
        Directory.CreateDirectory(otherCwd);
        Directory.SetCurrentDirectory(otherCwd);
        Check(BuilderHome.Resolve(null, app, standard) == home, "Home discovery trusted cwd.");
        Refuses(() => BuilderHome.Resolve("relative home", app, standard), "Relative home accepted.");
        Refuses(() => BuilderHome.Resolve(@"\\?\C:\authored", app, standard), "Device namespace accepted.");
        Refuses(() => BuilderHome.Resolve(@"\\.\authored", app, standard), "Device path accepted.");
        Refuses(() => BuilderHome.Resolve(null, app, standard, true), "Missing child root silently fell back.");
        Refuses(() => BuilderHome.Resolve(standard, app, standard, true), "Missing portable marker silently fell back.");
        Refuses(() => BuilderHome.Resolve(Path.GetPathRoot(root)! + new string('x', 270), app, standard), "Unrepresentable native root accepted.");

        Environment.SetEnvironmentVariable("HOOKSHOT_SETUP_BASE", root);
        Environment.SetEnvironmentVariable("HOOKSHOT_BUILDER_PORTABLE", "1");
        Check(BuilderHome.Base == root && BuilderHome.Portable, "Actual process context chose another home.");
        Check(Storage.Base == root && Storage.NgxSdk == Path.Combine(root, "ngx-sdk") && Storage.FfxSdk == Path.Combine(root, "ffx-sdk"), "Storage/SDKs escaped the home.");
        Check(Storage.Work == root, "Default work escaped the home.");
        Check(BuilderHome.DefaultGame == Path.Combine(volume, "Hookshot"), "Default game escaped the portable volume.");
        var child = new ProcessStartInfo("authored-child-never-started");
        child.Environment["HOOKSHOT_SETUP_BASE"] = Path.Combine(fixture, "stale unrelated home");
        child.Environment["HOOKSHOT_BUILDER_PORTABLE"] = "0";
        BuilderHome.PassTo(child);
        Check(child.Environment["HOOKSHOT_SETUP_BASE"] == root && child.Environment["HOOKSHOT_BUILDER_PORTABLE"] == "1", "Child context was not passed.");

        var game = Path.Combine(volume, "Games", "authored game");
        var saved = BuilderHome.SavePath(game);
        Check(saved != game && saved != null && !Path.IsPathRooted(saved), "Portable choice was stored as an absolute path.");
        Check(BuilderHome.LoadPath(saved) == game, "Saved portable path failed round trip.");
        Check(BuilderHome.LoadPath(BuilderHome.SavePath("relative choice")) == Path.Combine(root, "relative choice"), "Relative choice used cwd.");
        Check(BuilderHome.SavePath(null) == null && BuilderHome.LoadPath("") == "", "Empty choices changed.");
        var drive = Path.GetPathRoot(root)!;
        var external = (char.ToUpperInvariant(drive[0]) == 'Z' ? "Y" : "Z") + @":\authored external\package.nup";
        Check(BuilderHome.SavePath(external) == external && BuilderHome.LoadPath(external) == external, "External volume was remapped.");
        Refuses(() => BuilderHome.LoadPath(@"\ambiguous"), "Drive-less rooted path accepted.");
        Refuses(() => BuilderHome.SavePath("C:ambiguous"), "Drive-relative path accepted.");

        var moved = Path.Combine(fixture, "authored volume é, moved");
        Directory.Move(volume, moved);
        var newRoot = Path.Combine(moved, "HookshotBuilder");
        var next = BuilderHome.Resolve(null, Path.Combine(newRoot, "versions", "v-authored", "HookshotBuilder"), standard);
        Check(next.Root == newRoot && next.Portable, "Fresh direct start after relocation chose another home.");
        Check(BuilderHome.LoadPath(next, saved) == Path.Combine(moved, "Games", "authored game"), "Relocated choice kept its old prefix.");
        File.WriteAllText(Path.Combine(newRoot, BuilderHome.Marker), "not a portable marker");
        Refuses(() => BuilderHome.Resolve(null, Path.Combine(newRoot, "versions", "v-authored", "HookshotBuilder"), standard), "Invalid marker silently chose AppData.");
        File.WriteAllBytes(Path.Combine(newRoot, BuilderHome.Marker), new byte[] { 0xff });
        Refuses(() => BuilderHome.Resolve(newRoot, app, standard), "Malformed marker accepted.");
        File.WriteAllText(Path.Combine(newRoot, BuilderHome.Marker), BuilderHome.MarkerText + new string('x', 10000));
        Refuses(() => BuilderHome.Resolve(newRoot, app, standard), "Oversized marker accepted.");
        File.WriteAllBytes(Path.Combine(newRoot, BuilderHome.Marker), Encoding.UTF8.GetPreamble().Concat(Encoding.UTF8.GetBytes(BuilderHome.MarkerText)).ToArray());
        Refuses(() => BuilderHome.Resolve(newRoot, app, standard), "BOM changed the native byte contract.");
        var shared = Path.Combine(fixture, "csharp shared");
        Directory.CreateDirectory(shared);
        File.WriteAllText(Path.Combine(shared, BuilderHome.Marker), BuilderHome.MarkerText + "\n", new UTF8Encoding(false));
        Console.WriteLine($"PASS: {checks} portable home/storage/path/context assertions; no process started");
    }
}
