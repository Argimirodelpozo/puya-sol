"""Joint ARC-4/slot artifacts must never alias per-contract replay output."""
import json

import pytest

import oracle_cctp_historical as historical
import refresh_cctp_artifacts as refresh


def artifact(directory, contract="Example", *, methods=None, named=False, event="JointEvent"):
    directory.mkdir(parents=True, exist_ok=True)
    spec = {
        "methods": methods if methods is not None else [{"name": "read", "args": []}],
        "state": {"keys": {"global": {"counter": {}}}} if named else {},
        "events": [{"name": event, "args": []}],
    }
    for suffix in historical.ARTIFACT_SUFFIXES:
        path = directory / f"{contract}.{suffix}"
        if suffix == "arc56.json":
            path.write_text(json.dumps(spec))
        elif suffix.endswith(".bin"):
            path.write_bytes(b"\x01")
        else:
            path.write_text(f"// {event}\nint 1\n")
    return spec


def test_missing_joint_directory_never_falls_back_to_legacy(tmp_path):
    artifact(tmp_path / "sample/out_avm")
    with pytest.raises(historical.ArtifactError, match="out_avm_joint.*missing.*refresh_cctp"):
        historical.check_joint_artifact(tmp_path, "sample", "Example")


def test_joint_readers_ignore_conflicting_per_contract_output(tmp_path):
    artifact(tmp_path / "sample/out_avm", methods=[{"name": "__postInit"}], event="Legacy")
    directory = tmp_path / "sample" / historical.JOINT_ARTIFACT_DIR
    expected = artifact(directory)
    checked = historical.check_joint_artifact(tmp_path, "sample", "Example")
    assert checked["artifact"] == str(directory / "Example.arc56.json")
    data = historical.CaseData("sample", tmp_path / "sample", {"contract": "Example"}, {}, {}, {})
    assert data.arc56 == expected
    assert "JointEvent" in historical.read_artifact(directory, "Example")["source"]


@pytest.mark.parametrize("bad_profile", ["evm-abi", "named-storage"])
def test_joint_validator_rejects_wrong_profiles(tmp_path, bad_profile):
    artifact(tmp_path / "sample" / historical.JOINT_ARTIFACT_DIR,
             methods=[{"name": "__postInit"}] if bad_profile == "evm-abi" else None,
             named=bad_profile == "named-storage")
    with pytest.raises(historical.ArtifactError):
        historical.check_joint_artifact(tmp_path, "sample", "Example")


def test_history_loading_does_not_require_compiled_artifacts(tmp_path, monkeypatch):
    monkeypatch.setattr(historical, "CASE_CONFIG", {"sample": {"contract": "Example"}})
    directory = tmp_path / "sample"
    directory.mkdir()
    for name in ("case", "calls", "registry"):
        (directory / f"{name}.json").write_text("{}")
    assert historical.CaseData.load(tmp_path, "sample").case == {}


def test_refresh_writes_only_joint_output(tmp_path, monkeypatch):
    directory = tmp_path / "cctp_transmitter"
    directory.mkdir()
    (directory / "case.json").write_text("{}")
    old = directory / "out_avm"
    artifact(old, event="Untouched")
    compiled = []
    monkeypatch.setattr(refresh, "compile_into", lambda out, source, label: compiled.append(out))
    refresh.refresh("cctp_transmitter", tmp_path, {"STUB_SOURCE": {"tag": "cctp_minter"}})
    assert compiled == [directory / historical.JOINT_ARTIFACT_DIR]
    assert "Untouched" in (old / "Example.approval.teal").read_text()
    assert refresh.JOINT_LANE_ARGS == ["--contract-abi", "arc4", "--evm-storage-layout"]


def test_compatibility_copy_uses_only_joint_output(tmp_path, monkeypatch):
    monkeypatch.setattr(historical, "CASE_CONFIG", {"sample": {"contract": "TokenMinter"}})
    artifact(tmp_path / "sample/out_avm", "TokenMinter", event="Legacy")
    artifact(tmp_path / "sample" / historical.JOINT_ARTIFACT_DIR, "TokenMinter")
    temp, root, patches, _ = historical.build_pre08_compat_artifacts(tmp_path)
    try:
        source = historical.read_artifact(root / "sample" / historical.JOINT_ARTIFACT_DIR, "TokenMinter")
        assert "JointEvent" in source["source"]
        assert patches == {"sample": []}
    finally:
        temp.cleanup()


def test_joint_event_reader_uses_joint_arc56(tmp_path, monkeypatch):
    import cctp_joint_diff

    monkeypatch.setattr(cctp_joint_diff, "CASE_CONFIG", {"sample": {"contract": "Example"}})
    monkeypatch.setattr(cctp_joint_diff, "UPGRADES", [])
    artifact(tmp_path / "sample/out_avm", event="Legacy")
    artifact(tmp_path / "sample" / historical.JOINT_ARTIFACT_DIR)
    assert [name for name, _ in cctp_joint_diff.arc56_event_index(tmp_path).values()] == ["JointEvent"]
