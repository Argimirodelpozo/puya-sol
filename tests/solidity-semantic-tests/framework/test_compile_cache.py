"""Focused unit tests for the semantic harness's compiler cache."""

from __future__ import annotations

import subprocess

import pytest

from . import compile as compile_module


def _git(repo, *args: str) -> None:
    subprocess.run(
        ["git", *args], cwd=repo, check=True, capture_output=True, text=True
    )


@pytest.mark.parametrize("change", ["delete", "rename"])
def test_puya_backend_signature_tracks_path_only_changes(tmp_path, monkeypatch, change):
    puya_dir = tmp_path / "puya"
    backend_dir = puya_dir / "src"
    backend_dir.mkdir(parents=True)
    backend_file = backend_dir / "backend.py"
    backend_file.write_text("VALUE = 1\n")

    _git(puya_dir, "init", "-q")
    _git(puya_dir, "config", "user.email", "test@example.invalid")
    _git(puya_dir, "config", "user.name", "Test")
    _git(puya_dir, "config", "commit.gpgsign", "false")
    _git(puya_dir, "add", "src/backend.py")
    _git(puya_dir, "commit", "-qm", "fixture")

    monkeypatch.setattr(compile_module, "PUYA_BACKEND_SRC", backend_dir)
    clean_signature = compile_module._puya_backend_sig()

    if change == "delete":
        backend_file.unlink()
    else:
        _git(puya_dir, "mv", "src/backend.py", "src/renamed.py")

    assert compile_module._puya_backend_sig() != clean_signature
