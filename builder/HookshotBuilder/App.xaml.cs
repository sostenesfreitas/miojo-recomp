using System.IO;
using System.Text;
using System.Windows;

namespace HookshotBuilder;

public partial class App : Application
{
    // (RecompLauncher) --cli <make_game.py's options>: no window; its lines on stdout, and its result as the exit code.
    // Why it stopped is one "error <text>" line on stdout (the stream a launcher reads) and again on stderr
    protected override void OnStartup(StartupEventArgs e)
    {
        try { _ = BuilderHome.Base; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException)
        {
            var why = "The Builder home could not be opened. " + error.Message;
            if (e.Args.Length > 0 && e.Args[0] == "--cli")
            {
                using var output = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = true };
                output.WriteLine("error " + why);
                Console.Error.WriteLine("error " + why);
            }
            else MessageBox.Show(why, "Hookshot Builder", MessageBoxButton.OK, MessageBoxImage.Error);
            // StartupUri is processed after this handler; a rejected home must never construct MainWindow.
            Environment.Exit(1);
            return;
        }
        if (e.Args.Length > 0 && e.Args[0] == "--cli")
        {
            Environment.Exit(Cli(e.Args.Skip(1).ToList()));
            return;
        }
        base.OnStartup(e);
    }

    static int Cli(List<string> args)
    {
        using var stdout = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = true };
        var tools = Tools.Find();
        if (tools == null)
        {
            const string missing = "error The build tools were not found next to this program.";
            stdout.WriteLine(missing);
            Console.Error.WriteLine(missing);
            return 1;
        }
        var done = new ManualResetEventSlim();
        string? failure = null, said = null;   // (said: make_game's own "error" line in --prepare mode, already passed on to stdout)
        var runner = new Runner(tools, args);
        runner.Line += line =>
        {
            if (line.StartsWith("error ")) said = line[6..];
            lock (stdout) stdout.WriteLine(line);
        };
        runner.Finished += result => { failure = result; done.Set(); };
        runner.Start();
        done.Wait();
        if (failure == null) return 0;
        var reason = failure == "cancelled" ? failure : said ?? failure;
        if (said == null) lock (stdout) stdout.WriteLine("error " + reason);
        Console.Error.WriteLine("error " + reason);
        return 1;
    }
}
