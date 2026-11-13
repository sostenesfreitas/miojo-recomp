namespace HookshotBuilder;

/// What the player chose last time (the install folder, the game files, the shaders' tick): builder-settings.json beside the versions
sealed class BuilderSettings
{
    public string? Out { get; set; }
    public string? Nup { get; set; }
    public string? Romfs { get; set; }
    public string? Exefs { get; set; }
    public bool Dlss { get; set; }   // (NVIDIA DLSS ticked: NVIDIA's license was accepted)
    public bool DlssLicense { get; set; }   // (NVIDIA's license accepted on the DLSS tick: what keeps an update's DLSS, not the SDK's files)
    public bool Fsr { get; set; }    // (AMD FSR 4 ticked)
    public bool FsrLicense { get; set; }   // (AMD's license accepted when it was ticked: a tick saved before it was asked has not)
    public bool PrepareLater { get; set; }   // ("Prepare shaders now" unticked)
    public int? MemoryGb { get; set; }
    public string? Track { get; set; }   // (the update track chosen, Updater.cs: release, rc or nightly; null: these tools' own)   // (Build memory; null: Automatic. The updates use it too, as they show no menu)

    public static BuilderSettings? Load(string path)
    {
        try { return System.Text.Json.JsonSerializer.Deserialize<BuilderSettings>(System.IO.File.ReadAllText(path)); }
        catch (Exception) { return null; }
    }

    /// (the updates, from the launcher's Update and from the --update start after a self-update, begin before ApplySettings sets
    /// the tick from these settings: they read the saved choice itself. None saved: prepared, as the tick's default)
    public static bool PrepareNow(string path) => Load(path)?.PrepareLater != true;
}
