"""Small, scoped metadata tuples must not leak into surrounding Yul lifetimes."""

import base64
import hashlib

import pytest

from framework import as_int


@pytest.mark.parametrize("optimization", [1, 2])
def test_metadata_tuple_beside_memory_and_returndata(harness, optimization):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_extcodesize.sol", "AsmExtcodesize",
        extra_args=["--optimization-level", str(optimization)], postinit_inner_txns=2)
    assert harness.call(app, "contractHasCode()").abi_return is True
    assert harness.call(app, "eoaHasNoCode(address)", harness.localnet.account.address).abi_return is False
    assert harness.call(app, "agreesWithDotCode()").abi_return is True
    assert as_int(harness.call(app, "twoReads()").abi_return) > 0
    assert harness.call(app, "nestedAfterCall()", extra_fee=10_000).abi_return is True


@pytest.mark.parametrize("optimization", [1, 2])
def test_metadata_queries_oversized_program_without_materializing(harness, tmp_path, optimization):
    # Independent pseudo-random uint256 constants keep program bytes above the
    # stack-value limit without creating any oversized source byte value.
    branches = " ".join(f"if (x == {i}) return 0x{hashlib.sha256(str(i).encode()).hexdigest()};"
                        for i in range(110))
    source = tmp_path / "large.sol"
    source.write_text("// SPDX-License-Identifier: MIT\npragma solidity ^0.8.20;\n"
                      "contract Large { function f(uint256 x) external pure returns (uint256) { "
                      + branches + " return 0; } }\n"
                      "contract Query { function read(address a) external view returns (uint256 high, uint256 low) { "
                      "high = a.code.length; assembly { low := extcodesize(a) } } }")
    artifacts = harness.compile(source, extra_args=["--optimization-level", str(optimization)])
    target = harness.deploy(artifacts, "Large", fund_wei=10_000_000)
    app = harness.deploy(artifacts, "Query")
    params = harness.localnet.algod.application_info(target.app_id)["params"]
    assert len(base64.b64decode(params["approval-program"])) > 4096
    expected = (params.get("extra-program-pages", 0) + 1) * 2048
    result = harness.call(app, "read(address)", target.app_id.to_bytes(32, "big"), extra_fee=20_000)
    assert tuple(as_int(x) for x in result.abi_return) == (expected, expected)
    for address in (bytes(32), (2**63 + 17).to_bytes(32, "big")):
        result = harness.call(app, "read(address)", address, extra_fee=20_000)
        assert tuple(as_int(x) for x in result.abi_return) == (0, 0)
    teal = artifacts.by_contract["Query"]["approval_teal"].read_text()
    assert "app_params_get AppApprovalProgram" not in teal
    assert 1 <= teal.count("app_params_get AppExtraProgramPages") <= 2
