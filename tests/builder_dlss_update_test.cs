using HookshotBuilder;

sealed class Tools;
static class Storage { public static string NgxSdk = ""; }
sealed class Step { public void Done() { } public void Small(string value) { } }

// Only the UI and the build launcher are spies: UpdateBuild, settings and shader arguments are production code.
sealed class Window
{
    object? download = new();
    readonly Dictionary<string, Step> rows = new() { ["download"] = new Step() };
    public static string SettingsPath = "";
    public bool prepareNow = true;
    public bool? Dlss, Fsr;
    public List<string>? Args;
    void Run(Tools tools, List<string> args, bool keepRows, bool dlss, bool fsr = false)
    {
        if (!keepRows || download != null) throw new Exception("Update setup changed.");
        Dlss = dlss;
        Fsr = fsr;
        Args = args;
    }
    /* ACTUAL_UPDATE */
}

static class Program
{
    static int checks;
    static void Check(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
        checks++;
    }

    static void Update(string game, bool expected, bool shaders = true, bool fsr = false)
    {
        var w = new Window { prepareNow = shaders };
        w.UpdateBuild(new Tools(), game);
        Check(w.Dlss == expected, "Wrong DLSS decision for the saved choice and existing installation.");
        Check(w.Fsr == fsr, "Wrong AMD FSR 4 decision for the saved choice and existing installation.");
        var args = new List<string> { "--update", game };
        if (!shaders) args.Add("--no-prepare-shaders");
        Check(w.Args!.SequenceEqual(args), "The DLSS choice changed the update/shader arguments.");
    }

    static void Main()
    {
        // Authored placeholders only: no NVIDIA SDK, runtime or game data is used.
        var root = Path.Combine(Path.GetTempPath(), "hookshot-dlss-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            Window.SettingsPath = Path.Combine(root, "builder-settings.json");
            Storage.NgxSdk = Path.Combine(root, "sdk without files");
            var game = Path.Combine(root, "game with spaces é");
            Directory.CreateDirectory(game);
            // Simulates the saved, license-accepted checkbox before a self-update creates a new window.
            File.WriteAllText(Window.SettingsPath, "{\"Dlss\":true}");
            Update(game, true);
            Update(game, true, shaders: false);
            Check(!Directory.Exists(Storage.NgxSdk), "The decision unexpectedly provisioned an SDK.");
            Check(!Directory.Exists(Path.Combine(game, "ngx")), "The decision unexpectedly modified the game.");
            File.WriteAllText(Window.SettingsPath, "{\"Dlss\":false}");
            Update(game, false);
            File.WriteAllText(Window.SettingsPath, "{}");
            Update(game, false);
            File.WriteAllText(Window.SettingsPath, "not json");
            Update(game, false);
            File.Delete(Window.SettingsPath);
            Update(game, false);
            Directory.CreateDirectory(Storage.NgxSdk);
            File.WriteAllText(Path.Combine(Storage.NgxSdk, "LICENSE.txt"), "authored license placeholder");
            Update(game, false); // A license alone did not build this game's DLSS.
            Directory.CreateDirectory(Path.Combine(game, "ngx"));
            var runtime = Path.Combine(game, "ngx", "nvngx_dlss.dll");
            File.WriteAllText(runtime, "authored runtime placeholder");
            Update(game, true); // Preserve the existing update behavior for an opted-in installation.
            File.WriteAllText(Window.SettingsPath, "{\"Dlss\":false}");
            Update(game, true);
            File.Delete(Path.Combine(Storage.NgxSdk, "LICENSE.txt"));
            Update(game, false); // A leftover DLL alone does not accept NVIDIA's license.
            File.WriteAllText(Window.SettingsPath, "{\"Dlss\":false,\"DlssLicense\":true}");
            Update(game, true); // The license accepted is in the settings: the SDK's files gone (deleted, a new builder folder) keep DLSS.
            File.WriteAllText(Window.SettingsPath, "{\"DlssLicense\":true}");
            Update(game, true);
            File.WriteAllText(Window.SettingsPath, "{\"Dlss\":true}");
            Update(game, true); // The accepted preference also repairs a partial SDK.
            Check(File.ReadAllText(runtime) == "authored runtime placeholder", "Update decision modified the existing runtime.");

            // AMD FSR 4, the same rule: the saved, license-accepted tick, or a folder built with FSR 4 whose AMD runtime
            // was fetched (AMD's license saved beside it). NVIDIA's license is not there here, so no DLSS. Authored
            // placeholders only: none of AMD's files are used.
            File.WriteAllText(Window.SettingsPath, "{\"Fsr\":true,\"FsrLicense\":true}");
            Update(game, false, fsr: true);
            File.WriteAllText(Window.SettingsPath, "{\"Fsr\":true}");
            Update(game, false); // A tick saved before AMD's license was asked fetches nothing.
            File.WriteAllText(Window.SettingsPath, "{\"Fsr\":false}");
            Update(game, false);
            File.WriteAllText(Path.Combine(game, ".hookshot_fsr"), "");
            Update(game, false); // A folder built with FSR 4, its DLLs the player's own: nothing to fetch.
            File.WriteAllText(Path.Combine(game, "amd_fidelityfx_upscaler_vk.dll"), "authored upscaler placeholder");
            Update(game, false); // A DLL alone does not accept AMD's license.
            File.WriteAllText(Path.Combine(game, "LICENSE-AMD-FSR-SDK.md"), "authored license placeholder");
            Update(game, false, fsr: true);
            File.Delete(Path.Combine(game, ".hookshot_fsr"));
            Update(game, false); // Built without FSR 4 since: not opted back in.
            Console.WriteLine($"Builder DLSS update checks passed ({checks} assertions).");
        }
        finally { Directory.Delete(root, recursive: true); }
    }
}
