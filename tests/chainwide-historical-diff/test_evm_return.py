"""Both AVM replay lanes use the same ABI-directed accepted-call decoder."""

import pytest
from eth_abi import encode
from eth_abi.exceptions import DecodingError

from avm_leg import decode_evm_return

MAGIC = bytes.fromhex("151f7c75")


@pytest.mark.parametrize("logs", [[], [b"event"], [MAGIC], [MAGIC + b"assembly return"]])
def test_explicit_void_may_omit_return_record(logs):
    before = list(logs)
    assert decode_evm_return("f()", {"outputs": []}, logs) is None
    assert logs == before  # events and explicit assembly returns remain available


@pytest.mark.parametrize("fn", [None, {}, {"outputs": None}, {"outputs": ()}])
def test_missing_metadata_cannot_be_inferred_void(fn):
    with pytest.raises(ValueError, match="no declared ABI outputs"):
        decode_evm_return("f()", fn, [])


def test_nonvoid_still_requires_payload():
    with pytest.raises(ValueError, match="no structured payload"):
        decode_evm_return("f()", {"outputs": [{"type": "uint256"}]}, [b"event"])


def test_nonvoid_malformed_payload_still_fails():
    with pytest.raises(DecodingError):
        decode_evm_return("f()", {"outputs": [{"type": "uint256"}]}, [MAGIC])


def test_last_payload_wins_and_events_are_not_returns():
    logs = [MAGIC + encode(["uint256"], [1]), b"event",
            MAGIC + encode(["uint256"], [42]), b"last event"]
    assert decode_evm_return("f()", {"outputs": [{"type": "uint256"}]}, logs) == 42


def test_tuple_array_outputs_keep_their_abi_shape():
    fn = {"outputs": [{"type": "tuple[]", "components": [
        {"type": "uint256"}, {"type": "bytes"}]}, {"type": "bool"}]}
    payload = encode(["(uint256,bytes)[]", "bool"], [[(42, b"proof")], True])
    assert decode_evm_return("f()", fn, [MAGIC + payload]) == (((42, b"proof"),), True)
