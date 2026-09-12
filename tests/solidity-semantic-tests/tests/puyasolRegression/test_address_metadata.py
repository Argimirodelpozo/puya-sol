"""AVM metadata adaptations; portable constructor predicates checked against solc."""

import base64

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int
from framework.compile import CompileError, compile_sol


@pytest.fixture(params=[False, True], ids=["legacy", "via-ir"])
def via_ir(request):
    return request.param


@pytest.fixture(params=["arc4", "evm"])
def profile(request):
    return request.param


def call(harness, app, profile, signature, arguments, returns):
    if profile == "evm":
        inputs = signature.split("(", 1)[1][:-1]
        types = inputs.split(",") if inputs else []
        arguments = ["0x" + arg[-20:].hex() if kind == "address" else arg
                     for kind, arg in zip(types, arguments, strict=True)]
        selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
        result = harness.call_raw(app, selector, extra_args=(encode(types, arguments),),
                                  extra_fee=40_000, budget_pool=8)
        return decode(returns, result.logs[-1][4:])
    result = harness.call(app, signature, *arguments, extra_fee=40_000)
    values = (result.abi_return,) if len(returns) == 1 else result.abi_return
    return tuple(bytes(value) if kind.startswith("bytes") else as_int(value)
                 for kind, value in zip(returns, values, strict=True))


def test_address_metadata(harness, via_ir, profile, tmp_path):
    artifacts = harness.compile("puyasolRegression/contracts/address_metadata.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile])
    target = harness.deploy(artifacts, "MetadataTarget")
    target_address = target.app_id.to_bytes(32, "big")
    app = harness.deploy(artifacts, "AddressMetadata", ctor_args=[target_address],
                         fund_wei=30_000_000, postinit_budget_pool=8)
    check = lambda sig, args, ret: call(harness, app, profile, sig, args, ret)
    assert check("constructorChecks()", [], ["bool"]) == (True,)
    assert check("touches()", [], ["uint256"]) == (4,)
    code, capacity, touches = check("read(address)", [target_address],
                                    ["bytes", "uint256", "uint256"])
    actual_program = base64.b64decode(
        harness.localnet.algod.application_info(target.app_id)["params"]["approval-program"])
    assert code == actual_program
    assert capacity >= len(code) > 0
    assert touches == 2
    # Zero cannot alias self, and matching low 64 bits do not establish an app
    # address when the prefix is nonzero (still within the EVM's 160 bits).
    for address in (bytes(32), (target.app_id | 1 << 80).to_bytes(32, "big")):
        assert check("read(address)", [address], ["bytes", "uint256", "uint256"]) == (b"", 0, 2)
    alias_size, direct_size = check("selfSize()", [], ["uint256", "uint256"])
    assert alias_size == direct_size > 0
    assert check("gated(bool,address)", [False, target_address], ["uint256", "uint256"]) == (9, 0)
    assert check("gated(bool,address)", [True, target_address], ["uint256", "uint256"]) == (capacity, 1)
    assert check("fieldLength(bytes)", [b"abcde"], ["uint256"]) == (5,)

    hashed = harness.deploy(artifacts, "MetadataHash")
    assert call(harness, hashed, profile, "constructorHash()", [], ["bool"]) == (True,)
    program = base64.b64decode(
        harness.localnet.algod.application_info(hashed.app_id)["params"]["approval-program"])
    assert call(harness, hashed, profile, "hashes()", [], ["bytes32"] * 3) == (
        bytes(32), keccak.new(digest_bits=256, data=b"").digest(),
        keccak.new(digest_bits=256, data=program).digest())

    # A numeric literal can denote a deployed app; it is not proof of no code.
    source = tmp_path / "literal.sol"
    source.write_text("pragma solidity ^0.8.20; contract Literal { "
                      "function f() external view returns (bytes memory, uint256) { "
                      f"bytes memory code = address({target.app_id}).code; "
                      f"return (code, address({target.app_id}).code.length); "
                      "} }")
    literal = harness.compile_and_deploy(source, via_yul_behavior=via_ir,
                                         extra_args=["--contract-abi", profile])
    assert call(harness, literal, profile, "f()", [], ["bytes", "uint256"]) == (actual_program, capacity)


@pytest.mark.parametrize("receiver", ["address(11)", "address(0x1234)", "address(100 + 11)", "a"])
def test_unknown_codehash_rejected(tmp_path, receiver, via_ir):
    source = tmp_path / "unknown_hash.sol"
    source.write_text("pragma solidity ^0.8.20; contract C { "
                      "function f(address a) external view returns (bytes32) { return "
                      + receiver + ".codehash; } }")
    with pytest.raises(CompileError, match="non-`this` address is not supported"):
        compile_sol(source, tmp_path / "out", via_yul_behavior=via_ir)


def test_metadata_warnings(tmp_path):
    source = tmp_path / "warnings.sol"
    source.write_text("pragma solidity ^0.8.20; contract C { "
                      "function f() external view returns (bytes memory, uint256, bytes32) { "
                      "return (address(this).code, address(this).code.length, address(this).codehash); } }")
    artifacts = compile_sol(source, tmp_path / "out")
    assert artifacts.by_contract
    # Check the frontend directly: compile_sol may use the content-addressed
    # backend cache, whose logs are not a stable diagnostics interface.
    import subprocess
    from framework.paths import COMPILER
    result = subprocess.run([str(COMPILER), "--source", str(source), "--no-puya",
                             "--output-dir", str(tmp_path / "diagnostics")],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    diagnostics = result.stdout + result.stderr
    assert "not exact EVM code size" in diagnostics
    assert "not EVM bytecode" in diagnostics
    assert "not EVM bytecode identity" in diagnostics
