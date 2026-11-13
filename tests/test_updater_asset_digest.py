"""Exercise the actual Windows updater with authored HTTP/launch/temp-directory boundaries only.

Built for net8.0, the builder's framework, where this .NET SDK has its reference pack (CI installs .NET 8). An SDK of a
later .NET alone cannot restore net8.0 here (NuGet sources are cleared: NU1100), and the fixture is then built for that
SDK's own framework, with a line saying so. Not on GitHub's runners: there .NET 8 is installed for the builder, so a
missing reference pack (or an updater that no longer builds for net8.0) fails the check instead of being built around."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--dotnet", "{dotnet}"]
PROJECT = """<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<OutputType>Exe</OutputType><TargetFramework>%s</TargetFramework><ImplicitUsings>enable</ImplicitUsings>
<Nullable>enable</Nullable><TreatWarningsAsErrors>true</TreatWarningsAsErrors><NuGetAudit>false</NuGetAudit>
<UseSharedCompilation>false</UseSharedCompilation>
</PropertyGroup></Project>"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--dotnet", default="dotnet")
    args = parser.parse_args()
    dotnet = shutil.which(args.dotnet)
    if not dotnet:
        parser.error("a .NET SDK is required (the builder's is .NET 8)")
    source = (args.source / "builder/HookshotBuilder/Updater.cs").read_text(encoding="utf-8")
    # Compile the entire real class. Aliases replace only external boundaries, never its control flow or validation.
    aliases = """using HttpClient = DigestFixture.FakeHttpClient;
using Process = DigestFixture.FakeProcess;
using Environment = DigestFixture.FakeEnvironment;
using AppContext = DigestFixture.FakeAppContext;
using BuilderHome = DigestFixture.FakeBuilderHome;
using PortableArchive = DigestFixture.UnusedPortableArchive;
"""
    fixture = (ROOT / "tests/updater_asset_digest_test.cs").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="updater-digest-build-") as folder:
        root = Path(folder)
        (root / "Updater.cs").write_text(aliases + source, encoding="utf-8")
        (root / "Program.cs").write_text(fixture, encoding="utf-8")
        (root / "NuGet.Config").write_text("<configuration><packageSources><clear/></packageSources></configuration>", encoding="utf-8")
        project = root / "Fixture.csproj"
        project.write_text(PROJECT % "net8.0", encoding="utf-8")
        restore = subprocess.run([dotnet, "restore", str(project), "--disable-build-servers"], capture_output=True, text=True)
        if restore.returncode and "NU1100" in restore.stdout + restore.stderr:
            if os.environ.get("GITHUB_ACTIONS") == "true":   # (as tests/run_all.py tells a runner from a PC)
                raise SystemExit("on a GitHub runner the updater must build for the builder's net8.0, and its reference "
                                 "pack is missing: no fallback to a later framework here\n" + restore.stdout + restore.stderr)
            sdk = subprocess.run([dotnet, "--version"], capture_output=True, text=True, check=True).stdout.strip()
            framework = "net%s.0" % sdk.split(".")[0]
            print("this .NET SDK (%s) has no .NET 8 reference pack: the updater is built for %s, not the builder's net8.0"
                  % (sdk, framework), flush=True)
            project.write_text(PROJECT % framework, encoding="utf-8")
        elif restore.returncode:
            raise SystemExit(restore.stdout + restore.stderr)
        subprocess.run([dotnet, "run", "--project", str(project), "--configuration", "Release",
                        "--disable-build-servers"], check=True)


if __name__ == "__main__":
    main()
