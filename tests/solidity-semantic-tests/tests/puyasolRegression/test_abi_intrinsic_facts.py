"""ABI layouts/evaluation and solc-resolved environment names; legacy and IR."""

import base64
import os

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from test_call_operands import invoke


def selector(signature):
    return keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]


def packed_fixed():
    return (b"\x12\x34" + bytes(30) + b"\xab\xcd" + bytes(30)
            + encode(["uint16[2]", "int16[2]", "bool[9]"],
                     [[1, 65535], [-2, 32767], [True, False, False, False, False, False, False, True, True]]))


def packed_dynamic(n):
    return (b"".join((i + 1).to_bytes(32, "big") for i in range(n))
            + b"".join((i + 1).to_bytes(2, "big") + bytes(30) for i in range(n))
            + b"".join(int(i % 2 == 0).to_bytes(32, "big") for i in range(n)))


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_abi_intrinsic_facts(harness, via_ir, profile, slot):
    artifacts = harness.compile("puyasolRegression/contracts/abi_intrinsic_facts.sol",
                                via_yul_behavior=via_ir, extra_args=["--contract-abi", profile]
                                + (["--evm-storage-layout"] if slot else []))
    app = harness.deploy(artifacts, "PackedAbiFacts")
    assert invoke(harness, app, profile, "scalars()", returns=["bytes"]) == (
        bytes.fromhex("1234fffeabcd01") + (0x1234).to_bytes(20, "big") + b"\x01",)
    assert invoke(harness, app, profile, "fixedArrays()", returns=["bytes"]) == (packed_fixed(),)
    for n in (0, 1, 3, 9):
        assert invoke(harness, app, profile, "dynamicArrays(uint256)", [n], ["bytes"]) == (packed_dynamic(n),)
    selected = bytes.fromhex("abcdef01")
    expected = (bytes.fromhex("12345678") + encode(["uint16"], [7])
                + selected + encode(["uint16"], [8])
                + selector("callee(uint256)") + encode(["uint16"], [9]))
    assert invoke(harness, app, profile, "selectors(bytes4,string)",
                  [selected, "callee(uint256)"], ["bytes"]) == (expected,)
    assert invoke(harness, app, profile, "literals()", returns=["bytes"]) == (
        b"\xff\x00\x80OK" + encode(["bytes"], [b"\xff\x00\x80"]),)

    app = harness.deploy(artifacts, "AbiEffectFacts", fund_wei=5_000_000)
    assert invoke(harness, app, profile, "decodeOnce()", returns=["uint256", "uint256", "uint64"]) == (7, 9, 1)
    encoded_call = selector("callee(uint256)") + encode(["uint256"], [7])
    assert invoke(harness, app, profile, "encodeTarget()", returns=["uint64", "bytes"]) == (1, encoded_call)
    for choice in (False, True):
        assert invoke(harness, app, profile, "encodeConditional(bool)", [choice], ["uint64", "bytes"]) == (
            1 if choice else 2, encoded_call)
    assert invoke(harness, app, profile, "encodeDeclaration()", returns=["bytes"]) == (encoded_call,)
    assert invoke(harness, app, profile, "encodeInlineArray()", returns=["bytes"]) == (
        selector("arrayCallee(uint256[2])") + encode(["uint256[2]"], [[7, 9]]),)
    for choice in (False, True):
        assert invoke(harness, app, profile, "literalChoice(bool)", [choice], ["uint64", "bytes"]) == (
            1, b"\xff" if choice else b"\x00\x80")

    app = harness.deploy(artifacts, "ShadowedIntrinsicFacts")
    assert invoke(harness, app, profile, "shadowed()", returns=["uint256", "bytes4", "bytes", "uint256"]) == (
        88, bytes.fromhex("12345678"), bytes.fromhex("abcd"), 31)
    assert invoke(harness, app, profile, "realBlock()", returns=["bool"]) == (True,)


