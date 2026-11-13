using System.Diagnostics;
using System.IO;
using Path = System.Windows.Shapes.Path;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using System.Windows.Shapes;
using System.Windows.Threading;
using Microsoft.Win32;

namespace HookshotBuilder;

/// The installer: one 960x600 window. It opens with the hookshot in the middle while it looks for the game files, then walks
/// through Game files > Build > Play. The game's update offer (Updater.cs) opens the same window on its update screen.
public partial class MainWindow : Window
{
    const long NeededBytes = 45L << 30;   // (measured 2026-11-09: the game data 24.4 GB, the shader caches ~8 GB, the game 0.4 GB, the build files ~5 GB at their peak; the rest headroom)
    Language lang = Strings.Default();
    string T(S s) => (int)s < lang.Text.Length ? lang.Text[(int)s] : Strings.All[0].Text[(int)s];

    readonly DispatcherTimer clock = new() { Interval = TimeSpan.FromSeconds(1) };
    readonly Stopwatch elapsed = new();
    readonly Stopwatch compileClock = new();
    readonly Dictionary<string, StepRow> rows = new();
    readonly Dictionary<string, (bool Ok, string Text)?> checks = new(StringComparer.OrdinalIgnoreCase);
    readonly List<Dump> dumps = new();
    readonly System.Text.StringBuilder log = new();
    Runner? runner;
    string? current;
    string finished = "";
    string? chosenNup, romfs, exefs;
    // (issue 650) "Using the folders ..." above the list of .nup files, while romfs and exefs are the game files: FillDumps clears the
    // list, so the note is made there again each time (it was gone as soon as the list was first filled)
    string? foldersNote;
    TextBlock? foldersNoteBlock;
    bool updating, checking;
    double compiled;
    int filesDone, filesOf;
    int filesChanged = -1, filesCompiled;
    double compileEta = -1;
    long thisCompile, previousTotal = -1;
    string lastFailure = "";

    public MainWindow()
    {
        InitializeComponent();
        OutPath.Text = BuilderHome.DefaultGame;
        if (BuilderHome.Portable) DesktopTick.IsChecked = StartTick.IsChecked = false;
        LanguageBox.ItemsSource = Strings.All;
        LanguageBox.SelectedItem = lang;
        WorkerBox.ItemsSource = BuildWorkers.Choices(Environment.ProcessorCount);
        WorkerBox.SelectedIndex = 0;
        MemoryBox.ItemsSource = BuildMemory.Choices(BuildMemory.TotalGb());
        MemoryBox.SelectedIndex = 0;
        TrackBox.ItemsSource = TrackNames;
        TrackBox.SelectedIndex = Array.IndexOf(TrackIds, Track);
        TrackBox.SelectionChanged += TrackChanged;
        clock.Tick += (_, _) => Tick();
        Drop += Dropped;
        SourceInitialized += (_, _) =>
        {
            if (!Glass.Apply(this))   // (Windows 10, or no acrylic: plain Slate)
            {
                Background = (Brush)FindResource("Page");
                Root.Background = (Brush)FindResource("Page");
            }
        };
        Loaded += (_, _) => Start();
        var tools = Tools.Find();
        ToolsNote.Text = tools != null
            ? "Nothing else to install: Hookshot's own build tools are here, downloaded and checked by its setup."
            : "Hookshot's build tools are missing from its folder. Press Build and Hookshot offers to download them again.";
        SetStep(0);
        Refresh();
    }

    // ================= the start =================

    async void Start()
    {
        // (development) HOOKSHOT_TEST_SHORTCUTS=<game folder>: both shortcuts made for it, the result in the log beside the app, and out
        if (Environment.GetEnvironmentVariable("HOOKSHOT_TEST_SHORTCUTS") is { Length: > 0 } testFolder)
        {
            Finder.Shortcuts(testFolder, true, true);
            File.WriteAllText(System.IO.Path.Combine(AppContext.BaseDirectory, "shortcut-test.txt"),
                              $"desktop {File.Exists(Finder.DesktopLink)} start {File.Exists(Finder.StartLink)} error {Finder.ShortcutError}");
            Application.Current.Shutdown();
            return;
        }
        Animate();
        var started = Stopwatch.StartNew();
        if (Environment.GetCommandLineArgs().Contains("--update"))
        {
            SplashText.Text = "Getting the update…";
            await Task.Delay(900);
            ShowMain();
            Updater.FromCommandLine(this);
            return;
        }
        if (LaunchArg() is { } launchFolder)   // (Hookshot Launcher.exe in the game folder)
        {
            SplashText.Text = "Starting Hookshot…";
            await Task.Delay(700);
            ShowMain();
            ShowLauncher(launchFolder);
            return;
        }
        SplashText.Text = "Looking for your game files…";
        var found = await Task.Run(Finder.FindDumps);
        dumps.AddRange(found);
        SplashText.Text = found.Count switch { 0 => "No game files found yet", 1 => "Found 1 game file", _ => $"Found {found.Count} game files" };
        var wait = 1700 - (int)started.ElapsedMilliseconds;
        if (wait > 0) await Task.Delay(wait);
        ApplySettings();
        FillDumps();
        filesReady = true;
        ShowMain();
        if (Environment.GetEnvironmentVariable("HOOKSHOT_UI_PREVIEW") is { Length: > 0 } preview) { Preview(preview); return; }
        CheckAll();
        latestTag = Environment.GetEnvironmentVariable("HOOKSHOT_LATEST_TAG") is { Length: > 0 } fake ? (fake == "none" ? null : fake) : await Updater.LatestTag(Track);
        RefreshMode();
    }

    bool filesReady;   // (the first page has its game files and saved choices: a start with --update or --launch goes past that)

    /// The first page again, from a build or an update that stopped or was cancelled. RefreshMode leaves the button alone while
    /// an update runs, and one that stopped left it so for good: the page kept the button it had before. After a start with
    /// --update (the game's or the setup's, once the new builder is in) that was a grey "Build the game" over a page with no
    /// game files and no saved choices, and the Update button only came back when the app was closed and opened again (#2129).
    /// The update is over here: the page is set up if it never was, for the update's folder, and its button says what that needs
    async void ToFiles()
    {
        SetStep(0);
        ShowPage(FilesPage);
        if (!updating) return;
        updating = false;
        if (!filesReady)
        {
            filesReady = true;
            var folder = OutPath.Text;   // (the update's: the saved one is the last build's)
            dumps.AddRange(await Task.Run(Finder.FindDumps));
            ApplySettings();
            OutPath.Text = folder;
            FillDumps();
            CheckAll();
            latestTag ??= Environment.GetEnvironmentVariable("HOOKSHOT_LATEST_TAG") is { Length: > 0 } fake ? (fake == "none" ? null : fake) : await Updater.LatestTag(Track);
        }
        Refresh();
    }

    // ================= what the button does: build, update the game, play it, or get the newer builder =================
    // (as NX2navi's launcher: whatever version the player opens, it checks the latest release and says what is there)

    enum Mode { Build, UpdateGame, UpToDate, UpdateBuilder }
    Mode mode = Mode.Build;
    string? latestTag, updateTarget;
    bool sourcePicked;   // (the player chose game files here: a build from them, even over a game that is up to date)

    /// The release a game folder was built with (its hookshot_version.txt), when it keeps its game files for updates
    static string? GameVersion(string folder)
    {
        try
        {
            if (!Directory.Exists(System.IO.Path.Combine(folder, ".hookshot_exefs")) || !Directory.Exists(System.IO.Path.Combine(folder, "romfs")))
                return null;
            foreach (var line in File.ReadAllLines(System.IO.Path.Combine(folder, "hookshot_version.txt")))
                if (line.StartsWith("version=")) return line[8..].Trim() is { Length: > 0 } v ? v : null;
        }
        catch (Exception) { }
        return null;
    }

    void RefreshMode()
    {
        if (BuildButton == null || updating || runner != null) return;
        var game = OutPath.Text.Length > 2 ? GameVersion(OutPath.Text) : null;
        var builder = Tools.Find()?.Version;
        // (the newest release known: GitHub's, else this builder's own. v2.0.0 took a GitHub that did not answer for "up to date")
        updateTarget = Updater.Target(latestTag, builder, Track);
        if (game != null && updateTarget != null && game != updateTarget && !sourcePicked) mode = Mode.UpdateGame;
        else if (game != null && !sourcePicked) mode = Mode.UpToDate;
        else if (latestTag != null && builder != null && builder != updateTarget) mode = Mode.UpdateBuilder;
        else mode = Mode.Build;
        switch (mode)
        {
            case Mode.UpdateGame:
                FilesTitle.Text = "Update available";
                FilesSub.Text = $"Hookshot {updateTarget} is available. Your game is {game}: Update builds it again from the game files kept in " +
                                "its folder, compiling only what changed.";
                BuildButton.Content = "Update";
                break;
            case Mode.UpToDate:
                FilesTitle.Text = "Your game is up to date";
                FilesSub.Text = latestTag == null
                    ? $"Your game is {game}, as this builder. GitHub did not answer, so newer versions could not be checked."
                    : $"{game} is the latest version. Pick game files below only to build it again from scratch.";
                BuildButton.Content = "Play";
                break;
            case Mode.UpdateBuilder:
                FilesTitle.Text = "Build your game";
                FilesSub.Text = $"Hookshot Builder {updateTarget} is available (this one is {builder}): update it first, then build.";
                BuildButton.Content = "Update builder";
                break;
            default:
                FilesTitle.Text = "Build your game";
                FilesSub.Text = "Pick your game files and where the game goes. Hookshot does the rest, once.";
                BuildButton.Content = "Build the game";
                break;
        }
        BuildButton.IsEnabled = mode != Mode.Build || (SourceReady && OutPath.Text.Length > 2);
        AboutText.Visibility = mode is Mode.Build ? Visibility.Visible : Visibility.Collapsed;
    }

