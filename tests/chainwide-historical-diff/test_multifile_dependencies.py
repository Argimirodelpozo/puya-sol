"""Source-tree parity for real contract-to-contract replay dependencies."""
import shutil
from types import SimpleNamespace

import pytest

import fetch
from avm_leg import compile_case_contract
from chd_common import load_json
from oracle_case import OracleLane, creation_app_args, inner_application_calls


@pytest.fixture
def verified():
    return {
        "name": "Verifier", "compiler_version": "0.8.20", "abi": [],
        "file_path": "/src/Verifier.sol",
        "source_code": 'pragma solidity ^0.8.20; import "@lib/Value.sol"; contract Verifier {}',
        "additional_sources": [{
            "file_path": "/lib/Value.sol",
            "source_code": "pragma solidity ^0.8.20; library Value {}",
        }],
        "compiler_settings": {"remappings": [":@lib/=lib/"]},
    }


def test_fetch_multifile_dependency_preserves_tree(tmp_path, monkeypatch, verified):
    monkeypatch.setattr(fetch, "http_json", lambda _url: verified)
    dep = fetch.fetch_dep("example.test", "0x" + "12" * 20, tmp_path, 1, set())
    assert dep["multifile"] == {
        "main": "src/Verifier.sol",
        "files": ["lib/Value.sol", "src/Verifier.sol"],
        "remappings": ["@lib/=lib/"],
    }
    assert load_json(tmp_path / "case.json") == dep
    assert (tmp_path / "src/src/Verifier.sol").read_text() == verified["source_code"]
    assert (tmp_path / "src/lib/Value.sol").read_text() == verified["additional_sources"][0]["source_code"]


@pytest.mark.parametrize("path", ["../outside.sol", "/../outside.sol", "."])
def test_verified_source_paths_stay_inside_case(tmp_path, verified, path):
    verified["additional_sources"][0]["file_path"] = path
    with pytest.raises(ValueError, match="unsafe verified source path"):
        fetch.materialize_sources(tmp_path, verified)
    assert not list(tmp_path.iterdir())


def test_duplicate_source_path_is_rejected(tmp_path, verified):
    verified["additional_sources"][0]["file_path"] = verified["file_path"]
    with pytest.raises(ValueError, match="duplicate verified source path"):
        fetch.materialize_sources(tmp_path, verified)


def test_dependency_compile_uses_disposable_copy(tmp_path, verified):
    manifest = fetch.materialize_sources(tmp_path, verified)
    flags = ["--contract-abi", "evm"]

    def compile_copy(source, **kwargs):
        root = kwargs["extra_import_dir"]
        assert root != tmp_path / "src"
        assert source == root / manifest["main"]
        assert kwargs["extra_sources"] == [root / rel for rel in manifest["files"]]
        assert kwargs["extra_remappings"] == ["@lib/=lib/"]
        assert kwargs["extra_args"] == flags
        assert source.read_text() == verified["source_code"]
        shutil.rmtree(root)  # The real compile helper owns its temporary import tree.
        return "artifacts"

    assert compile_case_contract(SimpleNamespace(compile=compile_copy), tmp_path,
                                 {"multifile": manifest}, flags) == "artifacts"
    assert (tmp_path / "src/src/Verifier.sol").exists()


def test_single_file_dependency_keeps_existing_compile_path(tmp_path):
    def compile_single(source, **kwargs):
        assert source == tmp_path / "prepared.sol"
        assert kwargs == {"extra_args": []}
        return "artifacts"

    assert compile_case_contract(SimpleNamespace(compile=compile_single),
                                 tmp_path, {}, []) == "artifacts"


def test_inner_call_counts_include_nested_dependencies_and_helpers():
    txns = [
        {"u64": {"ApplicationID": 9002}, "inner_txns": [
            {"u64": {"ApplicationID": 9003}},
            {"u64": {"ApplicationID": 9002}},
        ]},
        {"u64": {"ApplicationID": 8002}},
        {"u64": {"ApplicationID": 0, "OnCompletion": 5}},
        {"u64": {"Amount": 1}},
    ]
    assert inner_application_calls(txns) == {"9002": 2, "9003": 1, "8002": 1}
    assert inner_application_calls([]) == {}


