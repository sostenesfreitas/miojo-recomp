using HookshotBuilder;

// UpscalerParts.Missing over authored placeholder folders: no game, no NVIDIA or AMD file is used.
static class Program
{
    static int checks;
    static void Check(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
        checks++;
    }

    static void Is(string game, bool dlss, bool fsr, bool license, params string[] expected)
    {
        var got = UpscalerParts.Missing(game, dlss, fsr, license);
        Check(got.SequenceEqual(expected), $"dlss={dlss} fsr={fsr} license={license}: [{string.Join(", ", got)}], " +
                                           $"not [{string.Join(", ", expected)}]");
    }

    static void Main()
    {
        var root = Path.Combine(Path.GetTempPath(), "hookshot-upscalers-" + Guid.NewGuid().ToString("N"));
        var game = Path.Combine(root, "game with spaces é");
        Directory.CreateDirectory(game);
        try
        {
            Is(game, false, false, false);   // (nothing ticked: nothing lacking, Play)
            Is(game, true, false, false, "NVIDIA DLSS");
            Is(game, false, true, false, "AMD FSR 4");
            Is(game, true, true, true, "NVIDIA DLSS", "AMD FSR 4");   // (in the page's order)

            Directory.CreateDirectory(Path.Combine(game, "ngx"));
            File.WriteAllText(Path.Combine(game, "ngx", "nvngx_dlss.dll"), "authored runtime placeholder");
            Is(game, true, false, false);   // (built with NVIDIA's SDK: its runtime in the ngx folder)
            File.WriteAllText(Path.Combine(game, "nvngx_dlss.dll"), "authored placeholder beside the folder");
            Directory.Delete(Path.Combine(game, "ngx"), recursive: true);
            Is(game, true, false, false, "NVIDIA DLSS");   // (a DLL copied elsewhere is not a build with DLSS)

            File.WriteAllText(Path.Combine(game, ".hookshot_fsr"), "");
            Is(game, false, true, false);   // (built with FSR 4, the DLLs the player's own: no license, nothing to fetch)
            Is(game, false, true, true, "AMD FSR 4");   // (license accepted, AMD's runtime not there: the build fetches it)
            Directory.CreateDirectory(Path.Combine(game, "bin"));
            File.WriteAllText(Path.Combine(game, "bin", "amd_fidelityfx_upscaler_vk.dll"), "authored placeholder");
            Is(game, false, true, true);   // (beside the exe, where gpu/vk/vk_fsr4.cpp looks first)
            File.Delete(Path.Combine(game, "bin", "amd_fidelityfx_upscaler_vk.dll"));
            File.WriteAllText(Path.Combine(game, "amd_fidelityfx_upscaler_vk.dll"), "authored placeholder");
            Is(game, false, true, true);   // (in the game folder, where make_game.py --fetch-fsr-runtime puts it)
            File.Delete(Path.Combine(game, ".hookshot_fsr"));
            Is(game, false, true, true, "AMD FSR 4");   // (DLLs alone: the game was not built with FSR 4)

            Is(Path.Combine(root, "no such folder"), true, true, true, "NVIDIA DLSS", "AMD FSR 4");
            Is("", true, false, false, "NVIDIA DLSS");   // (no folder chosen: as missing, no exception)
            Console.WriteLine($"Builder upscaler checks passed ({checks} assertions).");
        }
        finally { Directory.Delete(root, recursive: true); }
    }
}
