"""Foreign references are not executable edges in the caller's solc host."""

import pytest

from test_call_operands import invoke
from test_root_inventory import compile_source


@pytest.mark.parametrize("reference", [
    "token.mint();",
    "((token.mint))();",
    "token.mint{gas: 10000}();",
    "function() external mint = token.mint; mint();",
    "bytes4 selector = token.mint.selector;",
    "bytes4 selector = Token.mint.selector;",
])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_foreign_virtual_reference_frontend(tmp_path, reference, slot):
    result, _, _ = compile_source(tmp_path, """
        contract Token {
            function mint() external { _update(); }
            function _update() internal virtual {}
        }
        contract Bridge {
            function mint(Token token) external { """ + reference + """ }
        }
    """, extra=["--evm-storage-layout"] if slot else [])
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_foreign_virtual_reachability(harness, via_ir, profile, slot):
    artifacts = harness.compile(
        "puyasolRegression/contracts/foreign_reachability.sol",
        via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot else []),
    )
    tokens = [(harness.deploy(artifacts, name, fund_wei=20_000_000), increment)
              for name, increment in [("ReachabilityToken", 1), ("ReachabilityDerivedToken", 11)]]
    for name, increment in [("ReachabilityBridge", 103), ("ReachabilityShadowBridge", 203)]:
        bridge = harness.deploy(artifacts, name, fund_wei=20_000_000)
        assert invoke(harness, bridge, profile, "self(uint256)", [7]) == (7 + increment,)
        for token, token_increment in tokens:
            address = token.app_id.to_bytes(32, "big")
            for method in ("concrete", "throughInterface", "throughPointer"):
                signature = method + "(address,uint256)"
                assert invoke(harness, bridge, profile, signature, [address, 23]) == (23 + token_increment,)
                assert invoke(harness, token, profile, "saved()") == (23,)
            invoke(harness, bridge, profile, "concrete(address,uint256)", [address, 100], reverts=True)
