"""Test fragment validation and assembly without a game, network or third-party package.

python -I -B tests/test_tested_configurations.py
The ASCII pipe checks are portable stand-ins, not native Windows runtime evidence.
"""
import contextlib
import importlib.util
import io
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("tested_configurations", ROOT / "scripts/tested_configurations.py")
checked = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checked)
TABLE = checked.HEADER + "\n" + checked.SEPARATOR + "\n"
NAME = "2026-11-13-new-alex-run01.md"
LIMITS = "## Limits of these runs\n\n- Only the stated hardware was tried.\n"


def row(build="example", result="observed", day="2026-11-13"):
    return f"| {day} | {build} | Linux | GPU | CPU | settings | scene | {result} |\n"


class Fragments(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.directory = self.root / "docs/tested"
        self.directory.mkdir(parents=True)
        self.source_path = self.root / "docs/tested-configurations.md"
        self.old = row("old", "size → output")
        self.source()

    def source(self, frozen=None, prefix="# Tested configurations\n\n", limits=LIMITS):
        if frozen is None:
            frozen = [self.old]
        text = (prefix + checked.MARKER + "\n\n## Earlier runs (frozen)\n\n" + TABLE +
                "".join(frozen) + "\n" + limits)
        self.source_path.write_bytes(text.encode("utf-8"))
        return text

    def add(self, name=NAME, text=None):
        path = self.directory / name
        path.write_bytes((TABLE + (row() if text is None else text)).encode("utf-8"))
        return path

    def invoke(self, args, stream=None):
        output = io.StringIO() if stream is None else stream
        errors = io.StringIO()
        render = checked.render
        with mock.patch.object(checked, "render", lambda: render(self.root)), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            code = checked.main(args)
        return code, output, errors.getvalue()

    def test_new_rows_sort_by_date_then_filename_not_creation_order(self):
        newest = row("z-run")
        middle = row("a-run")
        oldest = row("older", day="2026-11-12")
        self.add("2026-11-12-older-alex-run01.md", oldest)
        self.add("2026-11-13-z-alex-run01.md", newest)
        self.add("2026-11-13-a-alex-run01.md", middle)
        prefix, suffix = self.source_path.read_text(encoding="utf-8").split(checked.MARKER)
        self.assertEqual(checked.render(self.root),
                         prefix + "## New runs\n\n" + TABLE +
                         (newest + middle + oldest).removesuffix("\n") + suffix)

    def test_no_fragments_has_a_message_not_an_empty_table(self):
        source = self.source()
        # The tracked instructions page is not a fragment, even if it contains an example.
        (self.directory / "README.md").write_text("Instructions, not a run.\n" + TABLE + row(), encoding="utf-8")
        view = checked.render(self.root)
        self.assertEqual(view, source.replace(checked.MARKER, "## New runs\n\nNo new run fragments yet."))
        self.assertEqual(view.count(checked.HEADER), 1)

    def test_missing_fragment_directory_is_not_reported_as_empty(self):
        self.directory.rmdir()
        with self.assertRaises(FileNotFoundError):
            checked.render(self.root)

    def test_frozen_duplicates_references_and_text_keep_their_order(self):
        frozen = [self.old, row("as above"), row("same host, different renderer"), self.old, row("ditto")]
        source = self.source(frozen, prefix="# Tested configurations\n\nKeep  this\tspacing.\n\n")
        self.add(text=row("new", "separate run"))
        prefix, suffix = source.split(checked.MARKER)
        view = checked.render(self.root)
        self.assertTrue(view.startswith(prefix + "## New runs\n\n"))
        self.assertTrue(view.endswith(suffix))
        self.assertEqual(checked.read_source(self.source_path)[2], [text.removesuffix("\n") for text in frozen])
        self.assertNotIn(checked.MARKER, view)

    def test_later_main_rows_are_read_without_a_refresh_step(self):
        self.add(text=row("new fragment"))
        later = row("later main", "newly landed result", "2026-11-14")
        self.assertNotIn(later, checked.render(self.root))
        source = self.source([later, self.old])
        view = checked.render(self.root)
        self.assertTrue(view.endswith(source.split(checked.MARKER)[1]))
        self.assertIn(row("new fragment"), view)
        self.assertEqual(checked.read_source(self.source_path)[2],
                         [later.removesuffix("\n"), self.old.removesuffix("\n")])
        self.add("2026-11-14-duplicate-alex-run01.md", later)
        with self.assertRaisesRegex(ValueError, "duplicate row from the frozen table"):
            checked.render(self.root)

    def test_fenced_example_in_prefix_is_not_frozen_evidence(self):
        example = row("new")
        prefix = "# Tested configurations\n\n```markdown\n" + TABLE + example + "```\n\n"
        source = self.source(prefix=prefix)
        self.add(text=example)
        view = checked.render(self.root)
        self.assertEqual(view, source.replace(checked.MARKER, "## New runs\n\n" + TABLE + example.rstrip("\n")))

    def test_limits_are_read_once_from_the_source_not_copied_by_the_script(self):
        first = checked.render(self.root)
        changed = "## Limits of these runs\n\n- Changed coverage: arrows → remain.\n"
        self.source(limits=changed)
        second = checked.render(self.root)
        self.assertEqual(first.count(LIMITS), 1)
        self.assertEqual(second.count(changed), 1)
        self.assertNotIn(LIMITS, second)

    def test_new_context_references_are_rejected_in_every_detail_column(self):
        references = ("as above", "As\tABOVE, with changes", "same host, another renderer",
                      "a run on the same host", "ditto", "result: DITTO")
        values = checked.cells(row())
        for column in range(1, len(values)):
            for reference in references:
                with self.subTest(column=column, reference=reference):
                    changed = values.copy()
                    changed[column] = reference
                    self.add(text="| " + " | ".join(changed) + " |\n")
                    with self.assertRaisesRegex(ValueError, "stand alone"):
                        checked.render(self.root)

    def test_copies_of_frozen_rows_are_rejected_after_whitespace_normalization(self):
        for text in (self.old, self.old.replace(" | ", "\t|  ").replace("size → output", "size  →\toutput")):
            with self.subTest(text=text):
                self.add(text=text)
                with self.assertRaisesRegex(ValueError, "duplicate row from the frozen table"):
                    checked.render(self.root)

    def test_duplicate_new_rows_are_rejected_after_whitespace_normalization(self):
        self.add("2026-11-13-new-alex-run01.md", row("new build"))
        self.add("2026-11-13-new-blair-run02.md", row("new\t build").replace(" | ", "  |  "))
        with self.assertRaisesRegex(ValueError, "duplicate row from 2026-11-13-"):
            checked.render(self.root)

    def test_whitespace_and_escaped_pipes_are_preserved_when_rendering(self):
        text = row("new  build\twith spacing", r"`left\|right` and `left\\\|right`")
        path = self.add(text=text)
        self.assertEqual(checked.read_row(path)[0], text.removesuffix("\n"))
        self.assertIn(text, checked.render(self.root))

    def test_odd_backslashes_escape_pipes_even_backslashes_do_not(self):
        for count in (1, 2, 3, 4):
            slashes = "\\" * count
            with self.subTest(count=count):
                text = row(result="left" + slashes + "|right")
                path = self.add(text=text)
                if count % 2:
                    self.assertEqual(checked.read_row(path)[0], text.removesuffix("\n"))
                else:
                    with self.assertRaisesRegex(ValueError, "eight non-empty cells"):
                        checked.read_row(path)
                    # An even run immediately before the closing delimiter is still a delimiter.
                    text = row(result="ends" + slashes).replace(slashes + " |", slashes + "|")
                    path.write_bytes((TABLE + text).encode("utf-8"))
                    self.assertEqual(checked.read_row(path)[0], text.removesuffix("\n"))

    def test_malformed_header_separator_multiple_rows_and_non_lf_fail(self):
        good = TABLE + row()
        malformed = (good.replace("Build", "Other", 1), good.replace("|---|", "|--|", 1),
                     good + row("extra"), TABLE + "<<<<<<< HEAD\n" + row(),
                     "\n" + good, good + "\n", TABLE + "\n" + row(),
                     good.replace("\n", "\r\n"), good.replace("\n", "\r"))
        path = self.add()
        for text in malformed:
            with self.subTest(text=text):
                path.write_bytes(text.encode("utf-8"))
                with self.assertRaisesRegex(ValueError, "standard header"):
                    checked.read_row(path)

    def test_single_final_newline_is_optional(self):
        path = self.add()
        for ending in ("", "\n"):
            with self.subTest(ending=repr(ending)):
                path.write_bytes((TABLE + row().removesuffix("\n") + ending).encode("utf-8"))
                self.assertEqual(checked.read_row(path)[0], row().removesuffix("\n"))

    def test_wrong_columns_empty_cells_and_unescaped_boundaries_fail(self):
        malformed = (row().replace(" | CPU", " | extra | CPU"), row().replace("CPU", "\t "),
                     row().replace(" | CPU", " CPU"), row().removeprefix("|"),
                     row().removesuffix("|\n") + "\\|\n", "outside " + row(),
                     row().removesuffix("\n") + " outside\n")
        for text in malformed:
            with self.subTest(text=text):
                path = self.add(text=text)
                with self.assertRaisesRegex(ValueError, "eight non-empty cells"):
                    checked.read_row(path)

    def test_dates_must_be_calendar_dates_and_match_the_filename(self):
        for day in ("today", "20261113", "2026-02-30", "2025-02-29", "0000-11-13", "2026-11-13 utc"):
            with self.subTest(day=day):
                path = self.add(text=row(day=day))
                with self.assertRaises(ValueError):
                    checked.read_row(path)
        path = self.add(text=row(day="2026-11-12"))
        with self.assertRaisesRegex(ValueError, "filename"):
            checked.read_row(path)
        for day in ("2024-02-29", "2026-11-13 UTC"):
            with self.subTest(day=day):
                path = self.add(day[:10] + "-test-alex-run01.md", row(day=day))
                self.assertEqual(checked.read_row(path)[0], row(day=day).removesuffix("\n"))

    def test_filename_requires_three_or_more_lowercase_parts_after_date(self):
        for name in ("missing-date.md", "2026-11-13-x.md", "2026-11-13-test-run01.md",
                     "2026-11-13-TEST-alex-run01.md", "2026-11-13-test-alex_run01.md",
                     "2026-11-13-test--alex-run01.md", "2026-11-13-test-alex-run01.extra.md"):
            with self.subTest(name=name):
                path = self.add(name)
                with self.assertRaisesRegex(ValueError, "filename"):
                    checked.read_row(path)
        for name in (NAME, "2026-11-13-multi-part-topic-alex-run01.md"):
            with self.subTest(name=name):
                self.assertEqual(checked.read_row(self.add(name))[0], row().removesuffix("\n"))

    def test_invalid_utf8_is_not_replaced_with_silent_corruption(self):
        path = self.add()
        path.write_bytes((TABLE + row()).encode("utf-8") + b"\xff")
        with self.assertRaises(UnicodeError):
            checked.read_row(path)

    def test_source_requires_one_standalone_marker_before_the_frozen_section(self):
        source = self.source()
        malformed = (source.replace(checked.MARKER, ""), source + checked.MARKER,
                     source.replace(checked.MARKER, "inline " + checked.MARKER),
                     source.replace(checked.MARKER, checked.MARKER + " inline"),
                     source.replace("Earlier runs (frozen)", "Other runs"))
        for text in malformed:
            with self.subTest(text=text):
                self.source_path.write_bytes(text.encode("utf-8"))
                with self.assertRaises(ValueError):
                    checked.render(self.root)

    def test_source_requires_a_frozen_table_and_one_limits_section(self):
        source = self.source()
        malformed = (source.replace(checked.HEADER, checked.HEADER.replace("Build", "Other")),
                     source.replace(checked.SEPARATOR, "|---|"), source.replace(self.old, ""),
                     source.replace(self.old, self.old.replace("CPU", "")),
                     source.replace(self.old, self.old + TABLE),
                     source.replace(self.old, self.old + checked.SEPARATOR + "\n"), source.replace(LIMITS, ""),
                     source + LIMITS, source.replace("\n\n## Limits", "\n## Limits"))
        for text in malformed:
            with self.subTest(text=text):
                self.source_path.write_bytes(text.encode("utf-8"))
                with self.assertRaises(ValueError):
                    checked.render(self.root)

    def test_cli_stdout_works_with_stringio(self):
        code, output, errors = self.invoke([])
        self.assertEqual((code, errors), (0, ""))
        self.assertEqual(output.getvalue(), checked.render(self.root))

    def test_cli_ascii_pipe_emits_the_arrow_containing_source_as_utf8(self):
        # A redirected TextIOWrapper stand-in, not a native Windows runtime test.
        with io.TextIOWrapper(io.BytesIO(), encoding="ascii") as pipe:
            code, _, errors = self.invoke([], stream=pipe)
            pipe.flush()
            self.assertEqual((code, errors), (0, ""))
            self.assertEqual(pipe.buffer.getvalue(), checked.render(self.root).encode("utf-8"))

    def test_cli_output_file_is_utf8_and_stdout_stays_empty(self):
        destination = self.root / "views/résultats.md"
        code, output, errors = self.invoke(["--output", str(destination)])
        self.assertEqual((code, output.getvalue(), errors), (0, "", ""))
        self.assertEqual(destination.read_bytes(), checked.render(self.root).encode("utf-8"))

    def test_cli_rejects_invalid_data_without_emitting_a_partial_view(self):
        self.add(text=row("as above"))
        destination = self.root / "absent/view.md"
        for args in ([], ["--output", str(destination)]):
            with self.subTest(args=args):
                code, output, errors = self.invoke(args)
                self.assertEqual((code, output.getvalue()), (1, ""))
                self.assertIn("stand alone", errors)
                self.assertFalse(destination.exists())


class Repository(unittest.TestCase):
    def test_repository_fragments_validate_and_source_suffix_is_preserved(self):
        source = (ROOT / "docs/tested-configurations.md").read_bytes().decode("utf-8")
        self.assertEqual(source.count(checked.MARKER), 1)
        prefix, suffix = source.split(checked.MARKER)
        view = checked.render(ROOT)
        self.assertTrue(view.startswith(prefix + "## New runs\n\n"))
        self.assertTrue(view.endswith(suffix))
        self.assertNotIn(checked.MARKER, view)
        self.assertEqual(len(re.findall(r"^## Limits(?:\s|$)", suffix, re.M)), 1)
        self.assertEqual(len(re.findall(r"^## Limits(?:\s|$)", view, re.M)), 1)
        self.assertTrue((ROOT / "docs/tested/README.md").is_file())


if __name__ == "__main__":
    unittest.main()