@pytest.mark.parametrize("requested,indexed", [(True, False), (False, True)])
def test_internal_trace_flag_is_not_replaced_by_index_results(
        tmp_path, monkeypatch, verified, requested, indexed):
    address = "0x" + "12" * 20
    creator = "0x" + "34" * 20
    txn = {"hash": "0xcall", "from": creator, "to": address,
           "input": "0x12345678", "timeStamp": "200", "blockNumber": "2",
           "transactionIndex": "0", "isError": "0", "txreceipt_status": "1"}

    def http(url):
        if "/smart-contracts/" in url:
            return verified
        if "action=txlistinternal" in url:
            return {"result": [{**txn, "hash": "0xinternal"}] if indexed else []}
        if "action=txlist&" in url:
            return {"result": [{**txn, "to": "", "hash": "0xcreate",
                                "timeStamp": "100", "blockNumber": "1"}, txn]}
        raise AssertionError(f"unexpected request: {url}")

    traced = []

    def trace(*args, **kwargs):
        traced.append((args, kwargs))
        return []

    monkeypatch.setattr(fetch, "CASES", tmp_path)
    monkeypatch.setattr(fetch, "http_json", http)
    monkeypatch.setattr(fetch, "fetch_internal_calls", trace)
    monkeypatch.setattr(fetch, "harvest_callees", lambda *args, **kwargs: None)
    case = fetch.fetch_case("example.test", address, "sample", internal=requested)
    assert bool(traced) is requested
    assert ("fetch_coverage" in case) is requested


@pytest.mark.parametrize("deferred", [True, False])
def test_creation_resources_follow_compiled_constructor_mode(deferred):
    requests = []

    def run(request):
        requests.append(request)
        return {"result": "ACCEPT"}

    state = SimpleNamespace(
        request=lambda *args, **kwargs: {"foreign_apps": [9002], **kwargs},
        carry=lambda _response: None,
        register_application=lambda _app: None,
    )
    lane = SimpleNamespace(
        state=state, oracle=SimpleNamespace(run=run), creator="00" * 32,
        approval="int 1", clear="int 1", approval_bin=b"", clear_bin=b"",
        write_budget_refs=0, round=1000, dep_apps=[9002, 9003],
        global_uints=16, global_bytes=16, extra_pages=0,
        _app_fields=lambda: {},
    )
    OracleLane.create(lane, [], 123, deferred_constructor=deferred)
    assert requests[0].get("foreign_apps", []) == ([] if deferred else [9002, 9003])
    assert lane.round == 1001


@pytest.mark.parametrize("deferred", [True, False])
def test_constructor_wire_encoding_follows_solc_abi_and_compiled_lifecycle(deferred):
    from algosdk import encoding
    from eth_abi import encode

    address = bytes(24) + (9003).to_bytes(8, "big")
    key = bytes.fromhex("42" * 32)
    abi = [{"type": "constructor", "inputs": [{"type": "address"}, {"type": "bytes32"}]}]
    methods = [SimpleNamespace(name="__postInit", args=[
        SimpleNamespace(type="address"), SimpleNamespace(type="byte[32]")])] if deferred else []
    args = creation_app_args(SimpleNamespace(methods=methods), {},
                             [encoding.encode_address(address), key], evm_abi=abi)
    assert args == ([address.hex(), key.hex()] if deferred else
                    [encode(["address", "bytes32"], [address[-20:], key]).hex()])


def test_evm_constructor_arity_must_match_solc():
    with pytest.raises(ValueError, match="constructor argument count mismatch"):
        creation_app_args(SimpleNamespace(methods=[]), {}, [1], evm_abi=[])
