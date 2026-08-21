"""Regression coverage for the EVM feature policy."""

import pytest

from framework import as_int
from framework.compile import CompileError


def test_configured_evm_environment(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/evm_feature_policy.sol",
        contract_name="EvmFeaturePolicyRegression",
        extra_args=[
            "--evm-chain-id", "11155111",
            "--evm-block-gas-limit", "30000000",
            "--evm-coinbase", "0x1111111111111111111111111111111111111111",
        ],
    )

    assert bool(as_int(
        harness.call(app, "configuredEnvironmentMatches()").abi_return
    )) is True


def test_evm_bytecode_introspection_is_a_hard_error(harness):
    # type(C).creationCode/runtimeCode have no AVM meaning; both direct use
    # and the folded consumers (.length, keccak256) must fail the compile.
    with pytest.raises(CompileError):
        harness.compile_and_deploy(
            "puyasolRegression/contracts/evm_feature_policy_code.sol",
            contract_name="BytecodeObserver",
        )
