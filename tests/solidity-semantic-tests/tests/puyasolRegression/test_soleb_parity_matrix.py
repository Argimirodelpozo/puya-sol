"""sol-eb/ semantics audit vs solc — oracle answers pinned 2026-09-02 on
solc 0.8.28 + py-evm (see the run log in the audit memory):

  exp: 3**5=243, 7**0=1, 0**7=0, 0**0=1; (-2)**3=-8, (-2)**2=4;
       2**256 checked -> Panic(0x11), unchecked -> 0;
       uint8: 3**4=81, 3**5=243, 4**4 -> Panic(0x11).  (No `**=` in Solidity.)
  bytes4 0x11223344: <<8=0x22334400, <<4=0x12233440, >>12=0x00011223,
       <<=16 (compound) = 0x33440000, >>40 = 0x00000000.
  bytes4 bitwise F0F0F0F0 op 0FF00FF0: &=00F000F0 |=FFF0FFF0 ^=FF00FF00;
       ~F0F0F0F0=0F0F0F0F.
  bytes4[i]: [0]=0x11 [2]=0x33 [7] -> Panic(0x32).
  bytes.concat(bytes2 AABB, hex"CCDD", hex"EE") = aabbccddee (bytesN
       contributes exactly N bytes); string.concat("ab","","cd")="abcd".
"""
import pytest

SOURCE = "puyasolRegression/contracts/soleb_parity_matrix.sol"


@pytest.fixture
def app(harness):
    return harness.compile_and_deploy(SOURCE, "SolEbParity")


def _ok(harness, app, sig, *args, expect=None, fee=20_000):
    r = harness.call(app, sig, *args, extra_fee=fee)
    assert not r.reverted, f"{sig}{args}: {r.fail_message}"
    return r.abi_return


def _revert(harness, app, sig, *args):
    r = harness.call(app, sig, *args, extra_fee=20_000, expect_revert=True)
    assert r.reverted, f"{sig}{args}: expected revert"


def test_exp_cells(harness, app):
    assert _ok(harness, app, "expBasics(uint256,uint256)", 3, 5) == 243
    assert _ok(harness, app, "expBasics(uint256,uint256)", 7, 0) == 1
    assert _ok(harness, app, "expBasics(uint256,uint256)", 0, 7) == 0
    assert _ok(harness, app, "expZeroZero()") == 1
    # decoder-dependent rendering: -8 or its two's-complement word
    neg8 = _ok(harness, app, "expSignedNegBase(int256,uint256)", -2, 3)
    assert neg8 in (-8, (1 << 256) - 8), neg8
    assert _ok(harness, app, "expSignedNegBase(int256,uint256)", -2, 2) == 4
    _revert(harness, app, "expOverflowPanics()")
    assert _ok(harness, app, "expUncheckedWraps()") == 0
    assert _ok(harness, app, "expNarrow(uint8,uint8)", 3, 4) == 81
    assert _ok(harness, app, "expNarrowOverflow()") == 243
    _revert(harness, app, "expNarrowPanics()")


def test_bytesn_shift_cells(harness, app):
    assert bytes(_ok(harness, app, "bytesShiftLeft()")) == bytes.fromhex("22334400")
    assert bytes(_ok(harness, app, "bytesShiftLeftOdd()")) == bytes.fromhex("12233440")
    assert bytes(_ok(harness, app, "bytesShiftRight()")) == bytes.fromhex("00011223")
    assert bytes(_ok(harness, app, "bytesShiftCompound()")) == bytes.fromhex("33440000")
    assert bytes(_ok(harness, app, "bytesShiftOverWidth()")) == bytes.fromhex("00000000")


def test_bytesn_bitwise_cells(harness, app):
    a, o, x = _ok(harness, app, "bytesBitwise()")
    assert bytes(a) == bytes.fromhex("00f000f0")
    assert bytes(o) == bytes.fromhex("fff0fff0")
    assert bytes(x) == bytes.fromhex("ff00ff00")
    assert bytes(_ok(harness, app, "bytesNot()")) == bytes.fromhex("0f0f0f0f")


def test_bytesn_index_cells(harness, app):
    assert bytes(_ok(harness, app, "bytesIndex(uint256)", 0)) == b"\x11"
    assert bytes(_ok(harness, app, "bytesIndex(uint256)", 2)) == b"\x33"
    _revert(harness, app, "bytesIndex(uint256)", 7)


def test_concat_cells(harness, app):
    assert bytes(_ok(harness, app, "bytesConcatMixed()")) == bytes.fromhex("aabbccddee")
    assert _ok(harness, app, "stringConcat3()") == "abcd"
