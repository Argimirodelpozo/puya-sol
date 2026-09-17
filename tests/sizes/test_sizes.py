"""Fast offline checks: python3 -m unittest discover -s tests/sizes -v."""
from __future__ import annotations

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import diff_commits
import sizes


class SizeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="size-tests-")
        self.root = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def row(self, name="C", value="100", source="one.sol", clear="4"):
        return sizes.Row(source, name, value, "10" if value.isdigit() else value,
                         clear if value.isdigit() else value)

    def table(self, *rows):
        return {(r.source, r.name): r for r in rows}

    def test_table_roundtrip_and_sorted_output(self):
        rows = [self.row("B"), self.row("A"), self.row("*", "EMPTY", "abstract.sol")]
        text = sizes.format_rows("test", rows)
        self.assertEqual(self.table(*rows), sizes.parse_rows(text))
        self.assertEqual(text, sizes.format_rows("test", list(reversed(rows))))

    def test_invalid_and_duplicate_rows_are_rejected(self):
        for text in ("s C 1 2 ERR", "s C -1 2 3", "s * bogus bogus bogus", "s C 1 2 3\ns C 1 2 3"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                sizes.parse_rows(text)

    def test_static_instruction_count(self):
        self.assertEqual(sizes.count_ops('#pragma version 11\n// comment\nlabel:\n\nint 1 //x\nreturn\n'), 2)
        self.assertEqual(sizes.count_ops('byte "https://example"\n'), 1)

    def test_compile_failure_is_not_a_size_reduction(self):
        delta = sizes.diff_tables(self.table(self.row()), self.table(self.row("*", "ERR")))
        self.assertEqual(delta["comparable"], 0)
        self.assertEqual(delta["bytes_total_old"], delta["bytes_total_new"])
        self.assertEqual(len(delta["regressions"]), 1)
        self.assertEqual(delta["added"], [])
        self.assertEqual(delta["removed"], [])
        self.assertEqual(delta["shrank"], 0)

    def test_empty_success_becoming_failure_is_a_compile_regression(self):
        delta = sizes.diff_tables(self.table(self.row("*", "EMPTY")), self.table(self.row("*", "ERR")))
        self.assertEqual(delta["comparable"], 0)
        self.assertEqual(len(delta["regressions"]), 1)

    def test_recovery_does_not_inflate_comparable_totals(self):
        delta = sizes.diff_tables(self.table(self.row("*", "ERR")), self.table(self.row()))
        self.assertEqual(delta["comparable"], 0)
        self.assertEqual(delta["regressions"], [])
        self.assertEqual(delta["status_changes"][0]["after"], "OK")

    def test_new_failed_source_is_visible(self):
        delta = sizes.diff_tables({}, self.table(self.row("*", "ERR")))
        self.assertEqual(delta["units_added"], [{"source": "one.sol", "status": "ERR"}])
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            sizes.print_diff(delta, 0)
        self.assertIn("one.sol: ERR", output.getvalue())

    def test_added_and_removed_are_separate_from_growth(self):
        delta = sizes.diff_tables(self.table(self.row("old"), self.row("same")),
                                  self.table(self.row("new", "2000"), self.row("same", "110")))
        self.assertEqual(delta["comparable"], 1)
        self.assertEqual(delta["bytes_total_new"] - delta["bytes_total_old"], 10)
        self.assertEqual(delta["added"][0]["name"], "new")
        self.assertEqual(delta["removed"][0]["name"], "old")

    def test_clear_only_changes_are_reported(self):
        delta = sizes.diff_tables(self.table(self.row()), self.table(self.row(clear="8")))
        self.assertEqual(delta["changed"][0]["clear_delta"], 4)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            sizes.print_diff(delta, 0)
        self.assertIn("clear +4", output.getvalue())

    def test_source_and_flags_invalidate_inputs_identity(self):
        source = self.root / "fixture.sol"
        dependency = self.root / "import.sol"
        source.write_text("contract C {}")
        dependency.write_text("library L {}")
        unit = sizes.Unit("fixture.sol", ("--source", str(source)), (source, dependency))
        with patch.object(sizes, "ROOT", self.root):
            before = sizes.inputs_identity([unit])
            dependency.write_text("library Changed {}")
            self.assertNotEqual(before, sizes.inputs_identity([unit]))
            dependency.write_text("library L {}")
            self.assertEqual(before, sizes.inputs_identity([unit]))
            slot = sizes.Unit(unit.id, (*unit.args, "--evm-storage-layout"), unit.inputs)
            self.assertNotEqual(before, sizes.inputs_identity([slot]))
            self.assertNotEqual(before, sizes.inputs_identity([unit, sizes.Unit("another", (), (source,))]))

    def test_metadata_changes_invalidate_inputs_identity(self):
        meta = self.root / "case.json"
        meta.write_text("{}")
        unit = sizes.Unit("case", (), (meta,))
        before = sizes.inputs_identity([unit])
        meta.write_text('{"multifile": {}}')
        self.assertNotEqual(before, sizes.inputs_identity([unit]))

    def test_chainwide_default_excludes_local_downloads(self):
        cases = self.root / "tests/chainwide-historical-diff/cases"
        for name in ("eul", "downloaded"):
            directory = cases / name
            directory.mkdir(parents=True)
            (directory / "case.json").write_text("{}")
            (directory / "prepared.sol").write_text("contract C {}")
        with patch.object(sizes, "ROOT", self.root), patch.object(sizes, "CHAINWIDE_DIR", cases), \
                patch.object(sizes, "git", return_value="tests/chainwide-historical-diff/cases/eul/case.json"):
            tracked = sizes.chainwide_units()
            self.assertEqual([u.id for u in tracked], ["eul"])
            self.assertIn("--evm-storage-layout", tracked[0].args)
            self.assertEqual(len(sizes.chainwide_units(True)), 2)

    def test_compiler_provenance_checks_binary_source_and_stdlib(self):
        binary = self.root / "puya-sol"
        stdlib = self.root / "share/puya-sol/libs/AVM.sol"
        stdlib.parent.mkdir(parents=True)
        stdlib.write_text("stdlib")
        binary.write_text("compiler")
        manifest = self.root / "puya-sol-build-manifest.txt"
        values = dict(root_commit="rev", root_tree_state="clean", solidity_commit="solc",
                      source_sha256="inputs", puya_sol_sha256=sizes.sha256(binary),
                      avm_stdlib_sha256=sizes.sha256(stdlib))
        manifest.write_text("".join(f"{k}={v}\n" for k, v in values.items()))
        with patch.object(sizes, "source_identity", return_value="inputs"), \
                patch.object(sizes, "git", side_effect=["solc", ""]):
            self.assertEqual(sizes.compiler_identity(binary, self.root), values)
        with patch.object(sizes, "source_identity", return_value="changed"), self.assertRaisesRegex(ValueError, "stale"):
            sizes.compiler_identity(binary, self.root)
        stdlib.write_text("changed")
        with self.assertRaisesRegex(ValueError, "standard library"):
            sizes.compiler_identity(binary, None)
        stdlib.write_text("stdlib")
        binary.write_text("other compiler")
        with self.assertRaisesRegex(ValueError, "binary"):
            sizes.compiler_identity(binary, None)

    def test_empty_incomplete_and_failed_compiler_outputs(self):
        unit = sizes.Unit("one.sol", ())
        def run(artifacts, code=0):
            def fake(cmd, timeout):
                out = Path(cmd[cmd.index("--output-dir") + 1])
                for name, content in artifacts.items():
                    (out / name).write_bytes(content)
                return code, "diagnostic"
            with patch.object(sizes, "run_compiler", side_effect=fake):
                return sizes.compile_unit(unit, Path("compiler"), Path("puya"), self.root, 1, None, None)[0]
        good = {"C.approval.bin": b"abc", "C.clear.bin": b"x", "C.approval.teal": b"int 1\nreturn\n"}
        self.assertEqual(run(good), [sizes.Row("one.sol", "C", "3", "2", "1")])
        self.assertEqual(run({})[0].approval, "EMPTY")
        self.assertEqual(run({"C.approval.bin": b"abc"})[0].approval, "ARTIFACT_ERR")
        self.assertEqual(run(good, 1)[0].approval, "ERR")
        self.assertEqual(run(good, None)[0].approval, "TIMEOUT")
        self.assertEqual(list(self.root.iterdir()), [])

    def test_exception_still_removes_raw_output(self):
        with patch.object(sizes, "run_compiler", side_effect=OSError("broken")), self.assertRaises(OSError):
            sizes.compile_unit(sizes.Unit("one", ()), Path("compiler"), Path("puya"), self.root, 1, None, None)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_timeout_captures_text_and_terminates_process_group(self):
        code, output = sizes.run_compiler(
            [sys.executable, "-u", "-c", "import time; print('started'); time.sleep(30)"], 0.2)
        self.assertIsNone(code)
        self.assertIn("started", output)

    def test_cache_hit_invalidation_corruption_and_retention(self):
        source = self.root / "one.sol"
        source.write_text("contract C {}")
        unit = sizes.Unit("one.sol", (), (source,))
        cache = self.root / "cache"
        identity = {"binary": "one"}
        backend = {"version": "puya test"}
        result = ([self.row()], 0.0)
        with patch.dict(sizes.CORPORA, {"test": lambda: [unit]}), \
                patch.object(sizes, "compiler_identity", return_value=identity), \
                patch.object(sizes, "puya_signature", return_value=backend), \
                patch.object(sizes, "compile_unit", return_value=result) as compile_, \
                contextlib.redirect_stderr(io.StringIO()):
            def census(**kwargs):
                return sizes.census("test", Path("compiler"), Path("puya"), cache_dir=cache, **kwargs)
            census()
            census()
            self.assertEqual(compile_.call_count, 1)
            source.write_text("contract D {}")
            census()
            self.assertEqual(compile_.call_count, 2)
            identity["binary"] = "other"
            census()
            self.assertEqual(compile_.call_count, 3)
            backend["version"] = "changed"
            census()
            self.assertEqual(compile_.call_count, 4)
            for path in cache.glob("*.json"):
                path.write_text("{}")
            census()
            self.assertEqual(compile_.call_count, 5)
            census(teal_dir=self.root / "teal")
            self.assertEqual(compile_.call_count, 6)  # raw TEAL is never in the cache
        unrelated = cache / "unrelated.json"
        unrelated.write_text("{}")
        sizes.prune_cache(cache, keep=1)
        self.assertTrue(unrelated.exists())
        self.assertEqual(len(list(cache.glob("v3-*.json"))), 1)

    def test_input_mutation_during_census_is_rejected(self):
        source = self.root / "one.sol"
        source.write_text("before")
        def compile_(*args):
            source.write_text("after")
            return [self.row()], 0.0
        with patch.dict(sizes.CORPORA, {"test": lambda: [sizes.Unit("one", (), (source,))]}), \
                patch.object(sizes, "compiler_identity", return_value={}), \
                patch.object(sizes, "puya_signature", return_value={}), \
                patch.object(sizes, "compile_unit", side_effect=compile_), \
                self.assertRaisesRegex(ValueError, "changed during"):
            sizes.census("test", Path("compiler"), Path("puya"), cache_dir=None)

    def test_incomplete_run_is_not_cached(self):
        unit = sizes.Unit("one", ())
        with patch.dict(sizes.CORPORA, {"test": lambda: [unit]}), \
                patch.object(sizes, "compiler_identity", return_value={}), \
                patch.object(sizes, "puya_signature", return_value={}), \
                patch.object(sizes, "compile_unit", return_value=([self.row("*", "TIMEOUT")], 0)):
            cache = self.root / "cache"
            sizes.census("test", Path("compiler"), Path("puya"), cache_dir=cache)
            self.assertFalse(cache.exists())

    def test_partial_and_local_corpora_cannot_overwrite_baselines(self):
        for arguments in (["--write", "--only", "one"], ["--check", "--corpus", "chainwide-local"]):
            with patch.object(sys, "argv", ["sizes.py", *arguments]), \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                sizes.main()
            self.assertEqual(error.exception.code, 2)

    def test_filtered_table_has_a_valid_regeneration_command(self):
        metadata = {"inputs": "hash", "backend": {"version": "test"}, "only": "foo bar"}
        text = sizes.format_rows("regression", [], metadata)
        self.assertIn("--only 'foo bar' --out /tmp/sizes.txt", text)
        self.assertNotIn("--write", text)
        self.assertIn("--out", sizes.format_rows("chainwide-local", []))

    def test_cli_rejects_negative_limits(self):
        with patch.object(sys, "argv", ["sizes.py", "--limit", "-1"]), \
                contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            sizes.main()
        self.assertEqual(error.exception.code, 2)

    def test_semantic_run_blocks_size_run(self):
        with patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 0)), \
                self.assertRaisesRegex(ValueError, "semantic suite"):
            sizes.ensure_idle()

    def test_size_runs_cannot_overlap(self):
        with patch.object(sizes, "ROOT", self.root), patch.object(sizes, "ensure_idle"):
            with sizes.run_lock():
                with self.assertRaisesRegex(ValueError, "another size run"):
                    with sizes.run_lock():
                        self.fail("second size run acquired the lock")
            with sizes.run_lock():
                pass  # releasing the first run allows the next one

    def test_compiler_retention_only_deletes_owned_snapshots(self):
        for i in range(3):
            path = self.root / ("v1-" + str(i) * 64)
            path.mkdir()
            (path / ".sizes-owned").touch()
            os.utime(path, (i + 10, i + 10))
        other = self.root / "v1-not-a-cache-key"
        other.mkdir()
        (other / ".sizes-owned").touch()
        diff_commits.prune_compilers(self.root, 2)
        self.assertTrue(other.exists())
        self.assertFalse((self.root / ("v1-" + "0" * 64)).exists())
        self.assertTrue((self.root / ("v1-" + "2" * 64)).exists())

    def test_interrupted_build_does_not_allocate_another_worktree(self):
        stale = self.root / "build/sizes/size-build-interrupted"
        stale.mkdir(parents=True)
        with patch.object(diff_commits, "ROOT", self.root), \
                patch.object(diff_commits, "COMPILERS", self.root / "build/sizes/compilers"), \
                patch.object(diff_commits, "cmake_settings", return_value=(None, [])), \
                patch.object(sizes, "ensure_idle"), \
                self.assertRaisesRegex(ValueError, "interrupted size build"):
            diff_commits.build_commit("revision", 2)
        self.assertTrue(stale.exists())

    def test_teal_diff_writes_only_changed_programs(self):
        old, new = self.root / "old", self.root / "new"
        for path, text in ((old, "int 1\n"), (new, "int 2\n")):
            (path / "one.sol").mkdir(parents=True)
            (path / "one.sol/C.approval.teal").write_text(text)
        delta = {"changed": [{"source": "one.sol", "contract": "C"}]}
        out = self.root / "diff"
        self.assertEqual(diff_commits.write_teal_diffs(delta, old, new, out), 1)
        self.assertIn("-int 1\n+int 2", (out / "one.sol/C.approval.diff").read_text())


if __name__ == "__main__":
    unittest.main()
