"""Builder arithmetic, initializer order and modifier/storage boundaries: solc oracles."""

import pytest
from Crypto.Hash import keccak

from framework.paths import TESTS_DIR
from test_ast_audit import compile_app
from test_call_operands import invoke


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_eb_arithmetic(harness, via_ir, profile):
    artifacts = compile_app(harness, "eb_arithmetic", via_ir, profile, False)
    app = harness.deploy(artifacts, "EbArithmetic")
    for bits in (32, 40, 48, 56, 64):
        signature = f"mul{bits}(uint{bits},uint{bits})"
        for a, b in ((0, 0), (3, 7), (1 << (bits - 1), 1 << (bits - 1)),
                     ((1 << bits) - 1, (1 << bits) - 1)):
            assert invoke(harness, app, profile, signature, [a, b]) == ((a * b) % (1 << bits),)
    assert invoke(harness, app, profile, "compound40(uint40,uint40)", [1 << 39, 1 << 39]) == (0,)
    assert invoke(harness, app, profile, "checked40(uint40,uint40)", [3, 7]) == (21,)
    invoke(harness, app, profile, "checked40(uint40,uint40)", [1 << 39, 1 << 39], reverts=True)
    for bits in (8, 128):
        invoke(harness, app, profile, f"shiftNeg{bits}(int{bits})", [1 << (bits - 2)], reverts=True)
        invoke(harness, app, profile, f"notNeg{bits}(int{bits})", [(1 << (bits - 1)) - 1], reverts=True)
        assert invoke(harness, app, profile, f"notNeg{bits}(int{bits})", [0], [f"int{bits}"]) == (1,)
    assert invoke(harness, app, profile, "complement(int128)", [0], ["bool"]) == (True,)
    assert invoke(harness, app, profile, "shifted(int128)", [1 << 126], ["bool"]) == (True,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_contract_initializers(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "contract_initializers", via_ir, profile, slot)
    def deploy(name):
        return harness.deploy(artifacts, name, fund_wei=30_000_000, postinit_budget_pool=8)
    fixed = deploy("FixedInitializers")
    for i in range(5):
        assert invoke(harness, fixed, profile, "xs(uint256)", [i]) == (11 + i,)
    defaults = deploy("GlobalDefaultInitializers")
    assert invoke(harness, defaults, profile, "initialized()") == (0,)
    assert invoke(harness, defaults, profile, "entries(uint256)", [0]) == (0,)
    assert invoke(harness, defaults, profile, "entries(uint256)", [1]) == (17,)
    assert invoke(harness, defaults, profile, "pair()", returns=["uint64"] * 2) == (29, 0)
    ordered = deploy("OrderedInitializers")
    for getter, value in (("seed()", 7), ("calls()", 1), ("afterBytes()", 1),
                           ("written()", 23), ("forward()", 19)):
        assert invoke(harness, ordered, profile, getter) == (value,)
    assert invoke(harness, ordered, profile, "xs(uint256)", [0]) == (7,)
    assert invoke(harness, ordered, profile, "b()", returns=["bytes"]) == ((7).to_bytes(32, "big"),)
    derived = deploy("InitializerDerived")
    assert invoke(harness, derived, profile, "seed()") == (9,)
    assert invoke(harness, derived, profile, "beforeConstructor(uint256)", [0]) == (7,)
    assert invoke(harness, derived, profile, "derived(uint256)", [0]) == (9 if via_ir else 7,)
    assert invoke(harness, derived, profile, "fixedValues(uint256)", [4]) == (25,)
    raw = deploy("BytesInitializers")
    for getter, value in (("encoded()", (7).to_bytes(32, "big")),
                           ("literalValue()", b"abc"), ("emptyValue()", b"")):
        assert invoke(harness, raw, profile, getter, returns=["bytes"]) == (value,)
    if profile == "evm":
        assert invoke(harness, raw, profile, "composed()", returns=["string"]) == ("abcd",)
    else:
        assert harness.call(raw, "composed()").abi_return == "abcd"


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_modifier_argument_boundaries(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "contract_modifier_args", via_ir, profile, slot)
    app = harness.deploy(artifacts, "ModifierArguments", fund_wei=30_000_000)
    assert invoke(harness, app, profile, "widened(int8)", [-1], ["bool"]) == (True,)
    invoke(harness, app, profile, "widened(int8)", [1], reverts=True)
    assert invoke(harness, app, profile, "plain()") == (1,)
    assert invoke(harness, app, profile, "parens()") == (2,)
    for i in (1, 2):
        assert invoke(harness, app, profile, "mapped()") == (i,)
        assert invoke(harness, app, profile, "computed()", returns=["uint256"] * 2) == (i, i)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_storage_reads_do_not_allocate(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "contract_storage_words", via_ir, profile, slot)
    app = harness.deploy(artifacts, "RawStorageRead", fund_wei=30_000_000)
    boxes = harness.localnet.algod.application_boxes(app.app_id)
    assert invoke(harness, app, profile, "named()") == (0,)
    assert invoke(harness, app, profile, "constantSlot()") == (0,)
    for key in (0, 777, 1 << 128, (1 << 256) - 1):
        assert invoke(harness, app, profile, "read(uint256)", [key]) == (0,)
    assert harness.localnet.algod.application_boxes(app.app_id) == boxes
    assert invoke(harness, app, profile, "store(uint256,uint256)", [777, 123]) == (123,)
    assert invoke(harness, app, profile, "read(uint256)", [777]) == (123,)
    assert invoke(harness, app, profile, "store(uint256,uint256)", [0, 33]) == (33,)
    assert invoke(harness, app, profile, "named()") == (33,)
    assert invoke(harness, app, profile, "constantSlot()") == (33,)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_bytes_storage_header_validation(harness, via_ir, profile):
    artifacts = compile_app(harness, "contract_storage_words", via_ir, profile, True)
    app = harness.deploy(artifacts, "BytesStorageHeaders", fund_wei=30_000_000)
    for method in ("read", "replace"):
        for word in (1, 3, 63, 64, 126, 254):
            invoke(harness, app, profile, method + "(uint256)", [word], reverts=True)
    assert invoke(harness, app, profile, "read(uint256)", [0], ["bytes"]) == (b"",)
    assert invoke(harness, app, profile, "read(uint256)", [(0x11 << 248) | 2], ["bytes"]) == (b"\x11",)
    assert invoke(harness, app, profile, "read(uint256)", [65], ["bytes"]) == (bytes(32),)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
def test_fixed_bytes_conversions(harness, via_ir, profile):
    artifacts = compile_app(harness, "eb_bytes", via_ir, profile, False)
    app = harness.deploy(artifacts, "EbBytes")
    for value in (b"", b"x", b"abcd", b"abcdef", b"z" * 1024):
        assert invoke(harness, app, profile, "resize(bytes)", [value], ["bytes4"]) == ((value + bytes(4))[:4],)
    for a, b in ((b"ab", b"abcd"), (b"ab", b"ab\x00\x00"), (b"\xff\xff", bytes(4))):
        left, right = int.from_bytes(a + bytes(2), "big"), int.from_bytes(b, "big")
        expected = tuple(v.to_bytes(4, "big") for v in (left | right, left & right, left ^ right))
        assert invoke(harness, app, profile, "mixed(bytes2,bytes4)", [a, b],
                      ["bytes4"] * 3 + ["bool", "bool"]) == expected + (left == right, left < right)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("v1", [False, True], ids=["v2", "v1"])
def test_contract_entry_validation(harness, tmp_path, via_ir, profile, v1):
    source = TESTS_DIR / "puyasolRegression/contracts/contract_ingress.sol"
    if v1:
        rewritten = tmp_path / "V1.sol"
        rewritten.write_text(source.read_text().replace("pragma solidity ^0.8.20;",
                                                       "pragma solidity ^0.8.20; pragma abicoder v1;"))
        source = rewritten
    artifacts = harness.compile(source, via_yul_behavior=via_ir, extra_args=["--contract-abi", profile])
    app = harness.deploy(artifacts, "ContractIngress", fund_wei=30_000_000)
    assert invoke(harness, app, profile, "fromDirty(uint256)", [263]) == (14,)
    assert invoke(harness, app, profile, "recurse(uint8)", [3]) == (4,)
    assert invoke(harness, app, profile, "skipped(uint8)", [7]) == (0,)
    assert invoke(harness, app, profile, "selfWord(uint256)", [263]) == (7,)
    for pointer in (False, True):
        assert invoke(harness, app, profile, "selfEnum(uint256,bool)", [1, pointer]) == (77,)
        invoke(harness, app, profile, "selfEnum(uint256,bool)", [5, pointer], reverts=True)
    for method in ("internalPointer", "externalPointer"):
        assert invoke(harness, app, profile, method + "()", returns=["uint128"]) == ((1 << 90) + 7,)

    def raw(name, signature, value, reverts):
        if profile == "evm":
            selector = keccak.new(digest_bits=256, data=signature.encode()).digest()[:4]
            argument = value.to_bytes(32, "big")
        else:
            selector = next(m for m in app.app_spec.methods if m.name == name).to_abi_method().get_selector()
            argument = value.to_bytes(8, "big")
        result = harness.call_raw(app, selector, extra_args=[argument], expect_revert=reverts,
                                  extra_fee=40_000, budget_pool=8)
        assert result.reverted == reverts
        return result

    # The canonical EVM decoder is strict in both source ABI versions;
    # native ARC4 retains solc's legacy-v1 cleanup/getter-enum convention.
    strict = profile == "evm" or via_ir or not v1
    for name, signature, argument, expected in (
            ("skipped", "skipped(uint8)", 256, 0),
            ("shared", "shared(uint8)", 263, 7),
            ("byNumber", "byNumber(uint8)", 263, 11),
            ("byChoice", "byChoice(uint8)", 2, 0)):
        result = raw(name, signature, argument, strict)
        if not strict:
            assert int.from_bytes(result.logs[-1][4:], "big") == expected
    raw("enumArg", "enumArg(uint8)", 2, True)
