"""The Builder's search for game files ends even when a drive never answers. 2026-11-14: a player's Builder stayed on "Looking for
your game files…" for good: it searched Downloads, Desktop, Documents, home and every drive one after another with no time limit,
and one drive that did not answer (a USB drive asleep, a cloud or network drive) held the start-up page.
Both Builders search every place at once and go on with what they have after a time limit. The Python Builder's search (core.py)
is run here with a place that hangs and one that throws; the Windows Builder's DumpSearch.cs is run the same way under .NET 8
(tests/builder_dump_search_test.cs), and its window is checked to search through it.

    python -I -B tests/test_builder_dump_search.py [--dotnet dotnet]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

RUN_ALL_ARGS = ["--dotnet", "{dotnet}"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder/py"))


def check_python():
    from hookshot_builder import core
    with tempfile.TemporaryDirectory(prefix="hookshot-dumps-") as folder:
        downloads = Path(folder) / "Downloads é"
        (downloads / "OoT").mkdir(parents=True)
        (downloads / ".hidden").mkdir()
        (downloads / "a.nup").write_text("x")
        (downloads / "OoT/b.nup").write_text("xy")
        (downloads / ".hidden/c.nup").write_text("xyz")
        os.utime(downloads / "a.nup", (1.7e9, 1.7e9))
        os.utime(downloads / "OoT/b.nup", (1.71e9, 1.71e9))
        assert len(core.look_for_dumps(str(downloads), "Downloads")) == 2
        assert core.look_for_dumps(str(Path(folder) / "nowhere"), "X") == []

        never = threading.Event()

        def look(d, name):
            if d == "Z:\\":
                never.wait()
            if d == "Y:\\":
                raise OSError("the drive went away")
            return core.look_for_dumps(d, name)

        t = time.monotonic()
        found = core.search_dumps([(str(downloads), "Downloads"), ("Z:\\", "Z:"), ("Y:\\", "Y:"), (str(downloads), "Home")],
                                  1.5, look)
        took = time.monotonic() - t
        assert took < 5, "the search waited %.1f s on a place that never answers (limit 1.5 s)" % took
        assert [Path(d.path).name for d in found] == ["b.nup", "a.nup"], found
        assert all(d.where == "Downloads" for d in found), found
        t = time.monotonic()
        core.search_dumps([(str(downloads), "Downloads")], 60, core.look_for_dumps)
        assert time.monotonic() - t < 5, "the search waited out its limit though every place had answered"
        never.set()
    window = (ROOT / "builder/py/hookshot_builder/window.py").read_text(encoding="utf-8")
    run = window[window.index('self.splash.text = "Looking for your game files…"'):window.index("def found_files")]
    assert "except Exception" in run, "a search that throws leaves the start-up page waiting for good"


def check_windows_source():
    finder = (ROOT / "builder/HookshotBuilder/Finder.cs").read_text(encoding="utf-8")
    find = finder[finder.index("public static List<Dump> FindDumps()"):finder.index("public static string Ago(")]
    assert "return DumpSearch.Search(places, DumpSearch.Budget, DumpSearch.Look);" in find
    assert "d.IsReady" not in find, "IsReady asks the drive and waits on one that does not answer"
    assert "EnumerateFiles" not in find
    window = (ROOT / "builder/HookshotBuilder/MainWindow.xaml.cs").read_text(encoding="utf-8")
    start = window.index("    void ApplySettings()")
    apply = window[start:window.index("\n    }", start)]
    # (the last build's files, which may be on a drive that does not answer: asked with a time limit, on the UI thread)
    assert "File.Exists(" not in apply and "Directory.Exists(" not in apply, "ApplySettings asks a saved path without a time limit"
    assert "DumpSearch.Exists(" in apply


def check_windows(dotnet):
    with tempfile.TemporaryDirectory(prefix="builder-dumps-") as folder:
        root = Path(folder)
        shutil.copyfile(ROOT / "tests/builder_dump_search_test.cs", root / "Program.cs")
        shutil.copyfile(ROOT / "builder/HookshotBuilder/DumpSearch.cs", root / "DumpSearch.cs")
        (root / "NuGet.Config").write_text("<configuration><packageSources><clear/></packageSources></configuration>", encoding="utf-8")
        (root / "Fixture.csproj").write_text('''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework><ImplicitUsings>enable</ImplicitUsings>
<Nullable>enable</Nullable><TreatWarningsAsErrors>true</TreatWarningsAsErrors><NuGetAudit>false</NuGetAudit>
</PropertyGroup></Project>''', encoding="utf-8")
        subprocess.run([dotnet, "run", "--project", str(root / "Fixture.csproj"), "--configuration", "Release"], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dotnet", default="dotnet")
    a = p.parse_args()
    check_python()
    check_windows_source()
    dotnet = shutil.which(a.dotnet)
    if dotnet:
        check_windows(dotnet)
    else:
        print("(no .NET 8 SDK here: the Windows Builder's search was checked by its source only)")
    print("builder dump search: ok")


if __name__ == "__main__":
    main()
