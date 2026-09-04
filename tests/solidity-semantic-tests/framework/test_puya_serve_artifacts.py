"""Artifact-integrity tests for the persistent backend path (audit M-03)."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from .puya_serve import _atomic_write_json, finalize_backend_artifacts


def _frontend_artifacts(
    out_dir: Path, *, child: bool = True, awst_value: list | None = None
) -> None:
    awst = _atomic_write_json(out_dir / "awst.json", awst_value or [])
    definitions = (
        {
            "APPROVAL_Child_P0": "0x00",
            "APPROVAL_Child_P1": "0x00",
            "CLEAR_Child": "0x00",
        }
        if child
        else {}
    )
    options = _atomic_write_json(
        out_dir / "options.json",
        {
            "cli_template_definitions": definitions,
        },
    )
    files = []
    for name, role, data in (
        ("awst.json", "frontend-awst", awst),
        ("options.json", "backend-options", options),
    ):
        files.append(
            {
                "path": name,
                "role": role,
                "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )
    _atomic_write_json(
        out_dir / "artifact-manifest.json",
        {
            "schema": "puya-sol/artifact-manifest/v1",
            "phase": "frontend-only",
            "scope": "test",
            "files": files,
        },
    )


def test_finalize_backend_artifacts_enforces_pages_and_hashes(tmp_path):
    _frontend_artifacts(tmp_path)
    (tmp_path / "Child.approval.bin").write_bytes(bytes(range(256)) * 32)
    (tmp_path / "Child.clear.bin").write_bytes(b"c" * 4096)

    assert finalize_backend_artifacts(tmp_path)
    template = json.loads((tmp_path / "deploy.tmpl.json").read_text())
    assert len(template["TMPL_APPROVAL_Child_P0"]) == 8192
    assert len(template["TMPL_APPROVAL_Child_P1"]) == 8192
    assert len(template["TMPL_CLEAR_Child"]) == 8192
    manifest = json.loads((tmp_path / "artifact-manifest.json").read_text())
    assert manifest["phase"] == "backend-complete"
    assert {record["path"] for record in manifest["files"]} == {
        "awst.json",
        "options.json",
        "Child.approval.bin",
        "Child.clear.bin",
        "deploy.tmpl.json",
    }


def test_finalize_backend_artifacts_rejects_missing_or_oversized_input(tmp_path):
    _frontend_artifacts(tmp_path)
    (tmp_path / "Child.approval.bin").write_bytes(b"a")
    assert not finalize_backend_artifacts(tmp_path)
    assert not (tmp_path / "deploy.tmpl.json").exists()

    (tmp_path / "Child.approval.bin").write_bytes(b"a" * 8193)
    (tmp_path / "Child.clear.bin").write_bytes(b"c")
    assert not finalize_backend_artifacts(tmp_path)
    assert not (tmp_path / "deploy.tmpl.json").exists()


def test_finalize_backend_artifacts_verifies_frontend_digest(tmp_path):
    _frontend_artifacts(tmp_path, child=False)
    (tmp_path / "awst.json").write_text("[]\n ")

    assert not finalize_backend_artifacts(tmp_path)
    manifest = json.loads((tmp_path / "artifact-manifest.json").read_text())
    assert manifest["phase"] == "frontend-only"


def test_finalize_backend_artifacts_hashes_all_current_target_outputs(tmp_path):
    _frontend_artifacts(
        tmp_path,
        child=False,
        awst_value=[{"_type": "Contract", "name": "Target"}],
    )
    for suffix, data in {
        ".approval.bin": b"a",
        ".clear.bin": b"c",
        ".approval.teal": b"approval",
        ".clear.teal": b"clear",
        ".arc56.json": b"{}",
        ".000.ssa.ir": b"ir",
    }.items():
        (tmp_path / f"Target{suffix}").write_bytes(data)

    assert finalize_backend_artifacts(tmp_path)
    manifest = json.loads((tmp_path / "artifact-manifest.json").read_text())
    assert {record["path"] for record in manifest["files"]} >= {
        "Target.approval.bin",
        "Target.clear.bin",
        "Target.approval.teal",
        "Target.clear.teal",
        "Target.arc56.json",
        "Target.000.ssa.ir",
    }
