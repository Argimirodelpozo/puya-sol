"""CCTP v2 TypedMemView body-slice isolate (investigation → guard).

Real v2 messages are 376 bytes with a 148-byte header: the body is EXACTLY
228 bytes, so any error in the view length fails the messenger's
`len >= 228` check. Compiled --evm-layout like the chainwide replay.
"""

from framework import as_int


MSG = bytes(range(256)) + bytes(range(120))  # 376 deterministic bytes
assert len(MSG) == 376


def test_tmv_body_slice_len(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/tmv_body_slice_len.sol",
        contract_name="TmvBodySliceLen",
        extra_args=["--evm-layout"],
    )
    assert as_int(harness.call(app, "rawLen(bytes)", MSG).abi_return) == 376
    assert as_int(harness.call(app, "bodyLen(bytes)", MSG).abi_return) == 228
    assert as_int(harness.call(app, "clonedBodyLen(bytes)", MSG).abi_return) == 228
    assert as_int(harness.call(app, "hop(bytes)", MSG).abi_return) == 228


def test_tmv_body_data(harness):
    app = harness.compile_and_deploy(
        "puyasolRegression/contracts/tmv_body_slice_len.sol",
        contract_name="TmvBodyData",
        extra_args=["--evm-layout"],
    )
    got = bytes(harness.call(app, "bodyFirstWord(bytes)", MSG).abi_return)
    assert got == MSG[148:180], got.hex()