    // ---- the launcher: Hookshot Launcher.exe in a game folder starts this with --launch <folder> (builder/setup/setup.c) ----
    string launchFolder = "";

    static string? LaunchArg()
    {
        var a = Environment.GetCommandLineArgs();
        var i = Array.IndexOf(a, "--launch");
        return i >= 0 && i + 1 < a.Length ? a[i + 1].TrimEnd('\\', '/') : null;
    }

    async void ShowLauncher(string folder)
    {
        launchFolder = folder;
        OutPath.Text = folder;
        StepPill.Visibility = Visibility.Collapsed;
        var game = GameVersion(folder) ?? ReadVersion(folder);
        LaunchVersion.Text = (game != null ? $"Hookshot {game}" : "Hookshot") + " · " + folder;
        LaunchPlay.IsEnabled = File.Exists(GameExe(folder));
        ShowPage(LaunchPage);
        latestTag = Environment.GetEnvironmentVariable("HOOKSHOT_LATEST_TAG") is { Length: > 0 } fake ? (fake == "none" ? null : fake) : await Updater.LatestTag(Track);
        updateTarget = Updater.Target(latestTag, Tools.Find()?.Version, Track);   // (GitHub's newest, else this builder's own)
        if (updateTarget == null) LaunchStatus.Text = "Could not check for updates (GitHub did not answer)";
        else if (game != null && game != updateTarget && GameVersion(folder) != null)
        {
            LaunchStatus.Text = $"Update available: {updateTarget}";
            LaunchStatus.Foreground = new SolidColorBrush(Color.FromRgb(0xB9, 0xC8, 0xFF));
            LaunchStatus.FontWeight = FontWeights.SemiBold;
            LaunchUpdate.Visibility = Visibility.Visible;
        }
        else if (game != null && game != updateTarget)
            LaunchStatus.Text = $"{updateTarget} is out: build this folder again with the Hookshot Builder to get it";
        else LaunchStatus.Text = latestTag == null ? "Up to date with this builder (GitHub did not answer)" : "Up to date";
    }

    /// (a folder built before it kept its game files has a version too: only shown)
    static string? ReadVersion(string folder)
    {
        try
        {
            foreach (var line in File.ReadAllLines(System.IO.Path.Combine(folder, "hookshot_version.txt")))
                if (line.StartsWith("version=")) return line[8..].Trim() is { Length: > 0 } v ? v : null;
        }
        catch (Exception) { }
        return null;
    }

    /// The game in a folder: bin\hookshot.exe below its launcher (hookshot.exe), or hookshot.exe in a folder built before the launcher
    static string GameExe(string folder)
    {
        var game = System.IO.Path.Combine(folder, "bin", "hookshot.exe");
        return File.Exists(game) ? game : System.IO.Path.Combine(folder, "hookshot.exe");
    }

    /// (the launcher already checked for updates: the game does not ask again)
    static void StartGame(string folder)
    {
        var si = new ProcessStartInfo(GameExe(folder)) { UseShellExecute = false, WorkingDirectory = folder };
        si.Environment["HOOKSHOT_NO_UPDATE_CHECK"] = "1";
        Process.Start(si);
    }

    void LaunchUpdateClick(object sender, RoutedEventArgs e)
    {
        if (OlderOk(GameVersion(launchFolder))) Updater.UpdateFolder(this, launchFolder, updateTarget, fetched: false, named: Switching);
    }

    // ---- the update track (Updater.cs): the one chosen in the Track menu, saved; by default these tools' own ----
    static readonly string[] TrackIds = { Updater.Release, Updater.Candidate, Updater.Nightly };
    static readonly string[] TrackNames = { "Release", "Release candidate", "Nightly" };
    string? chosenTrack;
    string Track
    {
        get
        {
            if (chosenTrack != null) return chosenTrack;
            var saved = BuilderSettings.Load(SettingsPath)?.Track;
            return saved != null && TrackIds.Contains(saved) ? saved : Updater.TrackOf(Tools.Find()?.Version);
        }
    }
    /// (a track other than these tools' own: its newest is installed, even an older version)
    bool Switching => Track != Updater.TrackOf(Tools.Find()?.Version);

    async void TrackChanged(object sender, SelectionChangedEventArgs e)
    {
        if (TrackBox.SelectedIndex < 0 || TrackIds[TrackBox.SelectedIndex] == Track) return;
        chosenTrack = TrackIds[TrackBox.SelectedIndex];
        SaveSettings();
        latestTag = null;
        RefreshMode();
        latestTag = await Updater.LatestTag(Track);
        RefreshMode();
    }

    /// (going to an older version, from another track: asked first)
    bool OlderOk(string? have) =>
        !Switching || !Updater.Newer(have, updateTarget) ||
        MessageBox.Show(this, $"{updateTarget} is older than {have}, the version you have now. Install {updateTarget}?", "Hookshot",
                        MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes;

    void LaunchPlayClick(object sender, RoutedEventArgs e)
    {
        StartGame(launchFolder);
        Application.Current.Shutdown();
    }

    void LaunchFolderClick(object sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo(launchFolder) { UseShellExecute = true });

    // ---- Save my pipeline list: the game folder's pipeline list and the "Submit your pipelines" issue's template, filled in,
    // zipped into the Downloads folder by nx2/pipeline_share.py (as the Linux builder and the game's menu do), then the template
    // on the clipboard, the zip shown in its folder and the issue opened. Nothing is uploaded: the player attaches the zip ----
    const string ShareIssue = "https://github.com/NotRexed/Hookshot/issues/2034";

    async void LaunchShareClick(object sender, RoutedEventArgs e)
    {
        var tools = Tools.Find();
        LaunchShareStatus.Visibility = Visibility.Visible;
        if (tools == null)
        {
            LaunchShareStatus.Text = "The builder's tools were not found: start it again from its own folder.";
            return;
        }
        LaunchShare.IsEnabled = false;
        LaunchShareStatus.Text = "Saving…";
        var folder = launchFolder;
        var (zip, template, count, error) = await Task.Run(() => SharePipelines(tools, folder, DownloadsFolder()));
        LaunchShare.IsEnabled = true;
        if (zip == null)
        {
            LaunchShareStatus.Text = error ?? "The pipeline list could not be saved.";
            return;
        }
        var copied = false;
        try { Clipboard.SetText(template ?? ""); copied = true; } catch (Exception) { }
        var explorer = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "explorer.exe");   // (not one found on the path)
        try { Process.Start(new ProcessStartInfo(File.Exists(explorer) ? explorer : "explorer.exe", $"/select,\"{zip}\"") { UseShellExecute = true }); } catch (Exception) { }
        var opened = false;
        try { Process.Start(new ProcessStartInfo(ShareIssue) { UseShellExecute = true }); opened = true; } catch (Exception) { }
        LaunchShareStatus.Text = $"Saved {System.IO.Path.GetFileName(zip)} ({count} pipelines) in {System.IO.Path.GetDirectoryName(zip)}. " +
            (copied ? "The report is copied. " : "The report is in the zip. ") +
            (opened ? "Issue #2034 is open in your browser: attach" : $"Open {ShareIssue} and attach") +
            " the zip to a comment, then paste the report and say where you played.";
    }

    /// nx2/pipeline_share.py --json: (the zip, the template, the pipelines) or the reason there is none
    static (string? Zip, string? Template, int Count, string? Error) SharePipelines(Tools tools, string folder, string outDir)
    {
        var si = new ProcessStartInfo(tools.Python)
        {
            WorkingDirectory = tools.Root,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = System.Text.Encoding.UTF8,
        };
        si.Environment["PYTHONIOENCODING"] = "utf-8";
        foreach (var a in tools.PythonArgs ?? Array.Empty<string>()) si.ArgumentList.Add(a);
        si.ArgumentList.Add("-I");
        si.ArgumentList.Add(System.IO.Path.Combine(tools.Root, "nx2", "pipeline_share.py"));
        si.ArgumentList.Add(folder);
        si.ArgumentList.Add("--out-dir");
        si.ArgumentList.Add(outDir);
        si.ArgumentList.Add("--json");
        try
        {
            using var p = Process.Start(si)!;
            var stderr = p.StandardError.ReadToEndAsync();   // (drained, so a long complaint cannot stall it)
            var line = p.StandardOutput.ReadToEnd().Trim().Split('\n').LastOrDefault() ?? "";
            p.WaitForExit();
            if (!line.StartsWith("{"))   // (the script stopped before its answer: what it said last)
            {
                var last = stderr.Result.Trim().Split('\n').LastOrDefault()?.Trim();
                return (null, null, 0, "The pipeline list could not be saved" + (string.IsNullOrEmpty(last) ? "." : ": " + last));
            }
            using var doc = System.Text.Json.JsonDocument.Parse(line);
            var r = doc.RootElement;
            if (r.GetProperty("ok").GetBoolean())
                return (r.GetProperty("zip").GetString(), r.GetProperty("template").GetString(), r.GetProperty("pipelines").GetInt32(), null);
            return (null, null, 0, r.GetProperty("error").GetString());
        }
        catch (Exception ex)
        {
            return (null, null, 0, tools.OwnPythonMissing ? Tools.OwnPythonMissingText : "The pipeline list could not be saved: " + ex.Message);
        }
    }

