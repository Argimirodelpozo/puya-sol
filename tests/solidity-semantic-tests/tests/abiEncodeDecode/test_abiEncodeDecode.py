"""Tests for the abiEncodeDecode category."""
import pytest

from framework import (
    Harness, lpad, rpad, hex_bytes, ErrorString, Panic, Reverted,
    as_int, as_bytes,
)


def _evm_abi_encode_uint_bytes(uint_val: int, raw_bytes: bytes) -> bytes:
    """Build the EVM-ABI encoding of `(uint256, bytes)` — used by the
    abi_decode_* fixtures whose `bytes data` argument is itself the
    EVM-encoded payload that the contract then `abi.decode`s."""
    return (
        uint_val.to_bytes(32, "big")          # uint256 head
        + (0x40).to_bytes(32, "big")          # offset of bytes payload (after the two heads)
        + len(raw_bytes).to_bytes(32, "big")  # bytes length
        + raw_bytes.ljust(((len(raw_bytes) + 31) // 32) * 32, b"\x00")  # bytes data, 32-byte padded
    )


def test_abi_decode_calldata(harness):
    """abiEncodeDecode/contracts/abi_decode_calldata.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_decode_calldata.sol")
    # The function decodes its bytes arg as `(uint256, bytes)` and returns
    # `(33, "abcdefg")`. We pass the EVM-encoded payload as a single bytes arg.
    payload = _evm_abi_encode_uint_bytes(33, b"abcdefg")
    r = harness.call(app, "f(bytes)", payload)
    assert as_int(r.abi_return[0]) == 33
    assert bytes(r.abi_return[1]) == b"abcdefg"

def test_abi_decode_simple(harness):
    """abiEncodeDecode/contracts/abi_decode_simple.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_decode_simple.sol")
    payload = _evm_abi_encode_uint_bytes(33, b"abcdefg")
    r = harness.call(app, "f(bytes)", payload)
    assert as_int(r.abi_return[0]) == 33
    assert bytes(r.abi_return[1]) == b"abcdefg"

def test_abi_decode_simple_storage(harness):
    """abiEncodeDecode/contracts/abi_decode_simple_storage.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_decode_simple_storage.sol")
    payload = _evm_abi_encode_uint_bytes(33, b"abcdefg")
    r = harness.call(app, "f(bytes)", payload)
    assert as_int(r.abi_return[0]) == 33
    assert bytes(r.abi_return[1]) == b"abcdefg"

def test_abi_encode_call(harness):
    """abiEncodeDecode/contracts/abi_encode_call.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_call.sol")
    # callExternal() -> true
    r = harness.call(app, "callExternal()")
    assert bool(as_int(r.abi_return)) is True

def test_abi_encode_call_declaration(harness):  # currently fails
    """abiEncodeDecode/contracts/abi_encode_call_declaration.sol"""
    app = harness.compile_and_deploy('abiEncodeDecode/contracts/abi_encode_call_declaration.sol')
    r = harness.call(app, 'test()')
    assert as_int(r.abi_return) == 11116

def test_abi_encode_call_is_consistent(harness):
    """abiEncodeDecode/contracts/abi_encode_call_is_consistent.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_call_is_consistent.sol")
    # assertConsistentSelectors() ->
    r = harness.call(app, "assertConsistentSelectors()")
    # (void return — call succeeding is the assertion)
    # fSignatureFromLiteral() -> 0x20, 0x84, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    r = harness.call(app, "fSignatureFromLiteral()")
    # TODO: verify structural decoding matches expected: 32, 132, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    assert not r.reverted
    # fSignatureFromLiteralCall() -> 0x20, 0x84, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    r = harness.call(app, "fSignatureFromLiteralCall()")
    # TODO: verify structural decoding matches expected: 32, 132, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    assert not r.reverted
    # fSignatureFromMemory() -> 0x20, 0x84, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    r = harness.call(app, "fSignatureFromMemory()")
    # TODO: verify structural decoding matches expected: 32, 132, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    assert not r.reverted
    # fSignatureFromMemoryCall() -> 0x20, 0x84, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    r = harness.call(app, "fSignatureFromMemoryCall()")
    # TODO: verify structural decoding matches expected: 32, 132, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    assert not r.reverted
    # fSignatureFromMemorys() -> 0x20, 0x84, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    r = harness.call(app, "fSignatureFromMemorys()")
    # TODO: verify structural decoding matches expected: 32, 132, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    assert not r.reverted
    # fPointerCall() -> 0x20, 0x84, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    r = harness.call(app, "fPointerCall()")
    # TODO: verify structural decoding matches expected: 32, 132, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    assert not r.reverted
    # fLocalPointerCall() -> 0x20, 0x84, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    r = harness.call(app, "fLocalPointerCall()")
    # TODO: verify structural decoding matches expected: 32, 132, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    assert not r.reverted
    # fReturnedFunctionPointer() -> 0x20, 0x84, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    r = harness.call(app, "fReturnedFunctionPointer()")
    # TODO: verify structural decoding matches expected: 32, 132, 23450202028776381066253055403048136312616272755117076566855971503345107992576, 26959946667150639794667015087019630673637144422540572481103610249216, 1725436586697640946858688965569256363112777243042596638790631055949824, 86060793054017993816230018372407419485142305772921726565498526629888, 0
    assert not r.reverted

def test_abi_encode_call_memory(harness):
    """abiEncodeDecode/contracts/abi_encode_call_memory.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_call_memory.sol", postinit_inner_txns=4)
    # AVM selector = sha512_256("something()void")[:4]
    r = harness.call(app, "test()", extra_fee=5000)
    assert bytes(r.abi_return) == bytes.fromhex("40e33532")

def test_abi_encode_call_special_args(harness):  # currently fails
    """abiEncodeDecode/contracts/abi_encode_call_special_args.sol"""
    app = harness.compile_and_deploy('abiEncodeDecode/contracts/abi_encode_call_special_args.sol')
    r = harness.call(app, 'assertConsistentSelectors()')
    r = harness.call(app, 'fSignatureFromLiteralNoArgs()')
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 0x04, 12200448252684243758085936796735499259670113115893304444050964496075123064832,)
    r = harness.call(app, 'fPointerNoArgs()')
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 4, 12200448252684243758085936796735499259670113115893304444050964496075123064832,)
    r = harness.call(app, 'fSignatureFromLiteralArray()')
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 0x44, 4612216551196396486909126966576324289294165774260092952932219511233230929920, 862718293348820473429344482784628181556388621521298319395315527974912, 0,)
    r = harness.call(app, 'fPointerArray()')
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 0x44, 4612216551196396486909126966576324289294165774260092952932219511233230929920, 862718293348820473429344482784628181556388621521298319395315527974912, 0,)
    r = harness.call(app, 'fPointerUint()')
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 0x44, 30372892641494467502622535050667754357470287521126424526399600764424271429632, 323519360005807677536004181044235568083645733070486869773243322990592, 350479306672958317330671196131255198757282877493027442254346933239808,)
    r = harness.call(app, 'fSignatureFromLiteralUint()')
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 0x44, 30372892641494467502622535050667754357470287521126424526399600764424271429632, 323519360005807677536004181044235568083645733070486869773243322990592, 350479306672958317330671196131255198757282877493027442254346933239808,)

