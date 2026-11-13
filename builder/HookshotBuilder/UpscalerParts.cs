using System.IO;

namespace HookshotBuilder;

/// The upscalers ticked here that a built game folder lacks: a game is built with NVIDIA DLSS or AMD FSR 4 or without it,
/// so ticking one after the game was built changes nothing until the folder is built again. The Builder then offers Update
/// (the game files the folder keeps, built again with what is ticked) instead of Play
static class UpscalerParts
{
    /// What the folder lacks of the ticked upscalers, by name, in the order they are ticked on the page; empty when it has them.
    /// NVIDIA DLSS: make_game.py puts NVIDIA's runtime in the folder's ngx folder only when it builds with NVIDIA's SDK
    /// (place_ngx_runtime). AMD FSR 4: make_game.py marks a folder built with it (.hookshot_fsr); with AMD's license accepted
    /// the build also fetches AMD's runtime, so a folder built with FSR 4 without AMD's DLLs (beside the exe or in the folder,
    /// where gpu/vk/vk_fsr4.cpp looks) lacks it too. Without the license, the DLLs are the player's to put there
    public static List<string> Missing(string folder, bool dlss, bool fsr, bool fsrLicense)
    {
        var missing = new List<string>();
        try
        {
            if (dlss && !File.Exists(Path.Combine(folder, "ngx", "nvngx_dlss.dll"))) missing.Add("NVIDIA DLSS");
            if (fsr && (!File.Exists(Path.Combine(folder, ".hookshot_fsr")) || fsrLicense && !HasFsrRuntime(folder)))
                missing.Add("AMD FSR 4");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException) { }
        return missing;
    }

    static bool HasFsrRuntime(string folder) =>
        File.Exists(Path.Combine(folder, "amd_fidelityfx_upscaler_vk.dll")) ||
        File.Exists(Path.Combine(folder, "bin", "amd_fidelityfx_upscaler_vk.dll"));
}
