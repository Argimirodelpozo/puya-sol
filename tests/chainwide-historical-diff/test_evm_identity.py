"""Creator identity is byte-significant, not merely a post-replay label."""
import json
import subprocess

from Crypto.Hash import keccak

from chd_common import EVM_PY, HERE, dump_json, load_json


def test_evm_constructor_and_owner_calls_preserve_historical_creator(tmp_path):
    creator = "0x4265de963cdd60629d03fee2cd3285e6d5ff6015"
    source = """pragma solidity ^0.8.20;
contract Identity {
    address public owner = msg.sender;
    uint256 public count;
    mapping(address => uint256) public balanceOf;
    constructor() { balanceOf[0x4265de963cdd60629d03FEE2cd3285e6d5ff6015] = 1; }
    function bump() external { require(msg.sender == owner); ++count; }
    function literalMatchesOwner() external view returns (bool) {
        return owner == 0x4265de963cdd60629d03FEE2cd3285e6d5ff6015;
    }
} """
    (tmp_path / "prepared.sol").write_text(source)
    abi = [{"type": "constructor", "inputs": [], "stateMutability": "nonpayable"}]
    for name, inputs, outputs, state in (
        ("bump", [], [], "nonpayable"),
        ("owner", [], ["address"], "view"),
        ("count", [], ["uint256"], "view"),
        ("balanceOf", ["address"], ["uint256"], "view"),
        ("literalMatchesOwner", [], ["bool"], "view"),
    ):
        abi.append({"type": "function", "name": name, "stateMutability": state,
                    "inputs": [{"name": f"a{i}", "type": t} for i, t in enumerate(inputs)],
                    "outputs": [{"name": "", "type": t} for t in outputs]})
    selector = keccak.new(digest_bits=256, data=b"bump()").digest()[:4].hex()
    txns = [{"hash": "0x" + f"{i+1:064x}", "from": sender,
             "ts": 1_000_000 + i, "input": "0x" + selector, "value": 0,
             "hist_ok": i == 0}
            for i, sender in enumerate((creator, "0x" + "12" * 20))]
    dump_json(tmp_path / "case.json", {"name": "Identity", "abi": abi,
        "compiler_version": "0.8.26", "ctor_args_hex": "", "txns": txns,
        "creation": {"creator": creator, "ts": 999_000}, "address": "0x" + "34" * 20})
    result = subprocess.run([str(EVM_PY), str(HERE / "evm_leg.py"), str(tmp_path),
                             json.dumps({"snapshot_every": 1})],
                            capture_output=True, text=True, timeout=90)
    assert result.returncode == 0, result.stdout + result.stderr
    replay = load_json(tmp_path / "evm_results.json")
    assert replay["results"]["0"]["ok"] is True
    assert replay["results"]["1"]["ok"] is False
    assert replay["snapshots"]["0"]["literalMatchesOwner()"] == [True]
    assert replay["storage"]["scalars"]["owner"] == "«C»"
    assert replay["storage"]["scalars"]["count"] == 1
    assert replay["storage"]["maps"]["balanceOf"]["«C»"] == 1
