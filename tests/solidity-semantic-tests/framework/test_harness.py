"""Focused unit tests for the semantic-test harness."""

from __future__ import annotations

from . import harness as harness_module


def test_each_compile_gets_an_isolated_output_directory(tmp_path, monkeypatch):
    source = tmp_path / "contract.sol"
    source.write_text("contract C {}\n")
    output = tmp_path / "out"
    seen_output_dirs = []

    def fake_compile_sol(_source, out_dir, **_opts):
        out_dir.mkdir(parents=True)
        (out_dir / "artifact").write_text("generated\n")
        seen_output_dirs.append(out_dir)
        return object()

    monkeypatch.setattr(harness_module, "compile_sol", fake_compile_sol)
    harness = harness_module.Harness(object(), output)

    harness.compile(source)
    harness.compile(source)

    assert seen_output_dirs == [
        output / "compile-0001",
        output / "compile-0002",
    ]

    legacy_artifact = output / "legacy-tracked-artifact"
    legacy_artifact.write_text("preserve me\n")
    harness.cleanup()

    assert legacy_artifact.exists()
    assert all(not compile_dir.exists() for compile_dir in seen_output_dirs)
