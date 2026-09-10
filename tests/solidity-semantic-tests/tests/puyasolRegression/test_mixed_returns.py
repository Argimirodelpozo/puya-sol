"""Partially named return lists remain positional and default-initialize every slot."""

import json

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_bytes, as_int, as_signed_int


def nodes(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from nodes(child)
    elif isinstance(value, list):
        for child in value:
            yield from nodes(child)


@pytest.mark.parametrize("via_ir", [False, True])
@pytest.mark.parametrize("slot_layout", [False, True])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_mixed_returns(harness, via_ir, slot_layout, profile):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/mixed_returns.sol", contract_name="MixedReturns",
        via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot_layout else []))

    def check(signature, returns, arguments, expected):
        if profile == "arc4":
            values = harness.call(app, signature, *arguments).abi_return
        else:
            inputs = signature.split("(", 1)[1][:-1].split(",") if not signature.endswith("()") else []
            selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
            result = harness.call_raw(app, selector, extra_args=(encode(inputs, arguments),))
            assert not result.reverted, result.fail_message
            values = decode(returns, result.logs[-1][4:])
        normalized = []
        for value, sol_type in zip(values, returns, strict=True):
            if sol_type.endswith("[1]"):
                normalized.append(tuple(map(as_int, value)))
            elif sol_type == "bytes":
                normalized.append(as_bytes(value))
            elif sol_type.startswith("int"):
                normalized.append(as_signed_int(value))
            else:
                normalized.append(as_int(value))
        assert tuple(normalized) == expected, (signature, arguments)

    triple = ["uint64"] * 3
    array_triple = ["uint64[1]", "uint64", "uint64"]
    check("f()", array_triple, [], ((0,), 1, 2))
    for x in (0, 7, 2**32):
        check("middle(uint64)", ["uint64", "int16", "uint64"], [x], (x, -3, x + 1))
        check("last(uint64)", triple, [x], (x, x + 1, x + 2))
        check("singleGap(uint64)", triple[:2], [x], (x + 1, x))
        check("forward(uint64)", array_triple, [x], ((x,), x + 1, x + 2))
        check("freeCaller(uint64)", triple, [x], (0, x + 1, 0))
        check("libraryCaller(uint64)", ["uint64"] * 4, [x], (x + 2, 0, 0, x + 2))
        check("modified(uint64)", triple, [x], (0, x if via_ir else 2 * x, 0))
    for flag in (False, True):
        check("implicitValues(bool)", ["uint64[1]", "uint64", "bool", "uint16"], [flag],
              ((7,), 2, True, 13) if flag else ((7,), 0, False, 11))
        check("skipped(bool)", triple, [flag], (9 if flag else 0, 0, 0))
    check("implicitHead()", triple, [], (0, 9, 0))
    check("implicitTail()", triple, [], (0, 0, 9))
    check("implicitDynamic()", ["bytes", "uint64", "uint64"], [], (b"", 0, 9))
    check("unnamedAggregates()", ["uint64", "bytes", "uint64[1]"], [], (9, b"", (0,)))
    for method in ("fullyNamed()", "forwardNamed()"):
        check(method, triple[:2], [], (3, 4))
    check("allUnnamed()", triple, [], (3, 4, 5))

    # Optional AWST field names must describe a fully named tuple. Source
    # names still identify the named locals, and complete names are retained.
    awst = json.loads((harness.out_dir / "awst.json").read_text())
    named_tuples = [node["names"] for node in nodes(awst)
                    if node.get("_type") == "WTuple" and node.get("names") is not None]
    assert ["first", "second"] in named_tuples
    for names in named_tuples:
        assert all(names) and len(names) == len(set(names)), names
