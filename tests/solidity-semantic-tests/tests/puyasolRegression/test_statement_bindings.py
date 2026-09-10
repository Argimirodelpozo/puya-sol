"""Statement conversion, reference binding and effect-order regressions.

Expected values are checked independently with solc 0.8.34 + PyEVM, legacy
and via-IR. Each group runs with both storage layouts and both ABI profiles.
"""

import pytest
from Crypto.Hash import keccak
from eth_abi import decode, encode

from framework import as_int, as_signed_int


@pytest.fixture(params=[False, True], ids=["legacy", "via-ir"])
def via_ir(request):
    return request.param


@pytest.fixture(params=[False, True], ids=["named", "slot"])
def slot_layout(request):
    return request.param


@pytest.fixture(params=["arc4", "evm"])
def profile(request):
    return request.param


def deploy(harness, name, via_ir, slot_layout, profile):
    sources = {
        "StatementDeclarations": "statement_declarations",
        "StatementEffects": "statement_effects",
        "StatementLoops": "statement_conditions",
        "StatementAssemblyFacts": "statement_assembly_facts",
        "StatementCalldata": "statement_calldata",
        "StorageReturnProtocol": "storage_return_protocol",
        "StorageArrayReturns": "storage_array_returns",
    }
    app = harness.compile_and_deploy(
        f"puyasolRegression/contracts/{sources[name]}.sol", contract_name=name,
        via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot_layout else []))

    def check(signature, arguments, expected, returns=None):
        returns = returns or ["uint256"] * len(expected)
        if profile == "arc4":
            result = harness.call(app, signature, *arguments)
            assert not result.reverted, result.fail_message
            values = (result.abi_return,) if len(returns) == 1 else result.abi_return
            actual = tuple((as_signed_int if sol_type.startswith("int") else as_int)(value)
                           for sol_type, value in zip(returns, values, strict=True))
        else:
            inputs = signature.split("(", 1)[1][:-1].split(",") if not signature.endswith("()") else []
            selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
            result = harness.call_raw(app, selector, extra_args=(encode(inputs, arguments),))
            assert not result.reverted, result.fail_message
            actual = decode(returns, result.logs[-1][4:])
        assert actual == expected, (signature, arguments, actual, expected)

    return check


def test_declaration_bindings(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "StatementDeclarations", via_ir, slot_layout, profile)
    for x in (-128, -7, 0, 127):
        check("signedValues(int8)", [x], (x, x, x), ["int128"] * 3)
    check("scalarCopy()", [], (13, 5))
    check("tupleCopy()", [], (13, -7, 19, 5, 7, 1),
          ["uint256", "int128", "uint256", "uint256", "uint256", "uint64"])
    check("arrayCopies()", [], (7, 9, 8, 11))
    check("memoryTupleAlias()", [], (9, 9, 11))


def test_tuple_storage_return_identity(harness, via_ir, slot_layout, profile):
    # Returned storage locations must survive an opaque, mixed-result call.
    check = deploy(harness, "StatementDeclarations", via_ir, slot_layout, profile)
    check("tupleReferences()", [], (23, 29, 1), ["uint256", "uint256", "uint64"])


def test_storage_pointer_rebound_from_tuple(harness, via_ir, slot_layout, profile):
    # The declaration starts as an ordinary alias, but a later opaque tuple
    # assignment requires a runtime handle for every use of that declaration.
    app = harness.compile_and_deploy(
        "various/contracts/tuples.sol", via_yul_behavior=via_ir,
        extra_args=["--contract-abi", profile] + (["--evm-storage-layout"] if slot_layout else []))
    if profile == "arc4":
        result = harness.call(app, "f()")
        assert not result.reverted, result.fail_message
        assert as_int(result.abi_return) == 0
    else:
        selector = keccak.new(digest_bits=256, data=b"f()").digest()[:4]
        result = harness.call_raw(app, selector, extra_args=(b"",))
        assert not result.reverted, result.fail_message
        assert decode(["uint256"], result.logs[-1][4:]) == (0,)


def test_storage_array_return_identity(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "StorageArrayReturns", via_ir, slot_layout, profile)
    check("words(bool)", [False], (5, 13, 17, 7))
    check("words(bool)", [True], (11, 3, 11, 3))
    check("packedWords()", [], (1, 65535, 16, 99, 37))
    check("fixedWords()", [], (3, 29, 7))


def test_storage_return_protocol(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "StorageReturnProtocol", via_ir, slot_layout, profile)
    for swap in (False, True):
        expected = (0, 29, 23, 0, -7, 1) if swap else (23, 0, 0, 29, -7, 1)
        check("references(bool)", [swap], expected,
              ["uint256"] * 4 + ["int128", "uint64"])
        check("copies(bool)", [swap], (3, 5, 7, 11, 99, 88, -7, 1),
              ["uint256"] * 6 + ["int128", "uint64"])
    check("rebind()", [], (0, 37, 31, 0, 2), ["uint256"] * 4 + ["uint64"])
    check("parameterReferences()", [], (41, 43, 7), ["uint256"] * 3)
    check("localReference(bool)", [False], (0, 47, 7), ["uint256"] * 3)
    check("localReference(bool)", [True], (47, 0, 7), ["uint256"] * 3)
    check("singleReference(bool)", [False], (53, 0, 1), ["uint256", "uint256", "uint64"])
    check("singleReference(bool)", [True], (0, 53, 1), ["uint256", "uint256", "uint64"])


def test_statement_effects(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "StatementEffects", via_ir, slot_layout, profile)
    for n in (0, 1, 33, 97):
        for method in ("bytesAllocation", "stringAllocation"):
            check(method + "(uint64)", [n], (n, n, 1), ["uint256", "uint256", "uint64"])
    check("tupleAllocation()", [], (2, 17, 19, 1), ["uint256"] * 3 + ["uint64"])
    for early in (False, True):
        check("voidEffect(bool)", [early], (int(not early),), ["uint64"])


def test_statement_conditions(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "StatementLoops", via_ir, slot_layout, profile)
    for limit in (0, 1, 2, 5):
        for kind in (0, 1, 2):
            body = max(0, limit - 1) if kind < 2 else max(1, limit)
            check("run(uint64,uint64)", [kind, limit],
                  (max(1, limit), body, sum(range(2, body + 1, 2)), body if kind == 1 else 0),
                  ["uint64"] * 4)
        check("branch(uint64)", [limit], (1, 7 if limit > 1 else 0), ["uint64"] * 2)


def test_assembly_declaration_facts(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "StatementAssemblyFacts", via_ir, slot_layout, profile)
    check("shadowed()", [], (1, 2, 7 << 80))
    check("memberAlias()", [], (1, 0))
    check("reassignedSlot()", [], (2, 0))


def test_calldata_binding_effects(harness, via_ir, slot_layout, profile):
    check = deploy(harness, "StatementCalldata", via_ir, slot_layout, profile)
    check("slice(uint256[2][])", [[[11, 13], [17, 19]]], (64, 17, 1),
          ["uint256", "uint256", "uint64"])
