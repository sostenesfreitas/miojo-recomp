"""The Windows Builder's UpscalerParts.Missing (an up-to-date game without a ticked NVIDIA DLSS or AMD FSR 4 offers Update, not
Play), run on authored placeholder folders without WPF, a game, NVIDIA's SDK or AMD's runtime; and a source check that the
Builder asks it when the game is up to date and again when either tick changes."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--dotnet", "{dotnet}"]
RUN_ALL_SKIP = {"linux": "Windows Builder; .NET 8 fixture exercised on Windows"}

ROOT = Path(__file__).resolve().parents[1]


def check_source(source):
    window = (source / "builder/HookshotBuilder/MainWindow.xaml.cs").read_text(encoding="utf-8")
    refresh = window[window.index("    void RefreshMode()"):window.index("\n    }", window.index("    void RefreshMode()"))]
    assert "UpscalerParts.Missing(OutPath.Text, DlssTick.IsChecked == true, FsrTick.IsChecked == true, fsrLicense)" in refresh
    assert "mode == Mode.UpToDate" in refresh and "if (lacking.Count > 0) mode = Mode.UpdateGame;" in refresh
    assert refresh.index("case Mode.UpdateGame when lacking.Count > 0:") < refresh.index("case Mode.UpdateGame:")
    for tick in ("void DlssChanged(", "void FsrChanged("):
        body = window[window.index(tick):window.index("\n    }", window.index(tick))]
        assert "RefreshMode();" in body, tick + " no longer refreshes the Build button"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=ROOT)
    p.add_argument("--dotnet", default="dotnet")
    a = p.parse_args()
    check_source(a.source)
    dotnet = shutil.which(a.dotnet)
    if not dotnet:
        p.error(".NET 8 SDK is required for the Windows Builder upscaler fixture")
    with tempfile.TemporaryDirectory(prefix="builder-upscalers-") as folder:
        root = Path(folder)
        shutil.copyfile(ROOT / "tests/builder_upscaler_parts_test.cs", root / "Program.cs")
        shutil.copyfile(a.source / "builder/HookshotBuilder/UpscalerParts.cs", root / "UpscalerParts.cs")
        (root / "NuGet.Config").write_text("<configuration><packageSources><clear/></packageSources></configuration>", encoding="utf-8")
        (root / "Fixture.csproj").write_text('''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework><ImplicitUsings>enable</ImplicitUsings>
<Nullable>enable</Nullable><TreatWarningsAsErrors>true</TreatWarningsAsErrors><NuGetAudit>false</NuGetAudit>
</PropertyGroup></Project>''', encoding="utf-8")
        subprocess.run([dotnet, "run", "--project", str(root / "Fixture.csproj"), "--configuration", "Release"], check=True)


if __name__ == "__main__":
    main()
