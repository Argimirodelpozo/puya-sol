"""Tests for the abiEncodeDecode category."""
import pytest
from Crypto.Hash import keccak
from eth_abi import encode as evm_encode

from framework import as_int


def _evm_abi_encode_uint_bytes(uint_val: int, raw_bytes: bytes) -> bytes:
    """Build the canonical EVM-ABI encoding of `(uint256, bytes)`."""
    return (
        uint_val.to_bytes(32, "big")          # uint256 head
        + (0x40).to_bytes(32, "big")          # offset of bytes payload (after the two heads)
        + len(raw_bytes).to_bytes(32, "big")  # bytes length
        + raw_bytes.ljust(((len(raw_bytes) + 31) // 32) * 32, b"\x00")  # bytes data, 32-byte padded
    )


def _evm_selector(signature: str) -> bytes:
    digest = keccak.new(digest_bits=256)
    digest.update(signature.encode())
    return digest.digest()[:4]


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

def test_abi_encode_call_declaration(harness):
    """abiEncodeDecode/contracts/abi_encode_call_declaration.sol"""
    app = harness.compile_and_deploy('abiEncodeDecode/contracts/abi_encode_call_declaration.sol')
    r = harness.call(app, 'test()')
    assert as_int(r.abi_return) == 11116

def test_abi_encode_call_is_consistent(harness):
    """abiEncodeDecode/contracts/abi_encode_call_is_consistent.sol"""
    app = harness.compile_and_deploy(
        "abiEncodeDecode/contracts/abi_encode_call_is_consistent.sol",
        extra_args=["--evm-selectors"])
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
    app = harness.compile_and_deploy(
        "abiEncodeDecode/contracts/abi_encode_call_memory.sol",
        postinit_inner_txns=4, extra_args=["--evm-selectors"])
    r = harness.call(app, "test()", extra_fee=5000)
    assert bytes(r.abi_return) == _evm_selector("something()")

def test_abi_encode_call_special_args(harness):
    """abiEncodeDecode/contracts/abi_encode_call_special_args.sol"""
    app = harness.compile_and_deploy('abiEncodeDecode/contracts/abi_encode_call_special_args.sol')
    r = harness.call(app, 'assertConsistentSelectors()')
    r = harness.call(app, 'fSignatureFromLiteralNoArgs()')
    no_args = _evm_selector("fNoArgs()")
    assert bytes(r.abi_return) == no_args
    r = harness.call(app, 'fPointerNoArgs()')
    assert bytes(r.abi_return) == no_args
    array_args = _evm_selector("fArray(uint256[])") + evm_encode(
        ["uint256[]"], [[]])
    r = harness.call(app, 'fSignatureFromLiteralArray()')
    assert bytes(r.abi_return) == array_args
    r = harness.call(app, 'fPointerArray()')
    assert bytes(r.abi_return) == array_args
    uint_args = _evm_selector("fUint(uint256,uint256)") + evm_encode(
        ["uint256", "uint256"], [12, 13])
    r = harness.call(app, 'fPointerUint()')
    assert bytes(r.abi_return) == uint_args
    r = harness.call(app, 'fSignatureFromLiteralUint()')
    assert bytes(r.abi_return) == uint_args

def test_abi_encode_call_uint_bytes(harness):
    """abiEncodeDecode/contracts/abi_encode_call_uint_bytes.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_call_uint_bytes.sol")
    # f() returns the canonical EVM argument body for
    # g(bytes2,bytes2,bytes2), with each fixed-bytes value right-padded.
    r = harness.call(app, "f()")
    assert bytes(r.abi_return) == evm_encode(
        ["bytes2", "bytes2", "bytes2"],
        [bytes.fromhex("1234"), b"ab", bytes.fromhex("1234")])
    # Sub-word integers remain 32-byte EVM words even though their internal
    # representation is native uint64.
    r = harness.call(app, "f2()")
    assert bytes(r.abi_return) == evm_encode(["uint16", "uint16"], [0x1234, 0x1234])

def test_abi_encode_empty_string_v1(harness):
    """abiEncodeDecode/contracts/abi_encode_empty_string_v1.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_empty_string_v1.sol")
    # abi.encode uses a canonical dynamic head/tail; encodePacked is empty.
    r = harness.call(app, "f()")
    assert bytes(r.abi_return[0]) == evm_encode(["string"], [""])
    assert bytes(r.abi_return[1]) == b""

def test_abi_encode_with_selector(harness):
    """abiEncodeDecode/contracts/abi_encode_with_selector.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_with_selector.sol")
    sel = bytes.fromhex("12345678")
    # f0() -> just the selector
    assert bytes(harness.call(app, "f0()").abi_return) == sel
    payload_abc = sel + evm_encode(["string"], ["abc"])
    assert bytes(harness.call(app, "f1()").abi_return) == payload_abc
    assert bytes(harness.call(app, "f2()").abi_return) == payload_abc
    # f3() -> selector + uint256(max). ARC4 uint256 is the same 32-byte big-endian
    # word as the EVM encoding, so this case is unchanged.
    assert bytes(harness.call(app, "f3()").abi_return) == sel + (2**256 - 1).to_bytes(32, "big")

def test_abi_encode_with_selectorv2(harness):
    """abiEncodeDecode/contracts/abi_encode_with_selectorv2.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_with_selectorv2.sol")
    sel = bytes.fromhex("12345678")

    assert bytes(harness.call(app, "f0()").abi_return) == sel

    payload_abc = sel + evm_encode(["string"], ["abc"])
    assert bytes(harness.call(app, "f1()").abi_return) == payload_abc
    assert bytes(harness.call(app, "f2()").abi_return) == payload_abc

    assert bytes(harness.call(app, "f3()").abi_return) == sel + (2**256 - 1).to_bytes(32, "big")

    # f4 encodes (uint256.max, S{uint a, string b, uint16 c}, uint(3)).
    s_b = "Lorem ipsum dolor sit ethereum........"
    f4_payload = sel + evm_encode(
        ["uint256", "(uint256,string,uint16)", "uint256"],
        [2**256 - 1, (0x1234567, s_b, 0x1234), 3])
    assert bytes(harness.call(app, "f4()").abi_return) == f4_payload

def test_abi_encode_with_signature(harness):
    """abiEncodeDecode/contracts/abi_encode_with_signature.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_with_signature.sol")
    sel = _evm_selector("f(uint256)")
    assert bytes(harness.call(app, "f0()").abi_return) == sel

    payload_abc = sel + evm_encode(["string"], ["abc"])
    assert bytes(harness.call(app, "f1()").abi_return) == payload_abc
    assert bytes(harness.call(app, "f1s()").abi_return) == payload_abc

    # encodeWithSignature hashes the string exactly as supplied.
    sel_long = _evm_selector(
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, "
        "sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.")
    r = harness.call(app, "f2()")
    elems = [(2**256 - 1) - i for i in range(4)]
    expected_r = sel_long + evm_encode(["uint256[]"], [elems])
    assert bytes(r.abi_return[0]) == expected_r
    assert list(r.abi_return[1]) == [0, 0]

def test_abi_encode_with_signaturev2(harness):
    """abiEncodeDecode/contracts/abi_encode_with_signaturev2.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/abi_encode_with_signaturev2.sol")
    sel_f = _evm_selector("f(uint256)")
    assert bytes(harness.call(app, "f0()").abi_return) == sel_f

    payload_abc = sel_f + evm_encode(["string"], ["abc"])
    assert bytes(harness.call(app, "f1()").abi_return) == payload_abc
    assert bytes(harness.call(app, "f1s()").abi_return) == payload_abc

    # f2 uses a runtime signature string and a canonical dynamic array body.
    sel_long = _evm_selector(
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, "
        "sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.")
    elems = [(2**256 - 1) - i for i in range(4)]
    expected_r = sel_long + evm_encode(["uint256[]"], [elems])
    r = harness.call(app, "f2()")
    assert bytes(r.abi_return[0]) == expected_r
    assert list(r.abi_return[1]) == [0, 0]

    sel_s_b = _evm_selector("Lorem ipsum dolor sit ethereum........")
    s_b = "Lorem ipsum dolor sit ethereum........"
    f4_payload = sel_s_b + evm_encode(
        ["uint256", "(uint256,string,uint16)", "uint256"],
        [2**256 - 1, (0x1234567, s_b, 0x1234), 3])
    assert bytes(harness.call(app, "f4()").abi_return) == f4_payload

def test_contract_array(harness):
    """abiEncodeDecode/contracts/contract_array.sol"""
    from algosdk import encoding
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/contract_array.sol")
    payload = evm_encode(
        ["address[]"], [[v.to_bytes(20, "big") for v in (1, 2, 3)]])
    r = harness.call(app, "f(bytes)", payload)
    expected_addrs = [encoding.encode_address(v.to_bytes(32, "big")) for v in (1, 2, 3)]
    assert list(r.abi_return) == expected_addrs
    # g() returns abi.encode of the in-contract array (addresses 0x42, 0x21, 0x23).
    expected_g = evm_encode(
        ["address[]"], [[v.to_bytes(20, "big") for v in (0x42, 0x21, 0x23)]])
    assert bytes(harness.call(app, "g()").abi_return) == expected_g

def test_contract_array_v2(harness):
    """abiEncodeDecode/contracts/contract_array_v2.sol

    isoltest words convention: f(bytes) args are the raw EVM calldata words
    (packed by the harness); returns are compared in the EVM-words view.
    f returns C[] (decoded from the bytes); g returns abi.encode(C[3]).
    """
    from algosdk import encoding
    app = harness.compile_and_deploy('abiEncodeDecode/contracts/contract_array_v2.sol')
    def evm_caddr(vs):
        return evm_encode(
            ["address[]"], [[v.to_bytes(20, "big") for v in vs]])
    r = harness.call(app, 'f(bytes)', evm_caddr([1, 2, 3]))
    expected = [encoding.encode_address(v.to_bytes(32, "big")) for v in (1, 2, 3)]
    assert list(r.abi_return) == expected, r.abi_return
    addr20 = 0x0102030405060708090A0B0C0D0E0F1011121314
    r = harness.call(app, 'f(bytes)', evm_caddr([addr20]))
    assert list(r.abi_return) == [encoding.encode_address(addr20.to_bytes(32, "big"))]
    addr21 = addr20 << 8
    noncanonical = (
        (32).to_bytes(32, "big") + (1).to_bytes(32, "big")
        + addr21.to_bytes(32, "big"))
    assert harness.call(
        app, 'f(bytes)', noncanonical, expect_revert=True).reverted
    r = harness.call(app, 'g()')
    assert bytes(r.abi_return) == evm_caddr([0x42, 0x21, 0x23])

def test_offset_overflow_in_array_decoding(harness):
    """abiEncodeDecode/contracts/offset_overflow_in_array_decoding.sol"""
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/offset_overflow_in_array_decoding.sol")
    # test() -> FAILURE
    r = harness.call(app, "test()", expect_revert=True)
    assert r.reverted

def test_offset_overflow_in_array_decoding_2(harness):
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
    string field's conversion fell through to an incompatible representation);
    a struct with uint256[] and bytes fields; a mixed static/dynamic tuple; and
    bytes32+address.
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


def test_encode_address_array(harness):
    """abiEncodeDecode/contracts/encode_address_array.sol  (CUSTOM)

    abi.encode(address[]) uses canonical EVM dynamic-array layout. Internal
    Algorand accounts are adapted at this explicit boundary by taking their
    low 20 bytes and left-padding each EVM address word with twelve zero bytes.
    """
    from algosdk import account, encoding
    app = harness.compile_and_deploy("abiEncodeDecode/contracts/encode_address_array.sol")
    addrs = [account.generate_account()[1] for _ in range(3)]
    raw = [encoding.decode_address(a) for a in addrs]
    r = bytes(harness.call(app, "enc(address[])", addrs).abi_return)
    expected = evm_encode(["address[]"], [[value[-20:] for value in raw]])
    assert r == expected
