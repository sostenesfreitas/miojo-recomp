"""Compile the actual Windows Builder home/storage paths over authored roots; no install, game or process launch."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--dotnet", "{dotnet}", "--cc", "gcc"]
RUN_ALL_SKIP = {"linux": "Windows portable Builder home and Windows path semantics; native .NET fixture on Windows"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--dotnet", default="dotnet")
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="portable-builder-home-") as folder:
        root = Path(folder)
        for name in ("BuilderHome.cs", "Storage.cs"):
            shutil.copyfile(args.source / "builder/HookshotBuilder" / name, root / name)
        shutil.copyfile(ROOT / "tests/portable_builder_home_test.cs", root / "Program.cs")
        (root / "NuGet.Config").write_text("<configuration><packageSources><clear/></packageSources></configuration>", encoding="utf-8")
        (root / "Fixture.csproj").write_text("""<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework><ImplicitUsings>enable</ImplicitUsings>
<Nullable>enable</Nullable><TreatWarningsAsErrors>true</TreatWarningsAsErrors><NuGetAudit>false</NuGetAudit>
<UseSharedCompilation>false</UseSharedCompilation></PropertyGroup></Project>""", encoding="utf-8")
        data = root / "authored filesystem"
        data.mkdir()
        native = root / "native roots é, fixture"
        for part in ("volume old/HookshotBuilder/versions/v-authored/HookshotBuilder", "volume old/Game", "unrelated cwd", "standard", "broken"):
            (native / part).mkdir(parents=True, exist_ok=True)
        exe = root / "native-home.exe"
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-municode",
                        "-I" + str(args.source / "builder/setup"), str(ROOT / "tests/portable_setup_home_test.c"),
                        "-o", str(exe)], check=True)
        subprocess.run([str(exe), str(native)], check=True, timeout=30)
        subprocess.run([args.dotnet, "run", "--project", str(root / "Fixture.csproj"), "--configuration", "Release",
                        "--disable-build-servers", "--", str(data), str(native / "volume moved/HookshotBuilder")], cwd=root, check=True)
        subprocess.run([str(exe), "--read", str(data / "csharp shared")], check=True, timeout=30)


if __name__ == "__main__":
    main()