    [System.Runtime.InteropServices.DllImport("shell32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, ExactSpelling = true)]
    static extern int SHGetKnownFolderPath([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPStruct)] Guid id,
                                           uint flags, IntPtr token, out string path);

    /// Where a browser's file picker opens: the Downloads folder, else the user's own
    static string DownloadsFolder()
    {
        try
        {
            if (SHGetKnownFolderPath(new Guid("374DE290-123F-4565-9164-39C4925E467B"), 0, IntPtr.Zero, out var path) == 0 && Directory.Exists(path))
                return path;
        }
        catch (Exception) { }
        return Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
    }

    // ---- what the player chose last time (BuilderSettings.cs): builder-settings.json beside the versions ----
    static string SettingsPath => System.IO.Path.Combine(Storage.Base, "builder-settings.json");

    void ApplySettings()
    {
        var s = BuilderSettings.Load(SettingsPath);
        if (s == null) return;
        s.Out = BuilderHome.LoadPath(s.Out);
        s.Nup = BuilderHome.LoadPath(s.Nup);
        s.Romfs = BuilderHome.LoadPath(s.Romfs);
        s.Exefs = BuilderHome.LoadPath(s.Exefs);
        PrepareTick.IsChecked = !s.PrepareLater;
        if (!string.IsNullOrEmpty(s.Out)) OutPath.Text = s.Out;
        DlssTick.IsChecked = s.Dlss;
        dlssLicense = s.DlssLicense;
        FsrTick.IsChecked = s.Fsr;
        fsrLicense = s.FsrLicense;
        MemoryBox.SelectedItem = BuildMemory.Saved(MemoryBox.ItemsSource.Cast<object>(), s.MemoryGb);
        if (s.Romfs != null && s.Exefs != null && Directory.Exists(s.Romfs) && File.Exists(System.IO.Path.Combine(s.Exefs, "main")))
        {
            romfs = s.Romfs;
            exefs = s.Exefs;
            foldersNote = $"Using the folders {romfs} and {exefs} (as last time)";
        }
        else if (s.Nup != null && File.Exists(s.Nup))
        {
            if (!dumps.Any(d => string.Equals(d.Path, s.Nup, StringComparison.OrdinalIgnoreCase)))
            {
                var info = new FileInfo(s.Nup);
                dumps.Insert(0, new Dump(info.FullName, "Last time", info.Length, info.LastWriteTime));
            }
            chosenNup = s.Nup;
        }
    }

    void SaveSettings()
    {
        try
        {
            var s = new BuilderSettings { Out = BuilderHome.SavePath(OutPath.Text),
                                          Nup = BuilderHome.SavePath(romfs == null ? chosenNup : null),
                                          Romfs = BuilderHome.SavePath(romfs), Exefs = BuilderHome.SavePath(exefs),
                                          Dlss = DlssTick.IsChecked == true, DlssLicense = dlssLicense,
                                          Fsr = FsrTick.IsChecked == true,
                                          FsrLicense = fsrLicense,
                                          PrepareLater = PrepareTick.IsChecked != true,
                                          MemoryGb = MemoryBox.SelectedItem is int gb ? gb : null,
                                          Track = chosenTrack ?? BuilderSettings.Load(SettingsPath)?.Track };
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(SettingsPath)!);
            File.WriteAllText(SettingsPath, System.Text.Json.JsonSerializer.Serialize(s));
        }
        catch (Exception) { }
    }

    /// (development) HOOKSHOT_UI_PREVIEW=build|ready|problem|update: one screen with made-up progress, nothing run
    void Preview(string what)
    {
        switch (what)
        {
            case "build":
                MakeRows(update: false, shaders: true);
                SetStep(1);
                ShowPage(BuildPage);
                rows["read"].Begin(); rows["read"].Done(); rows["read"].Small("Version checked");
                rows["lift"].Begin(); rows["lift"].Done();
                rows["compile"].Begin(); rows["compile"].Small("31 of 58 files");
                SetProgress(0.54);
                BuildNote.Text = "About 19 minutes left · your PC stays usable";
                break;
            case "ready":
                finished = OutPath.Text;
                ShowReady();
                break;
            case "problem":
                ShowProblem("Files of the builder's own compiler are missing (clang.exe in tools\\llvm-mingw\\bin).", tools: true);
                break;
            case "update":
                BeginUpdate(OutPath.Text, "honest-eta");
                rows["download"].Done(); rows["download"].Small("3 MB · build tools unchanged");
                rows["read"].Begin(); rows["read"].Done();
                rows["lift"].Begin(); rows["lift"].Done();
                rows["compile"].Begin(); rows["compile"].Small("7 of 516 files");
                SetProgress(0.62);
                BuildNote.Text = "About 3 minutes left. Your current game stays as it is until the new one is complete.";
                break;
        }
    }

