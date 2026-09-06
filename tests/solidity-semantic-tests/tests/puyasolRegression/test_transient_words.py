"""Canonical transient words, explicit native-address shadow, and no persistent cells."""

import json

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_bytes, as_int
from framework.compile import CompileError


def _call(harness, app, abi, sig, args=(), returns=()):
    if abi == "arc4":
        result = harness.call(app, sig, *args, extra_fee=30_000)
        values = (result.abi_return,) if len(returns) == 1 else result.abi_return
    else:
        selector = keccak.new(digest_bits=256, data=sig.encode()).digest()[:4]
        result = harness.call_raw(app, selector, extra_args=(encode(["uint256"] * len(args), args),),
                                  extra_fee=30_000, budget_pool=14)
        values = decode(returns, result.logs[-1][4:]) if returns and not result.reverted else ()
    assert not result.reverted, result.fail_message
    normalized = []
    for t, value in zip(returns, values):
        value = as_bytes(value) if t.startswith("bytes") else as_int(value)
        if abi == "arc4" and t.startswith("int") and value >= 2**255:
            value -= 2**256
        normalized.append(value)
    return tuple(normalized)


@pytest.mark.parametrize("abi", ["arc4", "evm"])
@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("via_yul_behavior", [False, True])
def test_canonical_transient_words(harness, abi, slot_layout, via_yul_behavior):
    artifacts = harness.compile(
        "puyasolRegression/contracts/rev_2_transient_words.sol",
        extra_args=["--contract-abi", abi] + (["--evm-storage-layout"] if slot_layout else []),
        via_yul_behavior=via_yul_behavior,
    )
    roots = json.loads((harness.out_dir / "awst.json").read_text())
    for root in roots:
        if root.get("name") in ("TransientWords", "TransientReverse"):
            assert 99 in root["reserved_scratch_space"]
        if root.get("name") == "TransientLeft":
            assert 99 not in root["reserved_scratch_space"]
    app = harness.deploy(artifacts, "TransientWords", exact_schema=True)
    schema = app.app_spec.state.schema.global_state
    assert (schema.ints, schema.bytes) == (0, 0)

    def call(sig, args=(), returns=("bool",)):
        return _call(harness, app, abi, sig, args, returns)

    assert call("layoutA()", returns=("uint256",) * 4) == (0, 2, 22, 30)
    assert call("layoutB()", returns=("uint256",) * 5) == (32, 52, 64, 67, 70)
    assert call("layoutC()", returns=("uint256",) * 4) == (96, 104, 128, 144)
    assert call("empty()") == (1,)
    assert call("typed()", returns=("bool", "bool", "int24", "bytes3")) == (1, 1, -8388607, bytes.fromhex("123456"))
    assert call("empty()") == (1,)  # A later app call starts with no transient state.
    word0 = (1 << 240) | (0x0102030405060708 << 176) | (0x1234 << 16) | 0xFFFE
    word1 = (0x0102030405060708090A0B0C << 160) | 0x5678
    word2 = (0x123456 << 24) | 0xFFFFFE
    assert call("canonicalWords()", returns=("uint256",) * 3) == (word0, word1, word2)
    assert call("fromRaw(uint256,uint256,uint256)", (word0, word1, word2),
                ("bool", "int24", "bytes3")) == (1, -2, bytes.fromhex("123456"))
    # ARC4 msg.sender is a full native address; the EVM profile already
    # zero-extends its low 20 bytes. Raw tstore clears only slot 0's shadow.
    assert call("afterRaw()", returns=("bool",) * 4) == (int(abi == "evm"), 1, 1, 1)
    assert call("effectful()", returns=("int16", "bool", "bool")) == (2, 1, 1)
    assert call("clear()") == (1,)
    assert call("empty()") == (1,)

    reverse = harness.deploy(artifacts, "TransientReverse", exact_schema=True)
    assert _call(harness, reverse, abi, "layout()", returns=("uint256",) * 4) == (0, 20, 28, 29)
    assert _call(harness, reverse, abi, "check()", returns=("bool",)) == (1,)


def test_transient_shadow_with_maximum_memory_reservation(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/rev_2_transient_words.sol", "TransientWords",
        extra_args=["--evm-memory-slots", "88"],
    )
    assert _call(harness, app, "arc4", "typed()", returns=("bool", "bool", "int24", "bytes3")) == (
        1, 1, -8388607, bytes.fromhex("123456"))


def test_transient_declaration_capacity_is_explicit(harness):
    with pytest.raises(CompileError, match="overflows the 5-slot transient declaration capacity"):
        harness.compile("puyasolRegression/contracts/rev_2_transient_capacity.sol")


@pytest.mark.parametrize("abi", ["arc4", "evm"])
def test_full_native_transient_addresses_in_both_profiles(harness, abi):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/rev_2_transient_native_addresses.sol",
        extra_args=["--contract-abi", abi],
    )
    assert _call(harness, app, abi, "check()", returns=("bool",) * 4) == (1, 1, 1, 1)
