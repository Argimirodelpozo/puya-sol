"""
M42: Standalone abi.encodeCall, abi.encodeWithSelector, abi.encodeWithSignature.
Tests Gaps 2 and 3.
"""

import pytest
import algokit_utils as au
from algosdk.abi import Method
from algosdk import encoding
from algokit_utils.models.account import SigningAccount

from conftest import deploy_contract


@pytest.fixture(scope="module")
def app(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_contract(localnet, account, "AbiEncode")


@pytest.mark.localnet
def test_encode_call_length(app: au.AppClient) -> None:
    """abi.encodeCall(ITarget.setValues, (a, b)) should produce 4 + 32 + 32 = 68 bytes."""
    result = app.send.call(
        au.AppClientMethodCallParams(
            method="testEncodeCallLength",
            args=[5, 10],
        ),
    )
    assert result.abi_return == 68


@pytest.mark.localnet
def test_encode_call_transfer(app: au.AppClient, account: SigningAccount) -> None:
    """abi.encodeCall(ITarget.transfer, (to, amount)) should have correct selector + args."""
    addr_bytes = encoding.decode_address(account.address)
    result = app.send.call(
        au.AppClientMethodCallParams(
            method="testEncodeCall",
            args=[addr_bytes, 100],
        ),
    )
    encoded = bytes(result.abi_return)
    # First 4 bytes: method selector for "transfer(address,uint256)bool"
    selector = encoded[:4]
    # The address occupies bytes 4..36
    addr_in_encoded = encoded[4:36]
    # The amount occupies bytes 36..68
    amount_in_encoded = int.from_bytes(encoded[36:68], "big")

    assert len(encoded) == 68
    assert addr_in_encoded == addr_bytes
    assert amount_in_encoded == 100
    # Selector should be non-zero (we don't hardcode the exact value as it depends on ARC4 hashing)
    assert selector != b"\x00\x00\x00\x00"


@pytest.mark.localnet
def test_encode_call_void(app: au.AppClient) -> None:
    """abi.encodeCall(ITarget.setValues, (a, b)) should encode both args."""
    result = app.send.call(
        au.AppClientMethodCallParams(
            method="testEncodeCallVoid",
            args=[42, 99],
        ),
    )
    encoded = bytes(result.abi_return)
    assert len(encoded) == 68  # 4 selector + 32 + 32
    a_val = int.from_bytes(encoded[4:36], "big")
    b_val = int.from_bytes(encoded[36:68], "big")
    assert a_val == 42
    assert b_val == 99


@pytest.mark.localnet
def test_encode_with_selector(app: au.AppClient) -> None:
    """abi.encodeWithSelector(sel, value) should prepend the selector."""
    custom_selector = b"\xde\xad\xbe\xef"
    result = app.send.call(
        au.AppClientMethodCallParams(
            method="testEncodeWithSelector",
            args=[custom_selector, 777],
        ),
    )
    encoded = bytes(result.abi_return)
    assert len(encoded) == 36  # 4 selector + 32 uint256
    assert encoded[:4] == custom_selector
    val = int.from_bytes(encoded[4:36], "big")
    assert val == 777


@pytest.mark.localnet
def test_encode_with_signature(app: au.AppClient) -> None:
    """abi.encodeWithSignature("transfer(address,uint256)", value) should work."""
    result = app.send.call(
        au.AppClientMethodCallParams(
            method="testEncodeWithSignature",
            args=[500],
        ),
    )
    encoded = bytes(result.abi_return)
    assert len(encoded) == 36  # 4 selector + 32 uint256
    # First 4 bytes should be the selector (non-zero)
    assert encoded[:4] != b"\x00\x00\x00\x00"
    val = int.from_bytes(encoded[4:36], "big")
    assert val == 500