    void Animate()
    {
        var forever = RepeatBehavior.Forever;
        var ease = new SineEase { EasingMode = EasingMode.EaseInOut };
        HaloScale.BeginAnimation(ScaleTransform.ScaleXProperty, new DoubleAnimation(0.94, 1.04, TimeSpan.FromSeconds(1.2)) { AutoReverse = true, RepeatBehavior = forever, EasingFunction = ease });
        HaloScale.BeginAnimation(ScaleTransform.ScaleYProperty, new DoubleAnimation(0.94, 1.04, TimeSpan.FromSeconds(1.2)) { AutoReverse = true, RepeatBehavior = forever, EasingFunction = ease });
        Halo.BeginAnimation(OpacityProperty, new DoubleAnimation(0.6, 1, TimeSpan.FromSeconds(1.2)) { AutoReverse = true, RepeatBehavior = forever, EasingFunction = ease });
        BobLift.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(0, -6, TimeSpan.FromSeconds(1.2)) { AutoReverse = true, RepeatBehavior = forever, EasingFunction = ease });
        BobTurn.BeginAnimation(RotateTransform.AngleProperty, new DoubleAnimation(-4, 2, TimeSpan.FromSeconds(1.2)) { AutoReverse = true, RepeatBehavior = forever, EasingFunction = ease });
        Sweep.BeginAnimation(TranslateTransform.XProperty, new DoubleAnimation(-88, 208, TimeSpan.FromSeconds(1.2)) { RepeatBehavior = forever, EasingFunction = ease });
    }

    /// The hookshot fades out and the first step fades in, in the same window
    void ShowMain()
    {
        var fade = new DoubleAnimation(0, TimeSpan.FromMilliseconds(450));
        fade.Completed += (_, _) => { Splash.Visibility = Visibility.Collapsed; StopSplash(); };
        Splash.BeginAnimation(OpacityProperty, fade);
        SplashScale.BeginAnimation(ScaleTransform.ScaleXProperty, new DoubleAnimation(1.04, TimeSpan.FromMilliseconds(500)));
        SplashScale.BeginAnimation(ScaleTransform.ScaleYProperty, new DoubleAnimation(1.04, TimeSpan.FromMilliseconds(500)));
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        Main.BeginAnimation(OpacityProperty, new DoubleAnimation(1, TimeSpan.FromMilliseconds(500)) { BeginTime = TimeSpan.FromMilliseconds(150) });
        MainLift.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(0, TimeSpan.FromMilliseconds(600)) { BeginTime = TimeSpan.FromMilliseconds(150), EasingFunction = ease });
        Main.IsHitTestVisible = true;
    }

    void StopSplash()
    {
        foreach (var (o, p) in new (Animatable, DependencyProperty)[] { (HaloScale, ScaleTransform.ScaleXProperty), (HaloScale, ScaleTransform.ScaleYProperty),
                     (BobLift, TranslateTransform.YProperty), (BobTurn, RotateTransform.AngleProperty), (Sweep, TranslateTransform.XProperty) })
            o.BeginAnimation(p, null);
        Halo.BeginAnimation(OpacityProperty, null);
    }

    // ================= the step pill: Game files > Build > Play =================

    void SetStep(int on)
    {
        StepItems.Children.Clear();
        var names = new[] { "Game files", "Build", "Play" };
        for (var i = 0; i < names.Length; i++)
        {
            if (i > 0)
                StepItems.Children.Add(new Path { Data = Geometry.Parse("M0,0 L4,4 L0,8"), Stroke = (Brush)FindResource("InkSoft"), StrokeThickness = 1.6,
                                                  Opacity = 0.45, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(5, 0, 5, 0) });
            var done = i < on;
            var text = new TextBlock { Text = names[i], FontSize = 13, VerticalAlignment = VerticalAlignment.Center,
                                       FontWeight = i == on ? FontWeights.SemiBold : FontWeights.Medium,
                                       Foreground = (Brush)FindResource(i == on ? "Ink" : done ? "InkSoft" : "InkFaint") };
            var inner = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
            if (done)
            {
                var tick = new Grid { Width = 22, Height = 22, Margin = new Thickness(0, 0, 8, 0) };
                tick.Children.Add(new Ellipse { Fill = (Brush)FindResource("GoSoft") });
                tick.Children.Add(new Path { Data = Geometry.Parse("M0,4 L3.5,7.5 L10,0"), Stroke = (Brush)FindResource("Go"), StrokeThickness = 1.8,
                                             HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center });
                inner.Children.Add(tick);
            }
            inner.Children.Add(text);
            var pill = new Border { Height = 30, CornerRadius = new CornerRadius(15), Padding = done ? new Thickness(4, 0, 14, 0) : new Thickness(14, 0, 14, 0), Child = inner };
            if (i == on)
            {
                text.Foreground = Brushes.White;
                pill.Background = new LinearGradientBrush(Color.FromRgb(0x4B, 0x70, 0xF0), Color.FromRgb(0x35, 0x57, 0xD6), 90);
                pill.Effect = new DropShadowEffect { Color = Color.FromRgb(0x3D, 0x63, 0xE6), BlurRadius = 18, ShadowDepth = 6, Direction = 270, Opacity = 0.6 };
            }
            StepItems.Children.Add(pill);
        }
        StepPill.Visibility = Visibility.Visible;
        VersionChip.Visibility = Visibility.Collapsed;
    }

    void ShowPage(FrameworkElement page)
    {
        foreach (var p in new FrameworkElement[] { FilesPage, BuildPage, ReadyPage, ProblemPage, LaunchPage })
            p.Visibility = p == page ? Visibility.Visible : Visibility.Collapsed;
        page.Opacity = 0;
        page.BeginAnimation(OpacityProperty, new DoubleAnimation(1, TimeSpan.FromMilliseconds(250)));
    }

    // ================= 1: game files =================

    void FillDumps()
    {
        DumpList.Children.Clear();
        foldersNoteBlock = null;
        if (romfs != null && exefs != null && foldersNote != null)
        {
            foldersNoteBlock = new TextBlock { Text = foldersNote, Style = (Style)FindResource("Note"), Margin = new Thickness(16, 12, 16, 12) };
            DumpList.Children.Add(foldersNoteBlock);
        }
        if (dumps.Count == 0)
        {
            DumpList.Children.Add(new TextBlock
            {
                Text = "No .nup files in Downloads, Desktop, Documents or the top folders of your drives. Browse to yours, or drop it on this window.",
                Style = (Style)FindResource("Note"), Margin = new Thickness(16, 14, 16, 14),
            });
            return;
        }
        for (var i = 0; i < dumps.Count; i++)
        {
            var d = dumps[i];
            if (i > 0) DumpList.Children.Add(new Border { Height = 1, Background = (Brush)FindResource("GlassLine") });
            var name = new TextBlock { Text = System.IO.Path.GetFileName(d.Path), FontWeight = FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
            var meta = new TextBlock { Text = $"{d.Where} · {Finder.Size(d.Size)} · {Finder.Ago(d.Modified)}", Style = (Style)FindResource("Faint") };
            var texts = new StackPanel { Children = { name, meta }, Margin = new Thickness(0, 0, 12, 0) };
            // (issue 563: "Can't be read" said why only in the chip's tooltip, which players did not find; the reason is in the row)
            if (checks.TryGetValue(d.Path, out var why) && why is { Ok: false } && !why.Value.Text.StartsWith("This is build"))
                texts.Children.Add(new TextBlock { Text = why.Value.Text, Style = (Style)FindResource("Faint"), TextWrapping = TextWrapping.Wrap,
                                                   Foreground = (Brush)FindResource("Bad"), Margin = new Thickness(0, 4, 0, 0) });
            var grid = new Grid { ColumnDefinitions = { new ColumnDefinition(), new ColumnDefinition { Width = GridLength.Auto } } };
            var chip = Chip(d.Path);
            Grid.SetColumn(chip, 1);
            grid.Children.Add(texts);
            grid.Children.Add(chip);
            var row = new RadioButton { Style = (Style)FindResource("DumpRow"), GroupName = "Dumps", Content = grid, Tag = d.Path, ToolTip = d.Path,
                                        IsChecked = string.Equals(d.Path, chosenNup, StringComparison.OrdinalIgnoreCase) };
            row.Checked += (_, _) => Choose(d.Path);
            DumpList.Children.Add(row);
        }
    }

    Border Chip(string path)
    {
        checks.TryGetValue(path, out var c);
        var (text, brush) = c == null ? ("Checking…", "InkFaint")
            : c.Value.Ok ? ("✓ Supported version", "Go")
            : c.Value.Text.StartsWith("This is build") ? ("Not the supported version", "Warn")
            : ("Can't be read", "Bad");
        var chip = new Border { Style = (Style)FindResource("Chip"), Child = new TextBlock { Text = text, FontSize = 12, Foreground = (Brush)FindResource(brush), VerticalAlignment = VerticalAlignment.Center } };
        if (c is { Ok: false }) chip.ToolTip = c.Value.Text;
        if (c is { Ok: true }) chip.Background = (Brush)FindResource("GoSoft");
        return chip;
    }

    /// Each found file's version, one at a time (the check unpacks into one probe folder); the first good one is chosen
    async void CheckAll()
    {
        if (checking) return;
        checking = true;
        var tools = Tools.Find();
        while (tools != null && dumps.FirstOrDefault(d => !checks.ContainsKey(d.Path)) is { } d)
        {
            checks[d.Path] = null;
            FillDumps();
            var r = await Runner.Check(tools, new[] { "--check", "--nup", d.Path });
            checks[d.Path] = r;
            if (!r.Ok && r.Text.Contains("antivirus", StringComparison.OrdinalIgnoreCase))
            {
                checking = false;
                ShowProblem(r.Text, tools: true);
                return;
            }
            // (issue 650: not over the romfs and exefs folders the player chose, here or last time: the found .nup showed as chosen)
            if (chosenNup == null && romfs == null && r.Ok) chosenNup = d.Path;
            FillDumps();
            Refresh();
        }
        if (chosenNup == null && romfs == null && dumps.Count > 0 && tools != null) { chosenNup = dumps[0].Path; FillDumps(); }
        checking = false;
        Refresh();
    }

    void Choose(string path)
    {
        sourcePicked = true;
        chosenNup = path;
        romfs = exefs = null;
        if (foldersNoteBlock != null) DumpList.Children.Remove(foldersNoteBlock);   // (not the whole list again: this row's own event)
        foldersNoteBlock = null;
        Refresh();
    }

    void AddDump(string path, string where)
    {
        var info = new FileInfo(path);
        if (!dumps.Any(d => string.Equals(d.Path, info.FullName, StringComparison.OrdinalIgnoreCase)))
            dumps.Insert(0, new Dump(info.FullName, where, info.Length, info.LastWriteTime));
        sourcePicked = true;
        chosenNup = info.FullName;
        romfs = exefs = null;
        FillDumps();
        CheckAll();
        Refresh();
    }

    bool SourceReady => romfs != null && exefs != null
        || chosenNup != null && checks.TryGetValue(chosenNup, out var c) && c is { Ok: true };

    void PathsChanged(object sender, RoutedEventArgs e) => Refresh();

    void Refresh()
    {
        if (BuildButton == null) return;
        BuildButton.IsEnabled = SourceReady && OutPath.Text.Length > 2;
        ShowWork();
        try
        {
            var root = System.IO.Path.GetPathRoot(System.IO.Path.GetFullPath(OutPath.Text))!;
            var free = new DriveInfo(root).AvailableFreeSpace;
            SpaceBar.Value = Math.Min(1, NeededBytes / (double)Math.Max(1, free));
            SpaceBar.Foreground = (Brush)FindResource(free >= NeededBytes ? "Go" : "Bad");
            SpaceText.Text = $"Needs 45 GB · {root.TrimEnd('\\')} has {free >> 30} GB free";
            SpaceText.Foreground = (Brush)FindResource(free >= NeededBytes ? "InkSoft" : "Bad");
        }
        catch (Exception)
        {
            SpaceText.Text = "Needs 45 GB";
        }
        RefreshMode();
    }

    /// The game folder or the build files folder on a hard drive: the build works, but much more slowly than on an SSD. Null
    /// when both are on an SSD (or their drive cannot be told, as on a network share)
    string? HardDriveWarning()
    {
        foreach (var (what, folder) in new[] { ("game folder", OutPath.Text), ("build files folder", Storage.Work) })
        {
            if (Disk.Spinning(folder) != true) continue;
            string root;
            try { root = System.IO.Path.GetPathRoot(System.IO.Path.GetFullPath(folder))!.TrimEnd('\\'); }
            catch (Exception e) when (e is ArgumentException or NotSupportedException or PathTooLongException) { root = folder; }
            return $"The {what} is on a hard drive ({root}). An SSD builds the game much faster: build there if you can.\n\n" +
                   "Choose OK to build here anyway, or Cancel to choose another folder.";
        }
        return null;
    }

    void Dropped(object sender, DragEventArgs e)
    {
        if (runner != null || e.Data.GetData(DataFormats.FileDrop) is not string[] { Length: > 0 } paths) return;
        foreach (var p in paths)
        {
            if (p.EndsWith(".nup", StringComparison.OrdinalIgnoreCase) && File.Exists(p)) AddDump(p, "Dropped");
            else if (Directory.Exists(System.IO.Path.Combine(p, "romfs")) && Directory.Exists(System.IO.Path.Combine(p, "exefs")))
                UseFolders(System.IO.Path.Combine(p, "romfs"), System.IO.Path.Combine(p, "exefs"));
        }
    }

    void BrowseNup(object sender, RoutedEventArgs e)
    {
        var d = new OpenFileDialog { Filter = "Game file (*.nup)|*.nup", Title = "Choose your game file" };
        if (d.ShowDialog(this) == true) AddDump(d.FileName, "Chosen");
    }

    /// The other way in: an unpacked game, its romfs and exefs folders (or one folder holding both)
    void BrowseFolders(object sender, RoutedEventArgs e)
    {
        var both = PickFolder("Choose the folder with romfs and exefs (or the romfs folder)");
        if (both == null) return;
        if (Directory.Exists(System.IO.Path.Combine(both, "romfs")) && Directory.Exists(System.IO.Path.Combine(both, "exefs")))
        {
            UseFolders(System.IO.Path.Combine(both, "romfs"), System.IO.Path.Combine(both, "exefs"));
            return;
        }
        var ex = PickFolder("Now choose the exefs folder");
        if (ex != null) UseFolders(both, ex);
    }

    void UseFolders(string r, string x)
    {
        if (!File.Exists(System.IO.Path.Combine(x, "main")))
        {
            MessageBox.Show(this, "That exefs folder has no file named main.", "Hookshot");
            return;
        }
        sourcePicked = true;
        romfs = r;
        exefs = x;
        chosenNup = null;
        foldersNote = $"Using the folders {r} and {x}";
        FillDumps();
        Refresh();
    }

    static string? PickFolder(string title)
    {
        var d = new OpenFolderDialog { Title = title };
        return d.ShowDialog() == true ? d.FolderName : null;
    }

    void BrowseOut(object sender, RoutedEventArgs e) => OutPath.Text = PickFolder("Choose where the game goes") ?? OutPath.Text;

    void BrowseWork(object sender, RoutedEventArgs e)
    {
        if (PickFolder("Choose where Hookshot keeps its build files") is not { } picked) return;
        ChangeWork(picked);
    }

    void ResetWork(object sender, RoutedEventArgs e) => ChangeWork(null);

    void ChangeWork(string? picked)
    {
        try { Storage.MoveWork(picked); }
        catch (Exception x) when (x is IOException or UnauthorizedAccessException)
        {
            MessageBox.Show(this, $"Hookshot cannot keep its build files there: {x.Message}", "Hookshot");
        }
        Refresh();
    }

    /// How much the build files take and where, said before the build (they stay after it, for quick updates)
    void ShowWork()
    {
        if (WorkNote == null) return;
        var work = Storage.Work;
        WorkReset.Visibility = Storage.WorkChosen ? Visibility.Visible : Visibility.Collapsed;
        WorkNote.Text = Storage.KeepBuildFiles
            ? $"Build files: about {Storage.Gb(Storage.BuildFilesBytes)}, kept in {work} so that updates are quick (HOOKSHOT_KEEP_BUILD_FILES)."
            : $"Build files: about {Storage.Gb(Storage.BuildFilesBytes)} in {work} while it builds; when the game is ready about 1 GB stays (the compiled code) so that updates compile only what changed.";
        WorkNote.Foreground = (Brush)FindResource("InkSoft");
        try
        {
            var root = System.IO.Path.GetPathRoot(System.IO.Path.GetFullPath(work))!;
            var free = new DriveInfo(root).AvailableFreeSpace;
            if (free < Storage.BuildFilesBytes)
            {
                WorkNote.Text += $" {root.TrimEnd('\\')} has only {Storage.Gb(free)} free.";
                WorkNote.Foreground = (Brush)FindResource("Bad");
            }
        }
        catch (Exception) { }
    }

    void LanguageChanged(object sender, SelectionChangedEventArgs e)
    {
        if (LanguageBox.SelectedItem is not Language l) return;
        lang = l;
        CancelButton.Content = T(S.Cancel);
        BackButton.Content = T(S.Back);
        OpenButton.Content = T(S.OpenFolder);
        PlayText.Text = T(S.Play);
        ShowLog.Content = T(ShowLog.IsChecked == true ? S.HideDetails : S.ShowDetails);
    }

    // ================= 2: the build =================

    async void StartBuild(object sender, RoutedEventArgs e)
    {
        switch (mode)
        {
            case Mode.UpdateGame:   // (the kept game files; the newer builder first when this one is older)
                if (!OlderOk(GameVersion(OutPath.Text))) return;
                SaveSettings();
                Updater.UpdateFolder(this, OutPath.Text, updateTarget, fetched: false, named: Switching);
                return;
            case Mode.UpToDate:
                finished = OutPath.Text;
                Play(sender, e);
                return;
            case Mode.UpdateBuilder:
                if (!OlderOk(Tools.Find()?.Version)) return;
                SaveSettings();
                BuildButton.IsEnabled = false;
                BuildButton.Content = "Getting the new builder…";
                try { await Updater.RunLatestSetup(this, Array.Empty<string>(), updateTarget, Switching); }
                catch (Exception ex)
                {
                    MessageBox.Show(this, ex.Message + " You can also download HookshotBuilder.exe from the releases page.", "Hookshot");
                    RefreshMode();
                }
                return;
        }
        SaveSettings();
        var tools = Tools.Find();
        if (tools == null)
        {
            ShowProblem("The build tools are not in Hookshot's folder.", tools: true);
            return;
        }
        var args = new List<string>();
        if (romfs != null && exefs != null) args.AddRange(new[] { "--romfs", romfs, "--exefs", exefs });
        else if (chosenNup != null) args.AddRange(new[] { "--nup", chosenNup });
        else return;
        args.AddRange(new[] { "--out", OutPath.Text, "--language", lang.Code });
        if (FsrTick.IsChecked == true) args.Add("--fsr");   // (an --update of the folder keeps it: make_game.py's mark)
        prepareNow = PrepareTick.IsChecked == true;
        Run(tools, ShaderStep.Args(args, prepareNow), dlss: DlssTick.IsChecked == true,
            fsr: FsrTick.IsChecked == true && fsrLicense);
    }

    const string DlssLicense =
        "NVIDIA DLSS is NVIDIA's, not Hookshot's. With it ticked, the build downloads NVIDIA's DLSS SDK (release v310.9.16) from " +
        "github.com/NVIDIA/DLSS, under NVIDIA's own license, the NVIDIA RTX SDKs license:\n\n" +
        "https://github.com/NVIDIA/DLSS/blob/9981c6350dc58c0ce21f024fd65e31e851c31689/LICENSE.txt\n\n" +
        "Its files stay on this PC (in the builder's ngx-sdk folder, and nvngx_dlss.dll and nvngx_dlssg.dll in the game " +
        "folder's ngx folder). " +
        "Hookshot never includes or shares them: never share a game folder built with them.\n\n" +
        "Do you accept NVIDIA's license?";

    /// NVIDIA's license accepted on the DLSS tick, kept in the settings (DlssLicense): an update keeps a folder's DLSS by it.
    /// It was the SDK's LICENSE.txt, a download: with the SDK gone, the update built without DLSS and its runtime went
    bool dlssLicense;

    /// The NVIDIA DLSS tick: NVIDIA's license first, and the tick stays only when it is accepted
    void DlssChanged(object sender, RoutedEventArgs e)
    {
        if (DlssTick.IsChecked == true &&
            MessageBox.Show(this, DlssLicense, "NVIDIA DLSS", MessageBoxButton.YesNo, MessageBoxImage.Information) != MessageBoxResult.Yes)
            DlssTick.IsChecked = false;
        else if (DlssTick.IsChecked == true)
            dlssLicense = true;
        SaveSettings();   // (kept at once, not only when Build is pressed: ticked and closed, it is still ticked next time)
    }

    const string FsrLicense =
        "AMD FSR 4 is AMD's, not Hookshot's. With it ticked, the build downloads AMD's FSR runtime for Vulkan (FidelityFX SDK " +
        "v2.4.0: amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll, signed by AMD) from " +
        "github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, each file checked against its SHA-256, under AMD's FSR SDK license:\n\n" +
        "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/7539ae8b6568ad29ed78a753404351daf3b5cca7/Kits/FidelityFX/docs/license.md\n\n" +
        "Its files stay on this PC (in the builder's ffx-sdk folder, and the two DLLs with LICENSE-AMD-FSR-SDK.md in the game " +
        "folder).\n\n" +
        "Do you accept AMD's license?";

    /// AMD's license accepted on the FSR 4 tick. A tick saved before the builder asked builds with FSR 4 as it did, its DLLs
    /// the player's own, and fetches nothing until it is ticked again
    bool fsrLicense;

    /// The AMD FSR 4 tick: AMD's license first, and the tick stays only when it is accepted (as DLSS's; this builder is
    /// Windows', where AMD publishes the runtime)
    void FsrChanged(object sender, RoutedEventArgs e)
    {
        if (FsrTick.IsChecked == true)
        {
            fsrLicense = MessageBox.Show(this, FsrLicense, "AMD FSR 4", MessageBoxButton.YesNo, MessageBoxImage.Information) ==
                         MessageBoxResult.Yes;
            if (!fsrLicense) FsrTick.IsChecked = false;
        }
        SaveSettings();
    }

    static readonly (string Id, string Build, string Update)[] Stages =
    {
        ("download", "", "Download the update"),
        ("read", "Read your game files", "Read the game files kept in the folder"),
        ("lift", "Translate the game to PC code", "Translate the game to PC code"),
        ("compile", "Compile", "Compile changed files"),
        ("finish", "Put the game together", "Swap in the new game"),
        (ShaderStep.Id, "Prepare the shaders", "Prepare the shaders"),
    };
    static readonly Dictionary<string, (double From, double Size)> WithShaders = new()
    {
        ["download"] = (0, 0.04), ["read"] = (0.04, 0.04), ["lift"] = (0.08, 0.14), ["compile"] = (0.22, 0.68), ["finish"] = (0.90, 0.02),
        [ShaderStep.Id] = (0.92, 0.08),   // (hookshot --prepare-shaders: minutes on some GPUs, the first start's wait moved here)
    };
    static readonly Dictionary<string, (double From, double Size)> WithoutShaders = new()
    {
        ["download"] = (0, 0.04), ["read"] = (0.04, 0.04), ["lift"] = (0.08, 0.14), ["compile"] = (0.22, 0.74), ["finish"] = (0.96, 0.04),
    };
    Dictionary<string, (double From, double Size)> Weight = WithoutShaders;
    // (this build prepares the shaders: the tick for a build, the saved choice for an update. The game's @@stage shaders came, and
    // make_game's line when it left them to the first start: the row's end says which)
    bool prepareNow, shadersBegan;
    string? shadersLater;
    TimeSpan? builtIn;   // (at @@times: the build without the shaders' minutes, as build_times.json keeps it)

    void MakeRows(bool update, bool shaders)
    {
        StageList.Children.Clear();
        rows.Clear();
        Weight = shaders ? WithShaders : WithoutShaders;
        var list = Stages.Where(s => (update || s.Id != "download") && (shaders || s.Id != ShaderStep.Id)).ToList();
        for (var i = 0; i < list.Count; i++)
        {
            var row = new StepRow(i + 1, update ? list[i].Update : list[i].Build, last: i == list.Count - 1);
            rows[list[i].Id] = row;
            StageList.Children.Add(row.Root);
        }
        if (update) rows["read"].Small("Your game files are kept: the .nup isn't needed");
    }

    /// dlss: NVIDIA DLSS, its license accepted: NVIDIA's SDK fetched into Storage.NgxSdk first (only what is missing), and built in.
    /// fsr: AMD FSR 4, its license accepted: built with it, AMD's runtime fetched into Storage.FfxSdk and put in the game folder
    void Run(Tools tools, List<string> args, bool keepRows = false, bool dlss = false, bool fsr = false)
    {
        // (a warning, not a stop: the player may have no other folder with the room. An update keeps its rows and its own flow)
        if (!keepRows && HardDriveWarning() is { } slow &&
            MessageBox.Show(this, slow, "Hookshot", MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK)
            return;
        int? workers, memoryGb;
        try
        {
            workers = BuildWorkers.Selected(WorkerBox.SelectedItem);
            memoryGb = BuildMemory.Selected(MemoryBox.SelectedItem);
        }
        catch (ArgumentException e)
        {
            MessageBox.Show(this, e.Message, "Hookshot");
            return;
        }
        if (!keepRows)
        {
            updating = false;
            MakeRows(update: false, shaders: prepareNow);
            SetStep(1);
            BuildTitle.Text = "Building your game";
            UpdateBar.Visibility = Visibility.Collapsed;
            RingBox.Visibility = Visibility.Visible;
        }
        log.Clear();
        Log.Clear();
        current = null;
        finished = "";
        compiled = 0;
        filesDone = filesOf = 0;
        filesChanged = -1;
        filesCompiled = 0;
        compileEta = -1;
        thisCompile = 0;
        previousTotal = -1;
        shadersBegan = false;
        shadersLater = null;
        builtIn = null;
        ShowPage(BuildPage);
        CancelButton.Visibility = Visibility.Visible;
        SetProgress(updating ? Weight["read"].From : 0);
        elapsed.Restart();
        compileClock.Reset();
        clock.Start();
        Tick();
        if (Storage.WorkChosen && !args.Contains("--work")) args.AddRange(new[] { "--work", Storage.Work });   // (the build files where the player chose)
        if (dlss && !args.Contains("--fetch-ngx-sdk")) args.AddRange(new[] { "--fetch-ngx-sdk", "--accept-nvidia-license" });
        if (fsr && !args.Contains("--fsr")) args.Add("--fsr");
        if (fsr && !args.Contains("--fetch-fsr-runtime")) args.AddRange(new[] { "--fetch-fsr-runtime", "--accept-amd-license" });
        runner = new Runner(tools, args, workers, dlss ? Storage.NgxSdk : null, memoryGb, fsr ? Storage.FfxSdk : null);
        runner.Line += l => Dispatcher.BeginInvoke(() => Line(l));
        runner.Finished += failure => Dispatcher.BeginInvoke(() => Ended(failure));
        runner.Start();
    }

    static readonly Regex FilesLine = new(@"^(\d+) files, "), FileLine = new(@"^\s+\S+\s+\d+ MB\s+\d+s");

    void Line(string line)
    {
        if (line.StartsWith("@@stage "))
        {
            var id = line.Split(' ')[1];
            if (current != null && rows.TryGetValue(current, out var was)) was.Done();
            current = id;
            if (rows.TryGetValue(id, out var row)) row.Begin();
            if (id == "compile") compileClock.Restart();
            if (id == ShaderStep.Id) shadersBegan = true;
            if (Weight.TryGetValue(id, out var w)) SetProgress(w.From);
        }
        else if (line.StartsWith("@@progress "))
        {
            if (double.TryParse(line[11..], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var f) && current != null
                && Weight.TryGetValue(current, out var w))
            {
                if (current == "compile") compiled = f;
                SetProgress(w.From + w.Size * Math.Clamp(f, 0, 1));
            }
            return;
        }
        else if (line.StartsWith("@@plan ") || line.StartsWith("@@file ") || line.StartsWith("@@eta ") || line.StartsWith("@@times "))
        {
            Number(line);
            return;
        }
        else if (line.StartsWith("@@done ")) finished = line[7..];
        else if (ShaderStep.Reason(line) is { } later) shadersLater = later;
        else if (FilesLine.Match(line) is { Success: true } m) filesOf = int.Parse(m.Groups[1].Value);
        else if (FileLine.IsMatch(line)) filesDone++;
        if (current == "compile" && filesChanged < 0 && filesOf > 0 && rows.TryGetValue("compile", out var c))
            c.Small($"{filesDone} of {filesOf} files");
        AddLog(line);
    }

    /// The compile timer's lines: build.py says which files this run really rebuilds and how long each took before (the
    /// countdown), and make_game.py gives this build's times beside the last one's. Not shown in details: the compiler's
    /// own lines there already carry each file's seconds.
    void Number(string line)
    {
        var p = line.Split(' ');
        switch (p[0])
        {
            case "@@plan" when p.Length == 3 && int.TryParse(p[1], out var changed) && int.TryParse(p[2], out var total):
                filesChanged = changed;
                filesOf = total;
                filesCompiled = 0;
                ChangedFiles();
                break;
            case "@@file" when p.Length == 4 && (p[3] is "new" or "again"):
                filesCompiled++;
                ChangedFiles();
                break;
            case "@@eta" when p.Length == 2 && double.TryParse(p[1], System.Globalization.NumberStyles.Float,
                                                              System.Globalization.CultureInfo.InvariantCulture, out var eta):
                compileEta = eta;
                break;
            case "@@times" when p.Length == 4:
                builtIn = elapsed.Elapsed;
                long.TryParse(p[2], out thisCompile);
                long.TryParse(p[3], out previousTotal);
                break;
        }
    }

    void ChangedFiles()
    {
        if (filesChanged < 0 || !rows.TryGetValue("compile", out var row)) return;
        row.Small(filesChanged == 0 ? "Everything is already built"
                : filesChanged >= filesOf ? $"{filesCompiled} of {filesChanged} files"
                : $"{filesCompiled} of {filesChanged} changed files");
    }

    void AddLog(string line)
    {
        log.AppendLine(line);
        if (log.Length > 600_000) log.Remove(0, log.Length - 400_000);
        if (Log.Visibility == Visibility.Visible)
        {
            Log.AppendText(line + "\n");
            Log.ScrollToEnd();
        }
    }

    void SetProgress(double f)
    {
        f = Math.Clamp(f, 0, 1);
        RingText.Text = $"{(int)(f * 100)}%";
        UpdateBar.Value = f;
        const double r = 55, c = 60;
        if (f >= 0.999) { RingArc.Data = new EllipseGeometry(new Point(c, c), r, r); return; }
        var a = f * 2 * Math.PI;
        var end = new Point(c + r * Math.Sin(a), c - r * Math.Cos(a));
        var fig = new PathFigure { StartPoint = new Point(c, c - r), IsClosed = false };
        fig.Segments.Add(new ArcSegment(end, new Size(r, r), 0, f > 0.5, SweepDirection.Clockwise, true));
        RingArc.Data = f < 0.002 ? null : new PathGeometry(new[] { fig });
    }

    void Tick()
    {
        foreach (var r in rows.Values) r.Tick();
        if (runner == null) return;
        var tail = updating ? ". Your current game stays as it is until the new one is complete." : " · your PC stays usable";
        if (current == "compile")
        {
            // (the timer of the compile itself, and the countdown build.py works out from what this PC measured before)
            var left = compileEta >= 0 ? TimeLeft(compileEta)
                : compiled > 0.03 ? TimeLeft(compileClock.Elapsed.TotalSeconds / compiled * (1 - compiled) + 60)
                : "Estimating…";
            BuildNote.Text = $"Compiling {Clock(compileClock.Elapsed)} · {left}{tail}";
            return;
        }
        BuildNote.Text = (updating ? "Working…" : "Usually about 45 minutes") + tail;
    }

    static string Clock(TimeSpan t) => $"{(int)t.TotalMinutes}:{t.Seconds:00}";

    static string TimeLeft(double seconds)
    {
        if (seconds < 45) return "Less than a minute left";
        if (seconds < 90) return "About a minute left";
        var minutes = (int)Math.Round(seconds / 60);
        return minutes >= 60 ? $"About {minutes / 60} h {minutes % 60} min left" : $"About {minutes} minutes left";
    }

    void Ended(string? failure)
    {
        clock.Stop();
        elapsed.Stop();
        runner = null;
        if (finished.Length > 0)
        {
            foreach (var (id, r) in rows)
            {
                if (id != ShaderStep.Id) r.Done();
                else if (ShaderStep.Skipped(shadersBegan, shadersLater) is { } why) { r.Skipped(); r.Small(why); }
                else r.Done();
            }
            SetProgress(1);
            ShowReady();
            return;
        }
        if (failure == "cancelled")
        {
            ToFiles();
            return;
        }
        if (current != null && rows.TryGetValue(current, out var row)) row.Stopped();
        ShowProblem(failure ?? "The build tools stopped without saying why.", tools: failure?.Contains("antivirus", StringComparison.OrdinalIgnoreCase) == true);
    }

    void LogToggled(object sender, RoutedEventArgs e)
    {
        var show = ShowLog.IsChecked == true;
        Log.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        StepCard.Visibility = show ? Visibility.Collapsed : Visibility.Visible;
        ShowLog.Content = T(show ? S.HideDetails : S.ShowDetails);
        if (show)
        {
            Log.Text = log.ToString();
            Log.ScrollToEnd();
        }
    }

    void CancelBuild(object sender, RoutedEventArgs e)
    {
        download?.Cancel();
        runner?.Cancel();
    }

    // ================= 3: play =================

    void ShowReady()
    {
        SetStep(2);
        ReadyTitle.Text = updating ? "Your game is updated" : "Your game is ready";
        var compiledNote = thisCompile > 0 ? $" (compiled in {Span(TimeSpan.FromSeconds(thisCompile))})" : "";
        ReadyNote.Text = $"{(updating ? "Updated" : "Built")} in {Span(elapsed.Elapsed)}{compiledNote}{Against(builtIn ?? elapsed.Elapsed)} · {finished}";
        if (!updating && Environment.GetEnvironmentVariable("HOOKSHOT_UI_PREVIEW") == null)
            Finder.Shortcuts(finished, DesktopTick.IsChecked == true, StartTick.IsChecked == true);
        else if (updating && Environment.GetEnvironmentVariable("HOOKSHOT_UI_PREVIEW") == null)
        {   // (the ones the player has, onto the folder's launcher; the boxes say which there are, and ticking one makes it)
            DesktopTick.IsChecked = File.Exists(Finder.DesktopLink);
            StartTick.IsChecked = File.Exists(Finder.StartLink);
            Finder.RefreshShortcuts(finished);
        }
        if (Finder.ShortcutError != null) AddLog("shortcut not made: " + Finder.ShortcutError);
        ShowPage(ReadyPage);
        _ = FreeAfterBuild();
    }

    // (issue: 5 GB and more left in %LOCALAPPDATA% and never said. Said here, with the way to let it go)
    async Task ShowStorageUse()
    {
        StorageRow.Visibility = Visibility.Collapsed;
        var (total, freeable) = await Task.Run(Storage.Measure);
        if (total < 100L << 20) return;
        // (both places when the build files are in a folder the player chose: the total counts both)
        var where = Storage.WorkChosen ? $"{Storage.Base} and {Storage.Work}" : Storage.Base;
        StorageNote.Text = freeable >= 100L << 20
            ? $"Hookshot Builder keeps {Storage.Gb(total)} on this PC ({where}). {Storage.Gb(freeable)} of it only makes the next " +
              "update quicker and can be freed: your game keeps playing, and the next update then takes about as long as this build."
            : $"Hookshot Builder keeps {Storage.Gb(total)} on this PC ({where}): the builder itself.";
        FreeButton.Visibility = freeable >= 100L << 20 ? Visibility.Visible : Visibility.Collapsed;
        FreeButton.IsEnabled = true;
        FreeButton.Content = "Free up space";
        StorageRow.Visibility = Visibility.Visible;
    }

    // (players asked for the space back: the translated code, the setup's and updater's downloads and older builder versions go as soon
    // as the game is ready. The compiled objects stay (~1 GB): without them every update compiled all ~600 files again, an hour and
    // more (issue 1287). Free up space lets them go too. HOOKSHOT_KEEP_BUILD_FILES=1 keeps everything)
    async Task FreeAfterBuild()
    {
        if (!Storage.KeepBuildFiles)
        {
            var left = await Task.Run(() => Storage.Free(afterBuild: true));
            if (left >= 100L << 20) AddLog($"build files: {Storage.Gb(left)} could not be deleted (a file in use); they go after the next build");
        }
        await ShowStorageUse();
    }

    async void FreeSpace(object sender, RoutedEventArgs e)
    {
        FreeButton.IsEnabled = false;
        FreeButton.Content = "Freeing…";
        var left = await Task.Run(() => Storage.Free());
        await ShowStorageUse();
        if (left >= 100L << 20)
            StorageNote.Text += $" ({Storage.Gb(left)} could not be deleted: a file is in use. Close the game and the builder, and delete the folder's contents by hand.)";
    }

    void ShowStorage(object sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo("explorer.exe", $"\"{(Storage.WorkChosen ? Storage.Work : Storage.Base)}\"") { UseShellExecute = true });   // (the bigger one)

    void ShortcutsChanged(object sender, RoutedEventArgs e)
    {
        if (finished.Length > 0) Finder.Shortcuts(finished, DesktopTick.IsChecked == true, StartTick.IsChecked == true);
    }

    static string Span(TimeSpan t) => t.TotalHours >= 1 ? $"{(int)t.TotalHours} h {t.Minutes} min" : t.TotalMinutes >= 1 ? $"{(int)t.TotalMinutes} minutes" : $"{t.Seconds} seconds";

    /// This build beside the last one on this PC (build_times.json, through make_game.py's @@times): "last time 45 minutes
    /// (4 minutes faster)"
    string Against(TimeSpan now)
    {
        if (previousTotal <= 0) return "";
        var last = TimeSpan.FromSeconds(previousTotal);
        var diff = last.TotalSeconds - now.TotalSeconds;
        var how = Math.Abs(diff) < 30 ? "about the same"
                : diff > 0 ? $"{Span(TimeSpan.FromSeconds(diff))} faster" : $"{Span(TimeSpan.FromSeconds(-diff))} slower";
        return $" · last time {Span(last)} ({how})";
    }

    void OpenFolder(object sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo("explorer.exe", $"\"{finished}\"") { UseShellExecute = true });

    void Play(object sender, RoutedEventArgs e)
    {
        StartGame(finished);
        WindowState = WindowState.Minimized;
    }

    // ================= when something goes wrong =================

    bool problemIsTools;

    void ShowProblem(string why, bool tools)
    {
        lastFailure = why;
        problemIsTools = tools;
        ProblemSteps.Children.Clear();
        ProblemSub.Text = updating ? "Your current game is unchanged and still plays." : "Nothing was changed in your game folder.";
        if (tools)
        {
            ProblemTitle.Text = "The build tools need repairing";
            ProblemHead.Text = "Your antivirus removed part of the build tools";
            ProblemText.Text = why + "\n\nAntivirus programs sometimes flag unsigned developer tools by mistake.";
            var base_ = Storage.Base;
            foreach (var (n, s) in new[]
                     {
                         (1, "Open Windows Security → Protection history, and restore the removed files."),
                         (2, $"Or add {base_} to its exclusions."),
                         (3, "Then press Repair: Hookshot downloads the tools again from their official releases and checks each one."),
                     })
                ProblemSteps.Children.Add(new TextBlock { Text = $"{n}.  {s}", Style = (Style)FindResource("Note"), FontSize = 14, Margin = new Thickness(0, 6, 0, 0) });
        }
        else
        {
            ProblemTitle.Text = updating ? "The update stopped" : "The build stopped";
            ProblemHead.Text = "What went wrong";
            ProblemText.Text = why;
        }
        RepairButton.Visibility = tools ? Visibility.Visible : Visibility.Collapsed;
        ProblemDetails.Content = "Show details";
        if (!updating) SetStep(1);   // (the update keeps its versions in the head)
        ShowPage(ProblemPage);
    }

    void ProblemShowDetails(object sender, RoutedEventArgs e)
    {
        var lines = log.ToString().Split('\n');
        ProblemSteps.Children.Clear();
        ProblemSteps.Children.Add(new TextBox
        {
            Text = string.Join("\n", lines.Skip(Math.Max(0, lines.Length - 80))), IsReadOnly = true, FontFamily = (FontFamily)FindResource("Mono"), FontSize = 11,
            Foreground = (Brush)FindResource("InkSoft"), Background = new SolidColorBrush(Color.FromArgb(0x33, 0, 0, 0)), BorderThickness = new Thickness(0),
            Padding = new Thickness(8), TextWrapping = TextWrapping.NoWrap, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
        });
    }

    void CopyForReport(object sender, RoutedEventArgs e)
    {
        var lines = log.ToString().Split('\n');
        var text = $"Hookshot Builder {Tools.Find()?.Version ?? "(source)"} on Windows {Environment.OSVersion.Version}\n" +
                   $"{ProblemTitle.Text}: {lastFailure}\n\n" + string.Join("\n", lines.Skip(Math.Max(0, lines.Length - 200)));
        try { Clipboard.SetText(text); CopyReport.Content = "Copied"; } catch (Exception) { }
    }

    void Back(object sender, RoutedEventArgs e) => ToFiles();

    /// The setup (HookshotBuilder-Setup.exe beside this app) checks every part against its checksum and downloads the missing ones
    void Repair(object sender, RoutedEventArgs e)
    {
        var setup = System.IO.Path.Combine(AppContext.BaseDirectory, "HookshotBuilder-Setup.exe");
        if (File.Exists(setup))
        {
            Process.Start(new ProcessStartInfo(setup, "--repair") { UseShellExecute = false, WorkingDirectory = AppContext.BaseDirectory });
            Application.Current.Shutdown();
            return;
        }
        Process.Start(new ProcessStartInfo("https://github.com/NotRexed/Hookshot/releases/latest") { UseShellExecute = true });
    }

    // ================= the update (Updater.cs): the game started this app with --update <folder> =================

    CancellationTokenSource? download;

    public void BeginUpdate(string folder, string? tag)
    {
        updating = true;
        OutPath.Text = folder;
        finished = "";
        var to = tag ?? "the latest version";
        var from = GameVersion(folder) ?? Tools.Find()?.Version;
        StepPill.Visibility = Visibility.Collapsed;
        VersionChip.Visibility = Visibility.Visible;
        VersionChipText.Text = from != null && from != to ? $"{from} → {to}" : to;
        prepareNow = BuilderSettings.PrepareNow(SettingsPath);
        // (the updates show no menu: the saved choice, as ApplySettings may not have run)
        MemoryBox.SelectedItem = BuildMemory.Saved(MemoryBox.ItemsSource.Cast<object>(), BuilderSettings.Load(SettingsPath)?.MemoryGb);
        MakeRows(update: true, shaders: prepareNow);
        BuildTitle.Text = "Updating your game";
        BuildNote.Text = "Getting the update…";
        RingBox.Visibility = Visibility.Collapsed;
        UpdateBar.Visibility = Visibility.Visible;
        SetProgress(0);
        rows["download"].Begin();
        CancelButton.Visibility = Visibility.Visible;
        ShowPage(BuildPage);
    }

    public CancellationToken DownloadToken => (download = new CancellationTokenSource()).Token;

    public void DownloadProgress(double f)
    {
        rows.GetValueOrDefault("download")?.Small($"{(int)(f * 100)}%");
        SetProgress(Weight["download"].Size * f);
    }

    public void Note(string line)
    {
        AddLog(line);
        rows.GetValueOrDefault("download")?.Small(line);
    }

    public void DownloadFailed(string why)
    {
        download = null;
        rows.GetValueOrDefault("download")?.Stopped();
        ShowProblem(why, tools: false);
        ProblemTitle.Text = "The update could not be downloaded";
    }

    /// The new tools are here: the folder built again with them (only what changed is compiled)
    internal void UpdateBuild(Tools tools, string folder)
    {
        download = null;
        if (rows.TryGetValue("download", out var d)) { d.Done(); d.Small("Build tools checked"); }
        // (the saved tick also opts an existing game into DLSS. After a self-update ApplySettings has not run yet;
        // a folder already built with DLSS keeps it, its license accepted then: the settings' record, or for a license accepted
        // before the record, the SDK's LICENSE.txt)
        var saved = BuilderSettings.Load(SettingsPath);
        var dlss = saved?.Dlss == true ||
                   (File.Exists(System.IO.Path.Combine(folder, "ngx", "nvngx_dlss.dll")) &&
                    (saved?.DlssLicense == true || File.Exists(System.IO.Path.Combine(Storage.NgxSdk, "LICENSE.txt"))));
        // (AMD FSR 4, the same: the saved tick with AMD's license accepted, or a folder built with FSR 4 whose AMD runtime
        // was fetched, AMD's license saved beside it then, keeps it and its runtime)
        var fsr = (saved?.Fsr == true && saved.FsrLicense) ||
                  (File.Exists(System.IO.Path.Combine(folder, ".hookshot_fsr")) &&
                   File.Exists(System.IO.Path.Combine(folder, "amd_fidelityfx_upscaler_vk.dll")) &&
                   File.Exists(System.IO.Path.Combine(folder, "LICENSE-AMD-FSR-SDK.md")));
        // (prepareNow: the saved choice, read in BeginUpdate. The launcher's Update and the --update start after a self-update never
        // ran ApplySettings, so the tick there is only its default)
        Run(tools, ShaderStep.Args(new List<string> { "--update", folder }, prepareNow), keepRows: true, dlss: dlss, fsr: fsr);
    }

    // ================= the window =================

    void Minimize(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    void CloseWindow(object sender, RoutedEventArgs e)
    {
        if (runner != null && MessageBox.Show(this, T(S.CloseQuestion), "Hookshot", MessageBoxButton.YesNo) != MessageBoxResult.Yes) return;
        runner?.Cancel();
        Close();
    }
}

/// One step of the build: its mark (done, now, waiting), its name, a line under it and its time.
sealed class StepRow
{
    public readonly Grid Root = new() { Margin = new Thickness(0, 10, 0, 10) };
    readonly Grid mark = new() { Width = 22, Height = 22, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 1, 0, 0) };
    readonly Ellipse disc = new(), halo = new() { Width = 32, Height = 32, Margin = new Thickness(-5) };
    readonly TextBlock number = new() { FontSize = 12, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
    readonly Path tick = new() { Data = Geometry.Parse("M0,4 L3.5,7.5 L10,0"), StrokeThickness = 1.8, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
    readonly TextBlock name = new() { FontWeight = FontWeights.SemiBold };
    readonly TextBlock small = new() { FontSize = 12, Visibility = Visibility.Collapsed, TextTrimming = TextTrimming.CharacterEllipsis };
    readonly TextBlock time = new() { FontSize = 12, Text = "—", VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 2, 0, 0) };
    readonly Stopwatch watch = new();
    readonly int n;
    bool running;

    static Brush B(string key) => (Brush)Application.Current.FindResource(key);

    public StepRow(int n, string text, bool last)
    {
        this.n = n;
        Root.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(34) });
        Root.ColumnDefinitions.Add(new ColumnDefinition());
        Root.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        mark.Children.Add(halo);
        mark.Children.Add(disc);
        mark.Children.Add(number);
        mark.Children.Add(tick);
        tick.Stroke = B("Go");
        time.FontFamily = (FontFamily)Application.Current.FindResource("Mono");
        var texts = new StackPanel { Children = { name, small } };
        Grid.SetColumn(texts, 1);
        Grid.SetColumn(time, 2);
        if (!last)
        {
            var link = new Rectangle { Width = 1, Fill = B("GlassLine"), HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(10.5, 28, 0, -19) };
            Root.Children.Add(link);
        }
        Root.Children.Add(mark);
        Root.Children.Add(texts);
        Root.Children.Add(time);
        name.Text = text;
        Wait();
    }

    public void Small(string text)
    {
        small.Text = text;
        small.Visibility = Visibility.Visible;
    }

    void Wait()
    {
        disc.Fill = Brushes.Transparent;
        disc.Stroke = B("GlassLineStrong");
        disc.StrokeThickness = 1.5;
        halo.Fill = Brushes.Transparent;
        number.Text = "";
        tick.Visibility = Visibility.Collapsed;
        name.Foreground = B("InkFaint");
        name.FontWeight = FontWeights.Medium;
        small.Foreground = B("InkFaint");
        time.Foreground = B("InkFaint");
    }

    public void Begin()
    {
        running = true;
        watch.Restart();
        disc.Fill = B("Brand");
        disc.Stroke = null;
        halo.Fill = B("BrandSoft");
        number.Text = n.ToString();
        number.Foreground = Brushes.White;
        tick.Visibility = Visibility.Collapsed;
        name.Foreground = B("Ink");
        name.FontWeight = FontWeights.SemiBold;
        time.Foreground = B("Ink");
        Tick();
    }

    public void Tick()
    {
        if (running) time.Text = $"{(int)watch.Elapsed.TotalMinutes}:{watch.Elapsed.Seconds:00}";
    }

    public void Done()
    {
        if (running) Tick();
        running = false;
        watch.Stop();
        disc.Fill = B("GoSoft");
        disc.Stroke = null;
        halo.Fill = Brushes.Transparent;
        number.Text = "";
        tick.Visibility = Visibility.Visible;
        name.Foreground = B("Ink");
        name.FontWeight = FontWeights.SemiBold;
        time.Foreground = B("InkFaint");
    }

    /// (not done: left to later, the shaders' step when they were not prepared now)
    public void Skipped()
    {
        if (running) Tick();
        running = false;
        watch.Stop();
        disc.Fill = Brushes.Transparent;
        disc.Stroke = B("GlassLineStrong");
        disc.StrokeThickness = 1.5;
        halo.Fill = Brushes.Transparent;
        number.Text = "–";
        number.Foreground = B("InkFaint");
        tick.Visibility = Visibility.Collapsed;
        name.Foreground = B("InkFaint");
        name.FontWeight = FontWeights.Medium;
        time.Foreground = B("InkFaint");
    }

    public void Stopped()
    {
        running = false;
        watch.Stop();
        disc.Fill = B("BadSoft");
        halo.Fill = Brushes.Transparent;
        number.Text = "!";
        number.Foreground = B("Bad");
    }
}
