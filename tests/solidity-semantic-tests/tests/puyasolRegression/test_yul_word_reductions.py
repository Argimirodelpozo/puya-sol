"""Full-width solc/Yul constants, shared word math, and observable operands."""
import pytest

from test_ast_audit import compile_app
from test_call_operands import invoke
from test_root_inventory import compile_source


WORD_MASK = 2**256 - 1


def word_results(x, n):
    signed = x - 2**256 if x >= 2**255 else x
    shift = min(n, 256)
    if n >= 31:
        extended = x
    else:
        bits = 8 * (n + 1)
        low = x & (2**bits - 1)
        extended = (low - 2**bits if low & (1 << (bits - 1)) else low) & WORD_MASK
    return ((x << shift) & WORD_MASK, x >> shift, (signed >> shift) & WORD_MASK, extended)


@pytest.mark.parametrize("via_ir", [False, True], ids=["legacy", "via-ir"])
@pytest.mark.parametrize("profile", ["arc4", "evm"])
@pytest.mark.parametrize("slot", [False, True], ids=["named", "slot"])
def test_yul_word_reductions(harness, via_ir, profile, slot):
    artifacts = compile_app(harness, "yul_word_reductions", via_ir, profile, slot)
    app = harness.deploy(artifacts, "YulWordReductions")
    for x in (0, 17, 2**255, WORD_MASK):
        assert invoke(harness, app, profile, "largeSignextend(uint256)", [x], ["uint256"] * 2) == (x, x)
    pairs = [(x, n) for x in (0, 0x80, 2**255, WORD_MASK) for n in (0, 7, 31, 255, 256, 2**64, WORD_MASK)]
    for x, n in pairs:
        expected = word_results(x, n)
        assert invoke(harness, app, profile, "words(uint256,uint256)", [x, n], ["uint256"] * 4) == expected
        assert invoke(harness, app, profile, "typed(uint256,uint256)", [x, n], ["uint256"] * 3) == expected[:3]
    for n in (0, 8, 256, 2**64):
        for op, expected in enumerate(word_results(WORD_MASK, n)):
            assert invoke(harness, app, profile, "ordered(uint256,uint256,uint256)",
                          [WORD_MASK, n, op], ["uint256"] * 2) == (expected, 21)
    assert invoke(harness, app, profile, "literals()", returns=["bytes32"] * 4) == tuple(
        value.ljust(32, b"\x00") for value in (b"abc", b"\x00\x01\xff\x00", b'\x00"\\\xff', b""))


@pytest.mark.parametrize("expression", ["add(1)", "mload(0, 1)", "sstore(0)", "log2(0, 0, 1)"])
def test_solc_owns_yul_arity(tmp_path, expression):
    prefix = "let value := " if expression.startswith(("add", "mload")) else ""
    result, _, _ = compile_source(tmp_path, "contract C { function f() external { assembly { "
                                 + prefix + expression + " } } }")
    assert result.returncode != 0
    assert "expects" in result.stderr and "arguments" in result.stderr
    assert "Internal compiler error" not in result.stderr
