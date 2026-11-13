using System.Diagnostics;
using System.Reflection;
using System.Text.Json;
using HookshotBuilder;

static class Program
{
    const string Key = "HOOKSHOT_JOBS";
    static readonly MethodInfo InfoMethod = typeof(Runner).GetMethod("Info", BindingFlags.Static | BindingFlags.NonPublic)!;
    static int checks;

    static void Require(bool ok, string message)
    {
        if (!ok) throw new Exception(message);
        checks++;
    }

    static ProcessStartInfo Info(Tools tools, string[] args, int? workers = null, int? memoryGb = null) =>
        (ProcessStartInfo)InfoMethod.Invoke(null, new object?[] { tools, args, workers, null, memoryGb, null })!;   // (ngxSdk, ffxSdk: none)

    static async Task<JsonElement> Delivery(ProcessStartInfo info)
    {
        using var child = Process.Start(info)!;
        var output = await child.StandardOutput.ReadToEndAsync();
        var error = await child.StandardError.ReadToEndAsync();
        await child.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(15));
        Require(child.ExitCode == 0 && error.Length == 0, "The authored argument/environment child failed.");
        using var json = JsonDocument.Parse(output.Trim());
        return json.RootElement.Clone();
    }

    static void CheckInfo(ProcessStartInfo info, Tools tools, string[] args, string? expected)
    {
        Require(info.ArgumentList.SequenceEqual(new[] { "-I", Path.Combine(tools.Root, "nx2", "make_game.py") }.Concat(args)),
            "The worker choice changed the original source/update arguments.");
        Require(info.WorkingDirectory == tools.Root && !info.UseShellExecute && info.CreateNoWindow
            && info.RedirectStandardOutput && info.RedirectStandardError, "Runner process options changed.");
        info.Environment.TryGetValue(Key, out var actual);
        Require(actual == expected, "Unexpected child worker override.");
    }

    static async Task EmptyParent(string python, Tools tools)
    {
        // Environment.SetEnvironmentVariable(Key, "") may remove the variable on Windows instead of keeping it empty.
        // Start a real parent with an explicit empty environment entry, then exercise the actual Runner.Info inheritance.
        Require(Environment.GetEnvironmentVariable(Key) == "", "The empty-parent fixture did not inherit an empty entry.");
        var info = Info(tools, new[] { "--out", "authored output" });
        CheckInfo(info, tools, new[] { "--out", "authored output" }, "");
        var value = await Delivery(info);
        Require(value.GetProperty("jobs").GetString() == "", "Automatic changed an inherited empty entry.");
    }

    /// The shaders' step: the update's argument from the saved choice (the launcher's Update and the --update start read it, not
    /// the tick), and what the row says when make_game left the shaders to the first start
    static void ShaderStepChecks(string root)
    {
        var settings = Path.Combine(root, "builder-settings.json");
        Require(BuilderSettings.PrepareNow(settings), "No saved settings must prepare the shaders, as the tick's default.");
        File.WriteAllText(settings, "not json");
        Require(BuilderSettings.PrepareNow(settings), "Unreadable settings must prepare the shaders.");
        File.WriteAllText(settings, """{"Out":"authored output","PrepareLater":true}""");
        Require(!BuilderSettings.PrepareNow(settings), "A saved unticked choice must leave the shaders to the first start.");
        Require(ShaderStep.Args(new List<string> { "--update", "authored game" }, BuilderSettings.PrepareNow(settings))
                .SequenceEqual(new[] { "--update", "authored game", "--no-prepare-shaders" }), "The update ignored the saved choice.");
        File.WriteAllText(settings, """{"Out":"authored output","PrepareLater":false}""");
        Require(ShaderStep.Args(new List<string> { "--update", "authored game" }, BuilderSettings.PrepareNow(settings))
                .SequenceEqual(new[] { "--update", "authored game" }), "A saved ticked choice must prepare the shaders.");

        Require(ShaderStep.Reason("the shaders were not prepared now (no usable Vulkan here): the game prepares them at its first start")
                == "no usable Vulkan here", "make_game's not-prepared line was not read.");
        Require(ShaderStep.Reason("the shaders were not prepared now (\"Prepare shaders now\" is unticked (here)): the game prepares them at its first start")
                == "\"Prepare shaders now\" is unticked (here)", "A reason with brackets was cut.");
        Require(ShaderStep.Reason("@@stage shaders Preparing shaders") is null && ShaderStep.Reason("done shaders") is null,
                "Other lines are not a skipped step.");
        Require(ShaderStep.Skipped(began: true, reason: null) is null, "Prepared shaders must get their check mark.");
        Require(ShaderStep.Skipped(began: false, reason: null) is { } none && none.Contains("first start"),
                "No @@stage shaders (an older make_game, or none ran) must not be a check mark.");
        Require(ShaderStep.Skipped(began: true, reason: "the game printed nothing for 15 minutes and was stopped") is { } stopped
                && stopped.Contains("15 minutes"), "A stopped preparation must say why under its row.");
    }

    static async Task Main(string[] args)
    {
        if (args.Length is < 1 or > 2) throw new Exception("Pass the Python executable for the authored child.");
        var emptyParent = args.Length == 2 && args[0] == "--empty-parent";
        var python = Path.GetFullPath(args[^1]);
        var original = Environment.GetEnvironmentVariable(Key);
        var originalHome = Environment.GetEnvironmentVariable("HOOKSHOT_SETUP_BASE");
        var originalPortable = Environment.GetEnvironmentVariable("HOOKSHOT_BUILDER_PORTABLE");
        var root = Path.Combine(Path.GetTempPath(), "hookshot-workers-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(root, "nx2"));
        Environment.SetEnvironmentVariable("HOOKSHOT_SETUP_BASE", root);
        Environment.SetEnvironmentVariable("HOOKSHOT_BUILDER_PORTABLE", null);
        try
        {
            Require(BuilderHome.Base == root && !BuilderHome.Portable, "The worker fixture did not use its isolated standard home.");
            // This is authored test content, not the real pipeline, game files or a compiler invocation.
            File.WriteAllText(Path.Combine(root, "nx2", "make_game.py"), """
                import json, os, sys
                text = json.dumps({'argv': sys.argv[1:], 'jobs': os.environ.get('HOOKSHOT_JOBS'), 'memory': os.environ.get('HOOKSHOT_MEM_LIMIT_GB')})
                print(('@@check ok ' if '--check' in sys.argv else '') + text, flush=True)
                """);
            var tools = new Tools(root, python);
            if (emptyParent)
            {
                await EmptyParent(python, tools);
                Console.WriteLine("Empty inherited worker entry passed.");
                return;
            }

            ShaderStepChecks(root);
            Require(BuildWorkers.Choices(4).SequenceEqual(new object[] { BuildWorkers.Automatic, 1, 2, 3, 4 }),
                "The menu must offer Automatic first and one count per logical processor.");
            Require(BuildWorkers.Choices(0).SequenceEqual(new object[] { BuildWorkers.Automatic, 1 }), "CPU fallback must still offer one worker.");
            Require(BuildWorkers.Selected(BuildWorkers.Automatic) is null && BuildWorkers.Selected(3) == 3, "Worker selection failed.");
            foreach (var invalid in new object?[] { null, "4", "automatic", 0, -1, 2L, new object() })
            {
                try { BuildWorkers.Selected(invalid); throw new Exception("Invalid selection accepted."); }
                catch (ArgumentException e) { Require(e.Message.Contains("Automatic"), "Invalid choice needs an actionable message."); }
            }
            foreach (var invalid in new[] { 0, -1 })
            {
                try { new Runner(tools, new List<string>(), invalid); throw new Exception("Invalid worker count accepted."); }
                catch (ArgumentOutOfRangeException) { checks++; }
            }

            Require(BuildMemory.Choices(32).SequenceEqual(new object[] { BuildMemory.Automatic, 4, 8, 12, 16, 20, 24, 28, 32 }),
                "The memory menu must offer Automatic first, then steps up to the PC's memory.");
            Require(BuildMemory.Choices(64).Count() == 17 && BuildMemory.Choices(64).Last() is 64,
                "A 64 GB PC must be offered every step up to 64.");
            Require(BuildMemory.Choices(31).Last() is 32 && BuildMemory.Choices(8).Last() is 8,
                "The memory menu must end on the step at or just above the PC's memory.");
            Require(BuildMemory.Choices(0).SequenceEqual(new object[] { BuildMemory.Automatic }), "An unknown total must still offer Automatic.");
            Require(BuildMemory.TotalGb() > 0, "The PC's memory could not be read.");
            Require(BuildMemory.Selected(BuildMemory.Automatic) is null && BuildMemory.Selected(8) == 8, "Memory selection failed.");
            var menu = BuildMemory.Choices(32).ToList();
            Require(BuildMemory.Saved(menu, 12) is 12, "A saved memory choice must be selected again.");
            Require(BuildMemory.Saved(menu, null) is string none && none == BuildMemory.Automatic, "No saved choice must select Automatic.");
            Require(BuildMemory.Saved(menu, 64) is string gone && gone == BuildMemory.Automatic, "A choice this PC no longer offers must select Automatic.");
            var settings = Path.Combine(Path.GetTempPath(), "hookshot-builder-settings-" + Guid.NewGuid().ToString("N") + ".json");
            try
            {
                File.WriteAllText(settings, System.Text.Json.JsonSerializer.Serialize(new BuilderSettings { MemoryGb = 16 }));
                Require(BuilderSettings.Load(settings)?.MemoryGb == 16, "The memory choice must be saved for the updates.");
                File.WriteAllText(settings, "{\"Dlss\":true}");
                Require(BuilderSettings.Load(settings) is { MemoryGb: null }, "Settings saved before the memory choice must mean Automatic.");
            }
            finally { File.Delete(settings); }
            foreach (var invalid in new object?[] { null, "8", "automatic", 0, -4, 8L, new object() })
            {
                try { BuildMemory.Selected(invalid); throw new Exception("Invalid memory selection accepted."); }
                catch (ArgumentException e) { Require(e.Message.Contains("Automatic"), "Invalid memory choice needs an actionable message."); }
            }
            foreach (var invalid in new[] { 0, -4 })
            {
                try { new Runner(tools, new List<string>(), null, null, invalid); throw new Exception("Invalid memory accepted."); }
                catch (ArgumentOutOfRangeException) { checks++; }
            }
            {
                var command = new[] { "--update", "authored existing game" };
                Info(tools, command).Environment.TryGetValue("HOOKSHOT_MEM_LIMIT_GB", out var automatic);
                Require(automatic == Environment.GetEnvironmentVariable("HOOKSHOT_MEM_LIMIT_GB"), "Automatic must leave the memory limit as inherited.");
                var value = await Delivery(Info(tools, command, null, 12));
                Require(value.GetProperty("memory").GetString() == "12", "The selected memory did not reach the child.");
            }

            var sourceArguments = new[]
            {
                new[] { "--nup", "authored file with spaces.nup", "--out", "authored output", "--language", "en-US" },
                new[] { "--romfs", "authored romfs", "--exefs", "authored exefs", "--out", "authored output", "--language", "fr" },
                new[] { "--update", "authored existing game" },
            };
            foreach (var inherited in new string?[] { null, "4", "0" })
            {
                Environment.SetEnvironmentVariable(Key, inherited);
                foreach (var command in sourceArguments)
                {
                    var info = Info(tools, command);
                    CheckInfo(info, tools, command, inherited);
                    Require(Environment.GetEnvironmentVariable(Key) == inherited, "Automatic changed its parent environment.");
                }
            }
            foreach (var inherited in new[] { "2", "not a number" })
            {
                Environment.SetEnvironmentVariable(Key, inherited);
                var command = sourceArguments[0];
                var info = Info(tools, command, 4);
                CheckInfo(info, tools, command, "4");
                var value = await Delivery(info);
                Require(value.GetProperty("jobs").GetString() == "4", "The selected worker count did not reach the child.");
                Require(value.GetProperty("argv").EnumerateArray().Select(e => e.GetString()).SequenceEqual(command), "Authored child arguments changed.");
                Require(Environment.GetEnvironmentVariable(Key) == inherited, "Manual selection changed the parent environment.");
            }

            Environment.SetEnvironmentVariable(Key, "6");
            var check = await Runner.Check(tools, new[] { "--check", "--nup", "authored check" });
            Require(check.Ok, "Runner.Check no longer recognizes its real protocol.");
            using (var value = JsonDocument.Parse(check.Text))
                Require(value.RootElement.GetProperty("jobs").GetString() == "6", "Check must retain the inherited override.");
            Require(Environment.GetEnvironmentVariable(Key) == "6", "Check changed its parent environment.");

            var completed = new TaskCompletionSource<string?>(TaskCreationOptions.RunContinuationsAsynchronously);
            string? delivered = null;
            var runner = new Runner(tools, sourceArguments[2].ToList(), 3);
            runner.Line += line => delivered = line;
            runner.Finished += failure => completed.TrySetResult(failure);
            runner.Start();
            Require(await completed.Task.WaitAsync(TimeSpan.FromSeconds(15)) is null, "Actual Runner.Start did not finish its authored child.");
            using (var value = JsonDocument.Parse(delivered!))
                Require(value.RootElement.GetProperty("jobs").GetString() == "3", "Runner.Start dropped the selected count.");
            Require(Environment.GetEnvironmentVariable(Key) == "6", "Runner.Start changed its parent environment.");

            var self = new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
            if (string.Equals(Path.GetFileNameWithoutExtension(Environment.ProcessPath), "dotnet", StringComparison.OrdinalIgnoreCase))
                self.ArgumentList.Add(Assembly.GetExecutingAssembly().Location);
            self.ArgumentList.Add("--empty-parent"); self.ArgumentList.Add(python);
            self.Environment[Key] = "";
            using var empty = Process.Start(self)!;
            var emptyOutput = await empty.StandardOutput.ReadToEndAsync();
            var emptyError = await empty.StandardError.ReadToEndAsync();
            await empty.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(15));
            Require(empty.ExitCode == 0 && emptyError.Length == 0 && emptyOutput.Contains("Empty inherited worker entry passed."), "Empty inheritance check failed: " + emptyError);
            Console.WriteLine($"Windows Builder worker checks passed ({checks} assertions). Only authored argv/HOOKSHOT_JOBS were inspected.");
        }
        finally
        {
            Environment.SetEnvironmentVariable(Key, original);
            Environment.SetEnvironmentVariable("HOOKSHOT_SETUP_BASE", originalHome);
            Environment.SetEnvironmentVariable("HOOKSHOT_BUILDER_PORTABLE", originalPortable);
            Directory.Delete(root, recursive: true);
        }
    }
}
