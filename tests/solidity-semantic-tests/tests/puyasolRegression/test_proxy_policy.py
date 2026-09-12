"""Explicit proxy registration and native lifecycle policy (candidate only)."""

import json
import subprocess

import pytest

from framework.paths import COMPILER
from test_address_metadata import call as metadata_call
from test_erc1967 import REJECTED, _funded_account, _update_app as update_admin
from test_uups import _update_app as update_uups
from test_erc1967 import test_proxy_adaptation_default_off_named_bodies as check_named_bodies


def call(harness, app, profile, signature, arguments, returns):
    if not returns:
        if profile == "arc4":
            harness.call(app, signature, *arguments, extra_fee=40_000)
        else:
            from Crypto.Hash import keccak
            assert not arguments  # The void call here is initialize().
            selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
            harness.call_raw(app, selector, extra_args=(b"",), extra_fee=40_000)
        return ()
    return metadata_call(harness, app, profile, signature, arguments, returns)


@pytest.mark.parametrize("layout", [[], ["--evm-storage-layout"]], ids=["native", "slots"])
def test_unregistered_names_are_ordinary_even_when_enabled(harness, layout):
    check_named_bodies(harness, ["--proxy-adaptation", *layout])


@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("layout", [[], ["--evm-storage-layout"]], ids=["native", "slots"])
@pytest.mark.parametrize("name", ["AdminPolicy", "AdminConstructor", "AdminFallback"])
def test_admin_profile_and_lifecycle(harness, profile, layout, name):
    artifacts = harness.compile("puyasolRegression/contracts/proxy_policy.sol",
                                extra_args=["--proxy-adaptation", "--contract-abi", profile, *layout])
    spec = json.loads(artifacts.by_contract[name]["arc56"].read_text())
    gates = [method for method in spec["methods"] if method["name"].endswith("_update")]
    assert [method["name"] for method in gates] == ["__erc1967_update"]
    assert gates[0]["actions"] == {"create": [], "call": ["UpdateApplication"]}
    assert any(method["name"] == "__postInit" for method in spec["methods"]) == (name == "AdminConstructor")
    app = harness.deploy(artifacts, name, fund_wei=30_000_000, postinit_budget_pool=8)
    owner = harness.localnet.account
    # Uninitialized and selector-less update surfaces stay closed.
    with pytest.raises(Exception, match=REJECTED):
        update_admin(harness, app, artifacts, name, owner.address, owner.private_key)
    call(harness, app, profile, "initialize()", [], [])
    stranger, stranger_key = _funded_account(harness)
    with pytest.raises(Exception, match=REJECTED):
        update_admin(harness, app, artifacts, name, stranger, stranger_key)
    with pytest.raises(Exception, match=REJECTED):
        update_admin(harness, app, artifacts, name, owner.address, owner.private_key, bare=True)
    update_admin(harness, app, artifacts, name, owner.address, owner.private_key)
    assert call(harness, app, profile, "value()", [], ["uint256"]) == (17,)


@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("layout", [[], ["--evm-storage-layout"]], ids=["native", "slots"])
@pytest.mark.parametrize("name,increment", [("DiamondPolicy", 1011), ("LifecycleOnly", 1)])
def test_exact_uups_hook_and_native_reachability(harness, profile, layout, name, increment):
    artifacts = harness.compile("puyasolRegression/contracts/proxy_policy.sol",
                                extra_args=["--proxy-adaptation", "--contract-abi", profile, *layout])
    app = harness.deploy(artifacts, name)
    owner = harness.localnet.account
    # Both owner and counter fit the first EVM storage page. Supply identical
    # resources to authorized and denied transactions so denial is the hook's.
    boxes = [(0, b"p:" + bytes(8))] if layout else None
    call(harness, app, profile, "initialize()", [], [])
    if name == "DiamondPolicy":
        assert call(harness, app, profile, "overload()", [], ["uint256"]) == (12,)
    stranger, key = _funded_account(harness)
    with pytest.raises(Exception, match=REJECTED):
        update_uups(harness, app, artifacts, name, stranger, key, boxes=boxes)
    with pytest.raises(Exception, match=REJECTED):
        update_uups(harness, app, artifacts, name, owner.address, owner.private_key, bare=True, boxes=boxes)
    update_uups(harness, app, artifacts, name, owner.address, owner.private_key, boxes=boxes)
    assert call(harness, app, profile, "checks()", [], ["uint256"]) == (increment,)


def frontend(tmp_path, source, *arguments):
    path = tmp_path / "proxy.sol"
    path.write_text("// SPDX-License-Identifier: MIT\npragma solidity ^0.8.20;\n" + source)
    return subprocess.run([str(COMPILER), "--source", str(path), "--no-puya",
                           "--output-dir", str(tmp_path / "out"), *arguments],
                          capture_output=True, text=True, timeout=60)


