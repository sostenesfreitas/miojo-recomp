"""Published WPF CLI: execute real startup and Runner with an authored child, never a game or GUI mode."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import traceback

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--dotnet", "{dotnet}"]
RUN_ALL_SKIP = {"linux": "Windows Desktop WPF application startup"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dotnet", default="dotnet")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="portable-wpf-cli-") as folder:
        root = Path(folder)
        project = root / "project"
        shutil.copytree(ROOT / "builder/HookshotBuilder", project, ignore=shutil.ignore_patterns("bin", "obj"))
        home = root / "Original ü, volume/HookshotBuilder"
        app = home / "versions/v-authored/HookshotBuilder"
        published = root / "published"
        # Exercise the shipped self-contained package, not a Debug app that depends on an installed Desktop runtime.
        subprocess.run([args.dotnet, "publish", str(project / "HookshotBuilder.csproj"), "--configuration", "Release",
                        "-p:NuGetAudit=false", "--output", str(published), "--disable-build-servers", "-v", "quiet"], check=True)
        shutil.copytree(published, app)
        marker = home / ".hookshot-portable"
        marker.write_text("Hookshot Builder portable home v1\n", encoding="utf-8")
        script = app / "tools/nx2/make_game.py"
        script.parent.mkdir(parents=True)
        script.write_text('import os,json\nprint(json.dumps({"home":os.environ.get("HOOKSHOT_SETUP_BASE"),"portable":os.environ.get("HOOKSHOT_BUILDER_PORTABLE")}))\n', encoding="utf-8")
        env = {k: v for k, v in os.environ.items() if k.upper() not in ("HOOKSHOT_SETUP_BASE", "HOOKSHOT_BUILDER_PORTABLE")}
        # A portable installation must start on a PC without .NET. Scope discovery to an owned empty folder;
        # publishing above still uses the SDK, while the actual app must use its packaged runtime below.
        runtime_root = root / "no-installed-runtime"
        runtime_root.mkdir()
        env.update(DOTNET_ROOT=str(runtime_root), DOTNET_ROOT_X64=str(runtime_root), DOTNET_MULTILEVEL_LOOKUP="0")
        runtime = json.loads((app / "HookshotBuilder.runtimeconfig.json").read_text(encoding="utf-8"))["runtimeOptions"]
        assert "includedFrameworks" in runtime and "framework" not in runtime and "frameworks" not in runtime
        assert (app / "coreclr.dll").is_file() and (app / "PresentationFramework.dll").is_file()
        # The authored tools fixture uses the trusted interpreter running this check, not another PATH entry.
        env["PATH"] = str(Path(os.sys.executable).parent) + os.pathsep + env.get("PATH", "")
        scenarios = 0
        def run(overrides, valid, expected_home=None):
            nonlocal scenarios
            result = subprocess.run([str(app / "HookshotBuilder.exe"), "--cli", "--authored-probe"],
                                    env=dict(env, **overrides), cwd=root, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=30,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            if valid:
                assert result.returncode == 0, result.stdout + result.stderr
                child = json.loads(next(x for x in result.stdout.splitlines() if x.startswith("{")))
                # .NET expands DOS aliases such as RUNNER~1; textual equality does not identify the directory.
                assert Path(child["home"]).samefile(expected_home or home) and child["portable"] == "1", child
            else:
                assert result.returncode == 1, result.stdout + result.stderr
                assert "error The Builder home could not be opened." in result.stdout
                assert "Unhandled exception" not in result.stderr
                assert not any(x.startswith("{") for x in result.stdout.splitlines())
            scenarios += 1
        run({"HOOKSHOT_SETUP_BASE": str(home), "HOOKSHOT_BUILDER_PORTABLE": "1"}, True)
        run({}, True)
        short_path = ctypes.windll.kernel32.GetShortPathNameW
        short_path.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_wchar), ctypes.c_uint32]
        short_path.restype = ctypes.c_uint32
        buffer = ctypes.create_unicode_buffer(32768)
        length = short_path(str(home), buffer, len(buffer))
        alias = Path(buffer.value) if 0 < length < len(buffer) else home
        alias_available = str(alias).lower() != str(home).lower()
        if alias_available:
            run({"HOOKSHOT_SETUP_BASE": str(alias), "HOOKSHOT_BUILDER_PORTABLE": "1"}, True, alias)
        moved = root / "Relocated ü, volume"
        home.parent.rename(moved)
        app = moved / app.relative_to(home.parent)
        home = moved / "HookshotBuilder"
        marker = home / ".hookshot-portable"
        run({}, True)
        run({"HOOKSHOT_SETUP_BASE": "relative", "HOOKSHOT_BUILDER_PORTABLE": "1"}, False)
        marker.write_text("broken")
        run({}, False)
        marker.unlink()
        run({"HOOKSHOT_SETUP_BASE": str(home), "HOOKSHOT_BUILDER_PORTABLE": "1"}, False)
        print("Published WPF startup: %d CLI/child/relocation/error scenarios passed; DOS alias exercised: %s" %
              (scenarios, alias_available))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        # run_all decodes child output as UTF-8; keep failure diagnostics printable on legacy Windows consoles too.
        print(traceback.format_exc().encode("ascii", "backslashreplace").decode("ascii"), file=sys.stderr)
        raise SystemExit(1)
