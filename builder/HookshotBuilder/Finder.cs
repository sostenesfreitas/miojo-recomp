using System.IO;

namespace HookshotBuilder;

/// The player's game files where people keep them, and the game's shortcuts.
static class Finder
{
    /// .nup files in Downloads, Desktop, Documents, the home folder and the top of each drive, one folder level down too,
    /// newest first. (Only names and sizes are read here: the version is checked one file at a time afterwards.) Each place is
    /// searched on its own, and the search gives what it has after DumpSearch.Budget: one drive that does not answer (a USB drive
    /// asleep, a cloud or network drive) held the start-up page on "Looking for your game files…" for good
    public static List<Dump> FindDumps()
    {
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var places = new List<(string Dir, string Name)>
        {
            (Path.Combine(home, "Downloads"), "Downloads"),
            (Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Desktop"),
            (Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Documents"),
            (home, "Home"),
        };
        try
        {
            // (only the drive's type here: IsReady asks the drive itself, and is asked in the drive's own search)
            foreach (var d in DriveInfo.GetDrives())
            {
                try
                {
                    if (d.DriveType is DriveType.Fixed or DriveType.Removable) places.Add((d.RootDirectory.FullName, d.Name.TrimEnd('\\')));
                }
                catch (Exception) { }
            }
        }
        catch (Exception) { }
        return DumpSearch.Search(places, DumpSearch.Budget, DumpSearch.Look);
    }

    public static string Ago(DateTime t)
    {
        var d = DateTime.Now - t;
        if (d.TotalDays < 1 && t.Date == DateTime.Today) return "modified today";
        if (d.TotalDays < 2) return "modified yesterday";
        if (d.TotalDays < 14) return $"{(int)d.TotalDays} days ago";
        if (d.TotalDays < 60) return $"{(int)(d.TotalDays / 7)} weeks ago";
        return t.ToString("d MMM yyyy");
    }

    public static string Size(long bytes) => bytes >= 1L << 30 ? $"{bytes / (double)(1L << 30):0.0} GB" : $"{bytes >> 20} MB";

    // ---- shortcuts to the built game ----

    public static string DesktopLink => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Hookshot.lnk");
    public static string StartLink => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), "Hookshot.lnk");

    /// Made or removed as the two ticks say (Windows' own shell object writes the .lnk)
    /// The shortcuts the player has, made again (onto the folder's launcher now that it has one); none made where there was none
    public static void RefreshShortcuts(string gameFolder) => Shortcuts(gameFolder, File.Exists(DesktopLink), File.Exists(StartLink));

    public static void Shortcuts(string gameFolder, bool desktop, bool start)
    {
        // (hookshot.exe: the game's launcher in a folder built since it came (Play, and Update when a newer release is out), else the
        // game. The icon is the game's own: bin\hookshot.exe when it is there)
        var exe = Path.Combine(gameFolder, "hookshot.exe");
        var icon = File.Exists(Path.Combine(gameFolder, "bin", "hookshot.exe")) ? Path.Combine(gameFolder, "bin", "hookshot.exe") : exe;
        foreach (var (link, want) in new[] { (DesktopLink, desktop), (StartLink, start) })
        {
            try
            {
                if (!want)
                {
                    if (File.Exists(link)) File.Delete(link);
                    continue;
                }
                if (!File.Exists(exe)) continue;
                // (the shell's own shortcut object (IShellLinkW + IPersistFile): the scripting host's, late-bound through dynamic,
                // made none for players and said nothing)
                var sl = (IShellLinkW)new ShellLink();
                sl.SetPath(exe);
                sl.SetWorkingDirectory(gameFolder);
                sl.SetIconLocation(icon, 0);
                sl.SetDescription("The Legend of Zelda: Ocarina of Time, built by Hookshot");
                Directory.CreateDirectory(Path.GetDirectoryName(link)!);
                ((System.Runtime.InteropServices.ComTypes.IPersistFile)sl).Save(link, true);
            }
            catch (Exception e)
            {
                ShortcutError = $"{Path.GetFileName(link)}: {e.Message}";
            }
        }
    }

    /// The last shortcut that could not be made, for the log
    public static string? ShortcutError;

    [System.Runtime.InteropServices.ComImport, System.Runtime.InteropServices.Guid("00021401-0000-0000-C000-000000000046")]
    class ShellLink { }

    [System.Runtime.InteropServices.ComImport, System.Runtime.InteropServices.Guid("000214F9-0000-0000-C000-000000000046"),
     System.Runtime.InteropServices.InterfaceType(System.Runtime.InteropServices.ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellLinkW
    {
        void GetPath([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder file, int max, IntPtr fd, uint flags);
        void GetIDList(out IntPtr ppidl);
        void SetIDList(IntPtr pidl);
        void GetDescription([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder name, int max);
        void SetDescription([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string name);
        void GetWorkingDirectory([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder dir, int max);
        void SetWorkingDirectory([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string dir);
        void GetArguments([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder args, int max);
        void SetArguments([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string args);
        void GetHotkey(out short hotkey);
        void SetHotkey(short hotkey);
        void GetShowCmd(out int show);
        void SetShowCmd(int show);
        void GetIconLocation([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder path, int max, out int index);
        void SetIconLocation([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string path, int index);
        void SetRelativePath([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string path, uint reserved);
        void Resolve(IntPtr hwnd, uint flags);
        void SetPath([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string file);
    }
}