def test_abi_encode_call_uint_bytes(harness):
    """abiEncodeDecode/contracts/abi_encode_call_uint_bytes.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_call_uint_bytes.sol")
    # f() returns the encoded args (selector stripped) for
    # g(bytes2, bytes2, bytes2). Each bytes2 is right-padded to a 32-byte
    # word: 0x1234, "ab" = 0x6162, 0x1234.
    r = harness.call(app, "f()")
    expected = (
        b"\x12\x34" + b"\x00" * 30
        + b"\x61\x62" + b"\x00" * 30
        + b"\x12\x34" + b"\x00" * 30
    )
    assert bytes(r.abi_return) == expected
    # f2() returns encoded args for h(uint16, uint16): two left-padded uints.
    r = harness.call(app, "f2()")
    assert bytes(r.abi_return) == (0x1234).to_bytes(32, "big") + (0x1234).to_bytes(32, "big")

def test_abi_encode_empty_string_v1(harness):
    """abiEncodeDecode/contracts/abi_encode_empty_string_v1.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_empty_string_v1.sol")
    # f() -> 0x40, 0xa0, 0x40, 0x20, 0x0, 0x0
    r = harness.call(app, "f()")
    # TODO: verify structural decoding matches expected: 64, 160, 64, 32, 0, 0
    assert not r.reverted

