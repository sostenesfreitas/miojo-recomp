"""Validate tested-run fragments and assemble a Markdown view from the source document.

python -I scripts/tested_configurations.py --output work/tested-configurations.md
Without --output, print the view to stdout. See docs/tested/README.md for the row format.
"""
import argparse
from datetime import date
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MARKER = "<!-- tested-run-fragments -->"
HEADER = "| Date | Build | OS / desktop | GPU / driver | CPU / RAM | Settings | Where | Result |"
SEPARATOR = "|---|---|---|---|---|---|---|---|"
NAME = re.compile(r"([0-9]{4}-[0-9]{2}-[0-9]{2})-[a-z0-9]+(?:-[a-z0-9]+){2,}\.md")
CONTEXT = re.compile(r"\b(?:as\s+above|same\s+host|ditto)\b", re.I)


def cells(row):
    """Markdown pipes escaped with an odd number of backslashes belong to their cell."""
    parts, start, slashes = [], 0, 0
    for i, char in enumerate(row):
        if char == "|" and slashes % 2 == 0:
            parts.append(row[start:i].strip())
            start = i + 1
        slashes = slashes + 1 if char == "\\" else 0
    parts.append(row[start:].strip())
    if parts[0] or parts[-1] or len(parts) != 10 or any(not cell for cell in parts[1:-1]):
        raise ValueError("expected eight non-empty cells between pipes (escape a literal pipe as \\|)")
    return parts[1:-1]


def row_key(values):
    """Whitespace is insignificant only when comparing rows, never when rendering them."""
    return tuple(" ".join(cell.split()) for cell in values)


def read_row(path):
    # read_text's universal-newline conversion would hide CRLF in a fragment.
    text = path.read_bytes().decode("utf-8")
    lines = text.removesuffix("\n").split("\n")
    if "\r" in text or len(lines) != 3 or lines[:2] != [HEADER, SEPARATOR]:
        raise ValueError(f"{path.name}: expected the standard header, separator and exactly one row (LF)")
    try:
        values = cells(lines[2])
        day = values[0].removesuffix(" UTC")
        if not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", day):
            raise ValueError("date must be YYYY-MM-DD, optionally followed by UTC")
        date.fromisoformat(day)
        match = NAME.fullmatch(path.name)
        if not match or match[1] != day:
            raise ValueError("filename must be YYYY-MM-DD-topic-tester-run.md, with the row's date")
        if any(CONTEXT.search(cell) for cell in values):
            raise ValueError("new rows must stand alone: repeat details instead of 'as above', 'same host' or 'ditto'")
    except ValueError as error:
        raise ValueError(f"{path.name}: {error}") from error
    return lines[2], row_key(values)


def read_source(path):
    """Read only the frozen table for comparison; keep both sides of the marker verbatim."""
    text = path.read_bytes().decode("utf-8")
    if text.count(MARKER) != 1:
        raise ValueError(f"{path.name}: expected exactly one {MARKER} marker")
    prefix, suffix = text.split(MARKER)
    if (prefix and not prefix.endswith("\n")) or not suffix.startswith("\n"):
        raise ValueError(f"{path.name}: the fragment marker must be on its own line")
    if not suffix.lstrip("\n").startswith("## Earlier runs (frozen)\n"):
        raise ValueError(f"{path.name}: expected Earlier runs (frozen) after the fragment marker")
    lines = suffix.splitlines()
    limits = [i for i, line in enumerate(lines) if re.match(r"^## Limits(?:\s|$)", line)]
    if lines.count(HEADER) != 1 or lines.count(SEPARATOR) != 1 or len(limits) != 1:
        raise ValueError(f"{path.name}: expected one frozen table and one Limits section")
    start = lines.index(HEADER)
    if start + 2 >= limits[0] or lines[start + 1] != SEPARATOR or lines[limits[0] - 1].strip():
        raise ValueError(f"{path.name}: expected the standard frozen table before Limits")
    frozen = lines[start + 2:limits[0]]
    while frozen and not frozen[-1].strip():
        frozen.pop()
    if not frozen:
        raise ValueError(f"{path.name}: missing frozen rows")
    for line in frozen:
        try:
            cells(line)
        except ValueError as error:
            raise ValueError(f"{path.name}: frozen table: {error}") from error
    return prefix, suffix, frozen


def render(root=ROOT):
    prefix, suffix, frozen = read_source(root / "docs/tested-configurations.md")
    # Frozen duplicates and context references stay untouched. Only new rows must be unique.
    seen = {row_key(cells(row)): "the frozen table" for row in frozen}
    new = []
    for path in sorted((root / "docs/tested").iterdir(), reverse=True):
        if path.suffix != ".md" or path.name == "README.md":
            continue
        row, key = read_row(path)
        if key in seen:
            raise ValueError(f"{path.name}: duplicate row from {seen[key]}")
        seen[key] = path.name
        new.append(row)
    section = HEADER + "\n" + SEPARATOR + "\n" + "\n".join(new) if new else "No new run fragments yet."
    return prefix + "## New runs\n\n" + section + suffix


def unicode_output():
    """Pipes get UTF-8; consoles keep their encoding, as in tests/run_all.py and team_chat.py."""
    for stream in (sys.stdout, sys.stderr):
        if not hasattr(stream, "reconfigure"):   # A StringIO, as in the tests.
            continue
        if stream.isatty():
            stream.reconfigure(errors="replace")
        else:
            stream.reconfigure(encoding="utf-8", errors="replace")


def main(argv=None):
    unicode_output()
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--output", type=Path, help="write the assembled view as UTF-8 (do not commit it)")
    args = parser.parse_args(argv)
    try:
        content = render()
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(content, encoding="utf-8", newline="\n")
        else:
            sys.stdout.write(content)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"tested configurations: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