ARRAY_CASES = [
    ("fixedBits", "routedBits", "bool[9]", [True, False, True, False, False, False, False, True, True]),
    ("fixedStrings", "routedStrings", "string[2]", ["abc", "longer than the first"]),
    ("nested", "routedNested", "uint16[][2]", [[1, 65535], [7]]),
    ("structures", "routedStructures", "(uint16,string,bool)[2]", [(1, "abc", True), (65535, "", False)]),
]


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_abi_array_facts(harness, via_ir, profile):
    app = harness.compile_and_deploy("puyasolRegression/contracts/abi_array_facts.sol",
                                    via_yul_behavior=via_ir, extra_args=["--contract-abi", profile])
    for method, routed, kind, values in ARRAY_CASES:
        body = encode([kind], [values])
        assert invoke(harness, app, profile, method + "(bytes)", [body], ["bytes"]) == (body,)
        signature = f"{routed}({kind})"
        if profile == "evm":
            result = harness.call_raw(app, selector(signature), extra_args=(body,),
                                      extra_fee=40_000, budget_pool=8)
            assert decode(["bytes"], result.logs[-1][4:]) == (body,)
        else:
            result = harness.call(app, signature, values, extra_fee=40_000)
            assert bytes(result.abi_return) == body

    for method, kind, values in [
        ("words", "uint256[]", []), ("words", "uint256[]", [0, 2**255, 2**256 - 1]),
        ("signedWords", "int256[3]", [-2**255, -1, 2**255 - 1]),
        ("fixedWords", "bytes32[3]", [bytes(32), bytes.fromhex("01" * 32), bytes.fromhex("ff" * 32)]),
    ]:
        assert invoke(harness, app, profile, f"{method}({kind})", [values], ["bytes"]) == (encode([kind], [values]),)

    for n in (0, 1, 8, 9, 16):
        values = [i % 3 == 0 for i in range(n)]
        assert invoke(harness, app, profile, "routedDynamicBits(bool[])", [values], ["bytes"]) == (
            encode(["bool[]"], [values]),)

    # solc accepts bounded, unaligned tails, even without full word padding.
    for offset in (32, 33, 63):
        body = offset.to_bytes(32, "big") + bytes(offset - 32) + (2).to_bytes(32, "big") + b"hi"
        assert invoke(harness, app, profile, "bytesValue(bytes)", [body], ["bytes"]) == (b"hi",)
    for body in (b"", bytes(31), (2**64).to_bytes(32, "big"),
                 (33).to_bytes(32, "big") + bytes(32),
                 (32).to_bytes(32, "big") + (2).to_bytes(32, "big") + b"h"):
        invoke(harness, app, profile, "bytesValue(bytes)", [body], reverts=True)
    dirty_bool = (2).to_bytes(32, "big") + bytes(8 * 32)
    invoke(harness, app, profile, "fixedBits(bytes)", [dirty_bool], reverts=True)
    # The memcpy fast path still checks the entire array head/body bounds.
    if profile == "evm":
        for signature, body in (("words(uint256[])", encode(["uint256[]"], [[1]])[:-1]),
                                ("signedWords(int256[3])", bytes(95)),
                                ("routedBits(bool[9])", dirty_bool)):
            assert harness.call_raw(app, selector(signature), extra_args=(body,), expect_revert=True).reverted


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_calldata_intrinsic_facts(harness, via_ir, profile):
    app = harness.compile_and_deploy("puyasolRegression/contracts/calldata_intrinsic_facts.sol",
                                    "CalldataIntrinsicFacts", via_yul_behavior=via_ir,
                                    extra_args=["--contract-abi", profile], ctor_args=[17], fund_wei=5_000_000)
    assert invoke(harness, app, profile, "construction()", returns=["uint256", "bytes4"]) == (0, bytes(4))
    if profile == "evm":
        selected = selector("inspect(uint16,bool)")
        body = encode(["uint16", "bool"], [17, True])
    else:
        selected = next(method for method in app.app_spec.methods if method.name == "inspect").to_abi_method().get_selector()
        body = None
    result = invoke(harness, app, profile, "inspect(uint16,bool)", [17, True], ["bytes", "bytes4"])
    assert result[1] == selected
    if profile == "evm":
        assert result[0] == selected + body
        # Do not broaden transport admission merely to ignore metadata.
        assert harness.call_raw(app, selected, extra_args=(body, b"metadata", b"more"), expect_revert=True).reverted
    else:
        # Native parameter ABI widens uint16 to its uint64 backing carrier.
        assert result[0] == selected + (17).to_bytes(8, "big") + b"\x80"
    for data in (b"", b"a", b"ab", b"abc", bytes.fromhex("deadbeef"), bytes.fromhex("deadbeef010203")):
        raw = harness.call_raw(app, data[:4] if data else None,
                               extra_args=(data[4:],) if data else (), extra_fee=10_000)
        assert decode(["bytes", "bytes", "bytes4"], raw.logs[-1][4:]) == (data, data, (data + bytes(4))[:4])


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
def test_xchain_calldata_facts(harness, via_ir):
    from algosdk import transaction
    from test_xchain_accounts import PLACEHOLDER, TOY_TEMPLATE_TEAL

    algod, account = harness.localnet.algod, harness.localnet.account
    template = base64.b64decode(algod.compile(TOY_TEMPLATE_TEAL)["result"])
    owner = os.urandom(20)
    lsig = transaction.LogicSigAccount(template.replace(PLACEHOLDER, owner))
    app = harness.compile_and_deploy("puyasolRegression/contracts/calldata_intrinsic_facts.sol",
                                    "CalldataIntrinsicFacts", via_yul_behavior=via_ir,
                                    extra_args=["--contract-abi", "evm", "--xchain-template", template.hex()],
                                    ctor_args=[17], fund_wei=5_000_000)
    funding = transaction.PaymentTxn(account.address, algod.suggested_params(), lsig.address(), 500_000,
                                     note=os.urandom(8))
    transaction.wait_for_confirmation(algod, algod.send_transaction(funding.sign(account.private_key)), 4)
    selected = selector("inspect(uint16,bool)")
    body = encode(["uint16", "bool"], [17, True])
    txn = transaction.ApplicationNoOpTxn(lsig.address(), algod.suggested_params(), app.app_id,
                                         app_args=[selected, body, owner], note=os.urandom(8))
    pending = transaction.wait_for_confirmation(
        algod, algod.send_transaction(transaction.LogicSigTransaction(txn, lsig)), 4)
    result = decode(["bytes", "bytes4"], base64.b64decode(pending["logs"][-1])[4:])
    assert result == (selected + body, selected)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
def test_block_seed_validity_window(harness, via_ir):
    from algosdk import transaction

    app = harness.compile_and_deploy("puyasolRegression/contracts/calldata_intrinsic_facts.sol",
                                    "BlockSeedFacts", via_yul_behavior=via_ir,
                                    extra_args=["--contract-abi", "evm"])
    algod, account = harness.localnet.algod, harness.localnet.account
    # Deliberately stale FirstValid: Round-2 is newer than the permitted
    # block-read ceiling. Both a narrow and maximal validity window must work.
    first = max(1, algod.status()["last-round"] - 10)
    expected = int.from_bytes(base64.b64decode(algod.block_info(first - 1)["block"]["seed"]), "big")
    for width in (100, 1000):
        for signature, count in (("seed()", 2), ("assemblySeed()", 1)):
            params = algod.suggested_params()
            params.first, params.last = first, first + width
            txn = transaction.ApplicationNoOpTxn(account.address, params, app.app_id,
                                                 app_args=[selector(signature), b""], note=os.urandom(8))
            pending = transaction.wait_for_confirmation(algod, algod.send_transaction(txn.sign(account.private_key)), 4)
            result = decode(["uint256"] * count, base64.b64decode(pending["logs"][-1])[4:])
            assert result == (expected,) * count
