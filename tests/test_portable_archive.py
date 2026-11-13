"""Execute portable ZIP installation with authored packages and real filesystem operations."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--dotnet", "{dotnet}"]
RUN_ALL_SKIP = {"linux": "Windows portable home path contract"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dotnet", default="dotnet")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="portable-archive-") as folder:
        root = Path(folder)
        for name in ("BuilderHome.cs", "PortableArchive.cs"):
            shutil.copyfile(ROOT / "builder/HookshotBuilder" / name, root / name)
        (root / "NuGet.Config").write_text("<configuration><packageSources><clear/></packageSources></configuration>")
        (root / "Fixture.csproj").write_text('''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework><ImplicitUsings>enable</ImplicitUsings>
<Nullable>enable</Nullable><TreatWarningsAsErrors>true</TreatWarningsAsErrors><NuGetAudit>false</NuGetAudit>
<UseSharedCompilation>false</UseSharedCompilation></PropertyGroup></Project>''')
        (root / "Program.cs").write_text('''using System.IO.Compression;
using HookshotBuilder;
var root = Path.Combine(AppContext.BaseDirectory, "authored ü, tree");
Directory.CreateDirectory(root);
var home = Path.Combine(root, "Builder"); Directory.CreateDirectory(home);
File.WriteAllText(Path.Combine(home, BuilderHome.Marker), BuilderHome.MarkerText);
var count = 0;
void Check(bool ok, string why) { if (!ok) throw new Exception(why); count++; }
string Pointer() => File.ReadAllText(Path.Combine(home, "versions", "current.txt")).Trim();
string Zip(string name, string version, bool nested = true, bool executable = true) {
    var path = Path.Combine(root, name + ".zip");
    using var archive = ZipFile.Open(path, ZipArchiveMode.Create);
    var prefix = nested ? "HookshotBuilder/" : "";
    if (executable) { using var file = new StreamWriter(archive.CreateEntry(prefix + "HookshotBuilder.exe").Open()); file.Write("authored, never executed"); }
    using (var file = new StreamWriter(archive.CreateEntry(prefix + "tools/version.txt").Open())) file.Write(version);
    return path;
}
void Fails(Action action, string why) { try { action(); } catch (InvalidDataException) { count++; return; } throw new Exception(why); }
var first = Zip("first", "v1");
var target = PortableArchive.Install(first, home, "v1");
Check(File.Exists(Path.Combine(target, "HookshotBuilder.exe")), "complete target");
Check(Pointer() == "v1", "initial pointer");
Check(PortableArchive.Install(first, home, "v1") == target, "same version reuse");
Fails(() => PortableArchive.Install(Zip("wrong", "other"), home, "v2"), "reject wrong version");
Check(Pointer() == "v1", "wrong version preserved current");
Fails(() => PortableArchive.Install(Zip("missing", "v2", executable:false), home, "v2"), "reject missing exe");
Check(Pointer() == "v1", "missing exe preserved current");
Fails(() => PortableArchive.Install(first, home, "../escape"), "reject unsafe tag");
var flat = PortableArchive.Install(Zip("flat", "v2", nested:false), home, "v2");
Check(File.Exists(Path.Combine(flat, "tools", "version.txt")), "flat package normalized");
Check(Pointer() == "v2", "new current");
Check(File.ReadAllText(Path.Combine(home, "versions", "previous.txt")).Trim() == "v1", "previous pointer");
Check(File.Exists(Path.Combine(target, "HookshotBuilder.exe")), "old installation retained");
var invalid = Path.Combine(home, "versions", "v3"); Directory.CreateDirectory(invalid);
File.WriteAllText(Path.Combine(invalid, "keep"), "preserve");
Fails(() => PortableArchive.Install(Zip("third", "v3"), home, "v3"), "reject incomplete existing target");
Check(File.ReadAllText(Path.Combine(invalid, "keep")) == "preserve" && Pointer() == "v2", "existing folder unchanged");
Check(!Directory.EnumerateDirectories(Path.Combine(home, "versions"), ".unpack-*").Any(), "staging cleaned");
Console.WriteLine($"portable archive: {count} checks passed; no network or process launch");
''', encoding="utf-8")
        subprocess.run([args.dotnet, "run", "--project", str(root / "Fixture.csproj"),
                        "--configuration", "Release", "--disable-build-servers"], check=True)


if __name__ == "__main__":
    main()
