from Crypto.Hash import keccak
from eth_abi import decode, encode


SOURCE = "puyasolRegression/contracts/evm_contract_abi.sol"
RETURN_PREFIX = bytes.fromhex("151f7c75")


def _selector(signature: str) -> bytes:
    h = keccak.new(digest_bits=256)
    h.update(signature.encode())
    return h.digest()[:4]


def _call(
        harness, app, signature: str, types: list[str], values: list,
        **call_kwargs):
    body = encode(types, values)
    call_kwargs.setdefault("extra_fee", 20_000)
    result = harness.call_raw(
        app, _selector(signature), extra_args=(body,), **call_kwargs)
    assert not result.reverted, result.fail_message
    assert result.logs and result.logs[-1].startswith(RETURN_PREFIX)
    return result.logs[-1][len(RETURN_PREFIX):]


def test_contract_abi_default_remains_arc4(harness):
    artifacts = harness.compile(SOURCE)
    app = harness.deploy(artifacts, "EvmContractAbi")
    result = harness.call(app, "ping()")
    assert not result.reverted
    assert result.abi_return == 77


def test_evm_contract_abi_scalar_entry_and_return(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--contract-abi", "evm"])
    app = harness.deploy(artifacts, "EvmContractAbi")
    payload = _call(
        harness, app, "scalars(uint16,bool,bytes3)",
        ["uint16", "bool", "bytes3"], [41, True, b"tag"])
    assert decode(["uint16", "bool", "bytes3"], payload) == (
        42, False, b"tag")


def test_evm_contract_abi_signed_values_convert_per_element(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--contract-abi", "evm"])
    app = harness.deploy(artifacts, "EvmContractAbi")
    payload = _call(
        harness, app, "signed(int16,int128)", ["int16", "int128"],
        [-7, -(1 << 100)])
    assert decode(["int16", "int128"], payload) == (
        -8, -(1 << 100) + 2)


def test_evm_contract_abi_zero_argument_route(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--contract-abi", "evm"])
    app = harness.deploy(artifacts, "EvmContractAbi")
    payload = _call(harness, app, "ping()", [], [])
    assert decode(["uint256"], payload)[0] == 77


def test_evm_contract_abi_nested_arrays_are_recursive(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--contract-abi", "evm"])
    app = harness.deploy(artifacts, "EvmContractAbi")
    value = [[[1, 2], [3]], [], [[4, 5, 6]]]
    payload = _call(
        harness, app, "nested(uint32[][][])", ["uint32[][][]"], [value])
    assert decode(["uint32[][][]"], payload)[0] == (
        ((1, 2), (3,)), (), ((4, 5, 6),))


def test_evm_contract_abi_dynamic_struct_recurses(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--contract-abi", "evm"])
    app = harness.deploy(artifacts, "EvmContractAbi")
    record_type = "(uint16,string,uint32[])"
    payload = _call(
        harness, app, f"record({record_type})", [record_type],
        [(7, "recursive", [1, 2, 3])])
    assert decode([record_type], payload)[0] == (
        8, "recursive", (1, 2, 3))


def test_evm_entry_rejects_noncanonical_words_and_offsets(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--contract-abi", "evm"])
    app = harness.deploy(artifacts, "EvmContractAbi")
    scalar_selector = _selector("scalars(uint16,bool,bytes3)")
    bytes3_word = b"tag" + bytes(29)

    bad_uint_padding = (1 << 16 | 41).to_bytes(32, "big")
    body = bad_uint_padding + (1).to_bytes(32, "big") + bytes3_word
    assert harness.call_raw(
        app, scalar_selector, extra_args=(body,), expect_revert=True).reverted

    body = (41).to_bytes(32, "big") + (2).to_bytes(32, "big") + bytes3_word
    assert harness.call_raw(
        app, scalar_selector, extra_args=(body,), expect_revert=True).reverted

    bad_bytes3 = b"tag" + bytes(28) + b"\x01"
    body = (41).to_bytes(32, "big") + (1).to_bytes(32, "big") + bad_bytes3
    assert harness.call_raw(
        app, scalar_selector, extra_args=(body,), expect_revert=True).reverted

    assert harness.call_raw(
        app, _selector("nested(uint32[][][])"),
        extra_args=((31).to_bytes(32, "big"),), expect_revert=True).reverted


def test_solidity_abi_intrinsics_stay_evm_under_evm_entry(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--contract-abi", "evm"])
    app = harness.deploy(artifacts, "EvmContractAbi")
    payload = _call(
        harness, app, "codec(uint16,bytes)", ["uint16", "bytes"],
        [7, b"hello"])
    inner = decode(["bytes"], payload)[0]
    assert decode(["uint16", "bytes"], inner) == (7, b"hello")


def test_explicit_arc4_intrinsics_round_trip_under_evm_entry(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--contract-abi", "evm"])
    app = harness.deploy(artifacts, "EvmContractAbi")
    payload = _call(
        harness, app, "arc4Codec(uint16,bytes)", ["uint16", "bytes"],
        [9, b"arc4 stays explicit"])
    assert decode(["uint16", "bytes"], payload) == (
        9, b"arc4 stays explicit")


def test_evm_profile_canonicalizes_ambient_sender_identity(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--contract-abi", "evm"])
    app = harness.deploy(artifacts, "EvmContractAbi")
    from algosdk import encoding
    sender20 = encoding.decode_address(harness.localnet.account.address)[-20:]
    expected = "0x" + sender20.hex()
    payload = _call(
        harness, app, "senderIdentity(address)", ["address"], [expected])
    high_level, assembly_level, returned = decode(
        ["bool", "bool", "address"], payload)
    assert high_level is True
    assert assembly_level is True
    assert returned.lower() == expected


def test_evm_profile_fallback_receives_full_solidity_calldata(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--contract-abi", "evm"])
    app = harness.deploy(artifacts, "EvmContractAbi")
    selector = bytes.fromhex("deadbeef")
    body = bytes.fromhex("0102030405")
    result = harness.call_raw(app, selector, extra_args=(body,))
    assert not result.reverted, result.fail_message
    assert result.logs[-1] == RETURN_PREFIX + selector + body


def test_evm_profile_typed_outbound_call_uses_same_boundary(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--contract-abi", "evm"])
    target = harness.deploy(artifacts, "EvmTarget")
    caller = harness.deploy(artifacts, "EvmCaller")
    evm_address = "0x" + (bytes(12) + target.app_id.to_bytes(8, "big")).hex()
    payload = _call(
        harness, caller, "forward(address,uint16)", ["address", "uint16"],
        [evm_address, 41])
    assert decode(["uint16"], payload)[0] == 42


def test_evm_profile_child_constructor_uses_canonical_body(harness):
    artifacts = harness.compile(SOURCE, extra_args=["--contract-abi", "evm"])
    factory = harness.deploy(artifacts, "EvmFactory")
    payload = _call(
        harness, factory, "makeAndRead(uint16)", ["uint16"], [42],
        extra_fee=40_000)
    assert decode(["uint16"], payload)[0] == 42
