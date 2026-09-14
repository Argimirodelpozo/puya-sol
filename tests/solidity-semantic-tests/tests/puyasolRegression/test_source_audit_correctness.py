"""Whole-src audit reproductions, with canonical solc legacy/viaIR controls."""

import base64
import os

import pytest
from algosdk import transaction
from algosdk.encoding import decode_address
from Crypto.Hash import keccak
from eth_abi import encode

from test_call_operands import invoke

SOURCE = "puyasolRegression/contracts/source_audit_correctness.sol"


def digest(data):
    return keccak.new(digest_bits=256, data=data).digest()


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
def test_audit_self_overload_identity(harness, via_ir):
    app = harness.compile_and_deploy("puyasolRegression/contracts/source_audit_overloads.sol",
        "AuditOverloads", via_yul_behavior=via_ir, extra_args=["--contract-abi", "evm"])
    for method in ("signatureCall", "selectorCall", "typedCall"):
        assert invoke(harness, app, "evm", method + "()") == (32,)
    assert invoke(harness, app, "evm", "missingOverload()", returns=["bool"]) == (False,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_audit_inline_array(harness, via_ir, profile):
    artifacts = harness.compile(SOURCE, via_yul_behavior=via_ir, extra_args=["--contract-abi", profile])
    app = harness.deploy(artifacts, "AuditCalls", fund_wei=5_000_000)
    target = harness.deploy(artifacts, "AuditCallTarget", fund_wei=5_000_000)
    assert invoke(harness, app, profile, "selfArray()") == (7,)
    assert invoke(harness, app, profile, "forwardedArray(address)", [target.app_id.to_bytes(32, "big")]) == (7,)
    assert invoke(harness, app, profile, "encodedArray()", returns=["bytes"]) == (
        digest(b"sum(uint256[2])")[:4] + encode(["uint256[2]"], [[3, 4]]),)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_audit_signed_mapping_getters(harness, via_ir, profile, slot):
    app = harness.compile_and_deploy(SOURCE, "AuditSignedKeys", via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot else []))
    widths = [72, 128, 248, 256]
    for values in ([-1] * 4, [-(1 << (bits - 1)) for bits in widths], [(1 << (bits - 1)) - 1 for bits in widths]):
        # ARC4 carries signed wide inputs as their declared-width TC uintN.
        wire = values if profile == "evm" else [value % (1 << bits) for value, bits in zip(values, widths)]
        signature = "put(int72,int128,int248,int256,uint256)"
        if profile == "evm":
            result = harness.call_raw(app, digest(signature.encode())[:4],
                extra_args=[encode(["int72", "int128", "int248", "int256", "uint256"], [*wire, 77])],
                extra_fee=40_000, budget_pool=8)
        else:
            result = harness.call(app, signature, *wire, 77, extra_fee=40_000)
        assert not result.reverted, result.fail_message
        for name, bits, value in zip(["small", "medium", "large", "word"], widths, wire):
            assert invoke(harness, app, profile, f"{name}(int{bits})", [value]) == (77,)
        assert invoke(harness, app, profile, "explicitGet(int128)", [wire[1]]) == (77,)
        assert invoke(harness, app, profile, "nested(int128,int72)", [wire[1], wire[0]]) == (77,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_audit_hash_memory_ranges(harness, via_ir, profile):
    app = harness.compile_and_deploy(SOURCE, "AuditHashRanges", via_yul_behavior=via_ir,
                                    extra_args=["--contract-abi", profile])
    memory = bytearray(20480)
    memory[4080:4112] = (0x1234).to_bytes(32, "big")
    for offset in (4079, 4080, 4095, 4096):
        for length in (0, 1, 31, 32, 33):
            expected = digest(bytes(memory[offset:offset + length]))
            assert invoke(harness, app, profile, "dynamicHash(uint256,uint256)", [offset, length], ["bytes32"]) == (expected,)
        assert invoke(harness, app, profile, "fixedHash(uint256)", [offset], ["bytes32"]) == (digest(bytes(memory[offset:offset + 32])),)
    for offset in (0, 2**64, 2**256 - 1):
        assert invoke(harness, app, profile, "dynamicHash(uint256,uint256)", [offset, 0], ["bytes32"]) == (digest(b""),)
        assert invoke(harness, app, profile, "emptyHash(uint256)", [offset], ["bytes32"]) == (digest(b""),)
    assert invoke(harness, app, profile, "tailHash(uint256)", [1], ["bytes32"]) == (digest(b"\xab"),)
    assert invoke(harness, app, profile, "fixedTailHash()", returns=["bytes32"]) == (digest(b"\xab"),)
    invoke(harness, app, profile, "tailHash(uint256)", [2], ["bytes32"], reverts=True)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("xchain", [False, True])
def test_audit_caller_identity(harness, via_ir, xchain):
    from test_xchain_accounts import PLACEHOLDER, TOY_TEMPLATE_TEAL

    algod, account = harness.localnet.algod, harness.localnet.account
    flags = ["--contract-abi", "evm"]
    if xchain:
        template = base64.b64decode(algod.compile(TOY_TEMPLATE_TEAL)["result"])
        flags += ["--xchain-template", template.hex()]
    app = harness.compile_and_deploy(SOURCE, "AuditCaller", via_yul_behavior=via_ir, extra_args=flags)
    methods = ["high()", "low()", "libraryLow()"]
    for method in methods:
        result = harness.call_raw(app, digest(method.encode())[:4], extra_args=[b""], extra_fee=10_000)
        assert not result.reverted, result.fail_message
        assert result.logs[-1][4:] == bytes(12) + decode_address(account.address)[-20:]
    if not xchain:
        return
    owner = os.urandom(20)
    lsig = transaction.LogicSigAccount(template.replace(PLACEHOLDER, owner))
    payment = transaction.PaymentTxn(account.address, algod.suggested_params(), lsig.address(), 500_000, note=os.urandom(8))
    transaction.wait_for_confirmation(algod, algod.send_transaction(payment.sign(account.private_key)), 4)
    for method in methods:
        tx = transaction.ApplicationNoOpTxn(lsig.address(), algod.suggested_params(), app.app_id,
            app_args=[digest(method.encode())[:4], b"", owner], note=os.urandom(8))
        pending = transaction.wait_for_confirmation(algod, algod.send_transaction(transaction.LogicSigTransaction(tx, lsig)), 4)
        assert base64.b64decode(pending["logs"][-1])[4:] == bytes(12) + owner
    result = harness.call_raw(app, digest(b"low()")[:4], extra_args=[b"", owner], expect_revert=True)
    assert result.reverted
