"""CCTP v2 TypedMemView body-slice isolate (investigation → guard).

Real v2 messages are 376 bytes with a 148-byte header: the body is EXACTLY
228 bytes, so any error in the view length fails the messenger's
`len >= 228` check. Compiled --evm-storage-layout like the chainwide replay.
"""

from framework import as_int


MSG = bytes(range(256)) + bytes(range(120))  # 376 deterministic bytes
assert len(MSG) == 376


def test_tmv_body_slice_len(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/tmv_body_slice_len.sol",
        contract_name="TmvBodySliceLen",
        extra_args=["--evm-storage-layout"],
    )
    assert as_int(harness.call(app, "rawLen(bytes)", MSG).abi_return) == 376
    assert as_int(harness.call(app, "bodyLen(bytes)", MSG).abi_return) == 228
    assert as_int(harness.call(app, "clonedBodyLen(bytes)", MSG).abi_return) == 228
    assert as_int(harness.call(app, "hop(bytes)", MSG).abi_return) == 228


def test_tmv_body_data(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/tmv_body_slice_len.sol",
        contract_name="TmvBodyData",
        extra_args=["--evm-storage-layout"],
    )
    got = bytes(harness.call(app, "bodyFirstWord(bytes)", MSG).abi_return)
    assert got == MSG[148:180], got.hex()


def test_multivar_blob_repoint(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/asm_multivar_blob_repoint.sol",
        contract_name="MultiAssignBlobRepoint",
        extra_args=["--evm-storage-layout"],
    )
    m = bytes(range(1, 41))  # 40 bytes, nonzero first word
    ret = harness.call(app, "alloc(bytes)", m).abi_return
    len_plus_tag, first_word = as_int(ret[0]), bytes(ret[1])
    assert len_plus_tag == 47, len_plus_tag  # 40 + tag 7
    assert first_word == m[:32], first_word.hex()
