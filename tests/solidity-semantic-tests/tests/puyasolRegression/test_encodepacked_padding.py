"""encodePacked / bytesN padding-and-width family (new_review.md B10/B11/B12/C17).

B10: fixed arrays of 1-7-byte elements hit an extract(8-N, N) truncation
     written for 8-byte itob inputs → runtime revert.
B11: bytesN array elements were LEFT-padded to the 32-byte word; EVM pads
     fixed bytes on the RIGHT — silent keccak divergence.
B12: mixed-width bytesN compares/bitwise padded only CONSTANT operands, but
     solc legally widens bytesM→bytesN (right-padded).
C17: bool[] ARC-4 bodies are BIT-packed; packed encoding emitted the raw
     bits instead of one 0/1 word per element.

Expected values are the EVM ABI-spec encodings, computed by hand.
"""

from framework import as_bytes


def _word(value: int) -> bytes:
    return value.to_bytes(32, "big")


def _rpad(b: bytes) -> bytes:
    return b + bytes(32 - len(b))


def test_encodepacked_padding_family(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/encodepacked_padding.sol")

    # B12: mixed-width compares and bitwise.
    eq, lt, gte = harness.call(app, "cmpMixed()").abi_return
    assert (eq, lt, gte) == (True, True, False)
    assert bytes(harness.call(app, "bandMixed()").abi_return) == bytes.fromhex("61620000")

    # B10: fixed uint32[3] → three left-padded words.
    got = as_bytes(harness.call(app, "packedFixedU32()").abi_return)
    assert got == _word(1) + _word(2) + _word(3)

    # B11: bytes8 elements pad RIGHT, fixed and dynamic arrays alike.
    b8 = bytes.fromhex("0102030405060708")
    b8b = bytes.fromhex("1112131415161718")
    assert as_bytes(harness.call(app, "packedFixedBytes8()").abi_return) \
        == _rpad(b8) + _rpad(b8b)
    assert as_bytes(harness.call(app, "packedDynBytes8()").abi_return) \
        == _rpad(b8) + _rpad(b8b)

    # C17: bool arrays → one 0/1 word per element.
    assert as_bytes(harness.call(app, "packedDynBool()").abi_return) \
        == _word(1) + _word(0) + _word(1)
    assert as_bytes(harness.call(app, "packedFixedBool()").abi_return) \
        == _word(1) + _word(0)

    # Regression guards: scalar widths and signed sign-extension unchanged.
    assert as_bytes(harness.call(app, "packedScalars()").abi_return) \
        == bytes.fromhex("01020304") + b8b + bytes.fromhex("01")
    assert as_bytes(harness.call(app, "packedSignedArray()").abi_return) \
        == (-2 % 2**256).to_bytes(32, "big") + _word(3)