@pytest.mark.parametrize("source,message", [
    ("/// @custom:avm-proxy unknown\ncontract C {}", "requires one role"),
    ("/// @custom:avm-proxy erc1967-utils\nlibrary L { function _setAdmin() private {} }",
     "unsupported annotated signature"),
    ("/// @custom:avm-proxy uups\nabstract contract B { "
     "function _checkProxy(uint256) internal view {} "
     "function _authorizeUpgrade(address) internal virtual; }", "unsupported annotated signature"),
    ("/// @custom:avm-proxy uups\nabstract contract B { "
     "function _authorizeUpgrade(uint256) internal virtual; }", "unsupported annotated signature"),
])
def test_malformed_proxy_registration_rejected(tmp_path, source, message):
    result = frontend(tmp_path, source, "--proxy-adaptation")
    assert result.returncode != 0
    assert message in result.stdout + result.stderr
    assert not (tmp_path / "out/awst.json").exists()


@pytest.mark.parametrize("hook", [
    "function _authorizeUpgrade(address proposed) internal override { require(proposed != address(0)); }",
    "modifier valid(address proposed) { require(proposed != address(0)); _; } "
    "function _authorizeUpgrade(address proposed) internal override valid(proposed) {}",
    "function check(address) private pure {} "
    "function _authorizeUpgrade(address proposed) internal override { check(proposed); }",
    "function _authorizeUpgrade(address proposed) internal override { assembly { pop(proposed) } }",
])
def test_implementation_dependent_hook_rejected(tmp_path, hook):
    result = frontend(tmp_path, "/// @custom:avm-proxy uups\nabstract contract B { "
                      "function _authorizeUpgrade(address) internal virtual; }\n"
                      "contract C is B { " + hook + " }", "--proxy-adaptation")
    assert result.returncode != 0
    assert "consumes its implementation argument" in result.stdout + result.stderr
    assert not (tmp_path / "out/awst.json").exists()


def test_mixed_native_policy_rejected(tmp_path):
    result = frontend(tmp_path, "/// @custom:avm-proxy uups\nabstract contract B { "
                      "function _authorizeUpgrade(address) internal virtual; }\n"
                      "contract C is B { function _authorizeUpgrade(address) internal override {} "
                      "function admin() external view returns (address a) { assembly { "
                      "a := sload(0xb53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5d6103) } } }",
                      "--proxy-adaptation")
    assert result.returncode != 0
    assert "ambiguous native update policy" in result.stdout + result.stderr
    assert not (tmp_path / "out/awst.json").exists()


def test_annotations_do_not_enable_adaptation(tmp_path):
    result = frontend(tmp_path, "/// @custom:avm-proxy unknown\ncontract C { "
                      "function f() external pure returns (uint256) { return 17; } }")
    assert result.returncode == 0, result.stdout + result.stderr
    awst = (tmp_path / "out/awst.json").read_text()
    assert "__uups_update" not in awst and "__erc1967_update" not in awst


@pytest.mark.parametrize("name,gate", [("AdminPolicy", "__erc1967_update"), ("LifecycleOnly", "__uups_update")])
def test_native_gate_uses_verified_xchain_identity(harness, name, gate):
    import base64
    import os
    from algosdk import transaction
    from algosdk.abi import Method
    from Crypto.Hash import keccak
    from test_xchain_accounts import PLACEHOLDER, TOY_TEMPLATE_TEAL

    algod, account = harness.localnet.algod, harness.localnet.account
    template = base64.b64decode(algod.compile(TOY_TEMPLATE_TEAL)["result"])
    owner = os.urandom(20)
    lsig = transaction.LogicSigAccount(template.replace(PLACEHOLDER, owner))
    artifacts = harness.compile("puyasolRegression/contracts/proxy_policy.sol",
                                extra_args=["--proxy-adaptation", "--contract-abi", "evm",
                                            "--xchain-template", template.hex()])
    app = harness.deploy(artifacts, name, fund_wei=2_000_000)
    fund = transaction.PaymentTxn(account.address, algod.suggested_params(), lsig.address(), 500_000,
                                   note=os.urandom(8))
    transaction.wait_for_confirmation(algod, algod.send_transaction(fund.sign(account.private_key)), 4)
    selector = keccak.new(digest_bits=256, data=b"initialize()").digest()[:4]
    initialize = transaction.ApplicationNoOpTxn(lsig.address(), algod.suggested_params(), app.app_id,
                                                app_args=[selector, b"", owner], note=os.urandom(8))
    transaction.wait_for_confirmation(algod, algod.send_transaction(
        transaction.LogicSigTransaction(initialize, lsig)), 4)
    entry = artifacts.by_contract[name]
    approval = base64.b64decode(algod.compile(entry["approval_teal"].read_text())["result"])
    clear = base64.b64decode(algod.compile(entry["clear_teal"].read_text())["result"])
    update = transaction.ApplicationUpdateTxn(lsig.address(), algod.suggested_params(), app.app_id,
                                               approval, clear, app_args=[
                                                   Method.from_signature(gate + "()void").get_selector(), b"", owner])
    confirmed = transaction.wait_for_confirmation(algod, algod.send_transaction(
        transaction.LogicSigTransaction(update, lsig)), 4)
    assert confirmed.get("confirmed-round", 0) > 0