def test_abi_encode_with_selector(harness):
    """abiEncodeDecode/contracts/abi_encode_with_selector.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_with_selector.sol")
    sel = bytes.fromhex("12345678")
    # f0() -> just the selector
    assert bytes(harness.call(app, "f0()").abi_return) == sel
    # f1()/f2() -> selector + abicoded "abc" (offset, length, data padded to a word)
    payload_abc = sel + (32).to_bytes(32, "big") + (3).to_bytes(32, "big") + b"abc".ljust(32, b"\x00")
    assert bytes(harness.call(app, "f1()").abi_return) == payload_abc
    assert bytes(harness.call(app, "f2()").abi_return) == payload_abc
    # f3() -> selector + uint256(max). abicoder v1 doesn't word-align the
    # selector, so the result is 36 raw bytes, not a 32-byte-padded layout.
    assert bytes(harness.call(app, "f3()").abi_return) == sel + (2**256 - 1).to_bytes(32, "big")

def test_abi_encode_with_selectorv2(harness):
    """abiEncodeDecode/contracts/abi_encode_with_selectorv2.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_with_selectorv2.sol")
    sel = bytes.fromhex("12345678")

    assert bytes(harness.call(app, "f0()").abi_return) == sel

    payload_abc = sel + (32).to_bytes(32, "big") + (3).to_bytes(32, "big") + b"abc".ljust(32, b"\x00")
    assert bytes(harness.call(app, "f1()").abi_return) == payload_abc
    assert bytes(harness.call(app, "f2()").abi_return) == payload_abc

    assert bytes(harness.call(app, "f3()").abi_return) == sel + (2**256 - 1).to_bytes(32, "big")

    # f4 encodes (uint256.max, S{a,b,c}, uint(3)). The struct has a dynamic
    # `b` field, so the head is (max, offset_to_S, 3) followed by the S tail:
    # (a, offset_to_b, c, length, b_padded).
    s_a = 0x1234567
    s_b = b"Lorem ipsum dolor sit ethereum........"
    s_c = 0x1234
    s_tail = (
        s_a.to_bytes(32, "big")
        + (0x60).to_bytes(32, "big")
        + s_c.to_bytes(32, "big")
        + len(s_b).to_bytes(32, "big")
        + s_b.ljust(((len(s_b) + 31) // 32) * 32, b"\x00")
    )
    f4_payload = (
        sel
        + (2**256 - 1).to_bytes(32, "big")
        + (0x60).to_bytes(32, "big")  # offset to S relative to args region
        + (3).to_bytes(32, "big")
        + s_tail
    )
    assert bytes(harness.call(app, "f4()").abi_return) == f4_payload

def test_abi_encode_with_signature(harness):
    """abiEncodeDecode/contracts/abi_encode_with_signature.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_with_signature.sol")
    # selector("f(uint256)")[:4]
    sel = bytes.fromhex("b3de648b")
    assert bytes(harness.call(app, "f0()").abi_return) == sel

    payload_abc = sel + (32).to_bytes(32, "big") + (3).to_bytes(32, "big") + b"abc".ljust(32, b"\x00")
    assert bytes(harness.call(app, "f1()").abi_return) == payload_abc
    assert bytes(harness.call(app, "f1s()").abi_return) == payload_abc

    # f2 signature is the long Lorem ipsum string; selector picked from the
    # contract's actual hash.
    sel_long = bytes.fromhex("e9c921cd")
    r = harness.call(app, "f2()")
    elems = [(2**256 - 1) - i for i in range(4)]
    expected_r = (
        sel_long
        + (32).to_bytes(32, "big")
        + (4).to_bytes(32, "big")
        + b"".join(v.to_bytes(32, "big") for v in elems)
    )
    assert bytes(r.abi_return[0]) == expected_r
    assert list(r.abi_return[1]) == [0, 0]

def test_abi_encode_with_signaturev2(harness):
    """abiEncodeDecode/contracts/abi_encode_with_signaturev2.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_with_signaturev2.sol")
    sel_f = bytes.fromhex("b3de648b")
    assert bytes(harness.call(app, "f0()").abi_return) == sel_f

    payload_abc = sel_f + (32).to_bytes(32, "big") + (3).to_bytes(32, "big") + b"abc".ljust(32, b"\x00")
    assert bytes(harness.call(app, "f1()").abi_return) == payload_abc
    assert bytes(harness.call(app, "f1s()").abi_return) == payload_abc

    # f2: selector for the long Lorem ipsum signature + encoded uint[4]
    sel_long = bytes.fromhex("e9c921cd")
    elems = [(2**256 - 1) - i for i in range(4)]
    expected_r = (
        sel_long
        + (32).to_bytes(32, "big")
        + (4).to_bytes(32, "big")
        + b"".join(v.to_bytes(32, "big") for v in elems)
    )
    r = harness.call(app, "f2()")
    assert bytes(r.abi_return[0]) == expected_r
    assert list(r.abi_return[1]) == [0, 0]

    # f4: selector("Lorem ipsum dolor sit ethereum........") + uintmax + offset + 3 + S tail
    sel_s_b = bytes.fromhex("7c793002")
    s_a, s_c = 0x1234567, 0x1234
    s_b = b"Lorem ipsum dolor sit ethereum........"
    s_tail = (
        s_a.to_bytes(32, "big")
        + (0x60).to_bytes(32, "big")
        + s_c.to_bytes(32, "big")
        + len(s_b).to_bytes(32, "big")
        + s_b.ljust(((len(s_b) + 31) // 32) * 32, b"\x00")
    )
    f4_payload = (
        sel_s_b
        + (2**256 - 1).to_bytes(32, "big")
        + (0x60).to_bytes(32, "big")
        + (3).to_bytes(32, "big")
        + s_tail
    )
    assert bytes(harness.call(app, "f4()").abi_return) == f4_payload

def test_contract_array(harness):
    """abiEncodeDecode/contracts/contract_array.sol"""
    from algosdk import encoding
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/contract_array.sol")
    # f decodes a bytes payload encoding `(C[])` and returns the C[].
    payload = b"".join(v.to_bytes(32, "big") for v in (32, 3, 1, 2, 3))
    r = harness.call(app, "f(bytes)", payload)
    expected_addrs = [encoding.encode_address(v.to_bytes(32, "big")) for v in (1, 2, 3)]
    assert list(r.abi_return) == expected_addrs
    # g() returns the abi.encode of the in-contract array (addresses 0x42, 0x21, 0x23).
    expected_g = b"".join(v.to_bytes(32, "big") for v in (32, 3, 0x42, 0x21, 0x23))
    assert bytes(harness.call(app, "g()").abi_return) == expected_g

def test_contract_array_v2(harness):  # currently fails
    """abiEncodeDecode/contracts/contract_array_v2.sol"""
    app = harness.compile_and_deploy('abiEncodeDecode/contracts/contract_array_v2.sol')
    r = harness.call(app, 'f(bytes)', 0x20, 0xA0, 0x20, 3, 0x01, 0x02, 0x03)
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 3, 0x01, 0x02, 0x03,)
    r = harness.call(app, 'f(bytes)', 0x20, 0x60, 0x20, 1, 0x0102030405060708090a0b0c0d0e0f1011121314)
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 1, 0x0102030405060708090a0b0c0d0e0f1011121314,)
    r = harness.call(app, 'f(bytes)', 0x20, 0x60, 0x20, 1, 0x0102030405060708090a0b0c0d0e0f101112131415, expect_revert=True)
    assert r.reverted
    r = harness.call(app, 'g()')
    assert tuple(as_int(x) for x in r.abi_return) == (0x20, 0xa0, 0x20, 3, 0x42, 0x21, 0x23,)

def test_offset_overflow_in_array_decoding(harness):
    """abiEncodeDecode/contracts/offset_overflow_in_array_decoding.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/offset_overflow_in_array_decoding.sol")
    # test() -> FAILURE
    r = harness.call(app, "test()", expect_revert=True)
    assert r.reverted

def test_offset_overflow_in_array_decoding_2(harness):  # currently fails
    """abiEncodeDecode/contracts/offset_overflow_in_array_decoding_2.sol"""
    app = harness.compile_and_deploy('abiEncodeDecode/contracts/offset_overflow_in_array_decoding_2.sol')
    r = harness.call(app, 'withinArray()', expect_revert=True)
    assert r.reverted

def test_offset_overflow_in_array_decoding_3(harness):
    """abiEncodeDecode/contracts/offset_overflow_in_array_decoding_3.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/offset_overflow_in_array_decoding_3.sol")
    # test() -> FAILURE
    r = harness.call(app, "test()", expect_revert=True)
    assert r.reverted


def test_encode_dynamic_arg_side_effect_once(harness):
    """abiEncodeDecode/contracts/encode_dyn_side_effect.sol

    CUSTOM regression guard (NOT vendored). A side-effecting DYNAMIC argument to
    abi.encode — `abi.encode(mkStr())`, `abi.encode(mkArr())` — must evaluate
    once. encodeDynamicTail referenced its input in every path (bytes/string:
    len + rightPadTo32 which itself doubles = 3 evals; arrays: 2). Fixed by a
    single makeEvalOnce wrap at the top of encodeDynamicTail; the static-arg
    case is covered by array::test_array_builtin_side_effects_once.
    """
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/encode_dyn_side_effect.sol")
    r = harness.call(app, "encStrOnce()").abi_return
    assert as_int(r[0]) == 1, f"string arg evaluated {as_int(r[0])}x"
    r = harness.call(app, "encArrOnce()").abi_return
    assert (as_int(r[0]), as_int(r[1])) == (1, 9), r


def test_decode_roundtrip_matrix(harness):
    """abiEncodeDecode/contracts/decode_roundtrip_matrix.sol

    CUSTOM regression guard (NOT vendored). abi.decode(abi.encode(x), (T))
    round-trips for: a dynamic struct with a STRING field (before the fix the
    string field's decode fell to the wrong-layout ARC4FromBytes fallback,
    treating the first 2 data bytes as an ARC4 header — S(42,"hi there",7)
    silently decoded as " there"); a struct with uint256[] and bytes fields;
    a mixed static/dynamic tuple; and bytes32+address.

    KNOWN GAP (loud, not asserted here): nested dynamic arrays (uint256[][])
    revert at decode — rtNested() in the contract documents it.
    """
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/decode_roundtrip_matrix.sol")
    r = harness.call(app, "rtStruct()").abi_return
    assert (as_int(r[0]), r[1], as_int(r[2])) == (42, "hi there", 7), r
    r = harness.call(app, "rtStructArr()").abi_return
    assert tuple(as_int(x) for x in r) == (5, 6, 2, 3), r
    r = harness.call(app, "rtTuple()").abi_return
    assert (as_int(r[0]), bytes(r[1]).hex(), as_int(r[2]), bool(r[3])) == (9, "deadbeef", 513, True), r
    r = harness.call(app, "rtFixed()")
    assert r.abi_return is not None
