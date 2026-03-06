"""
AAVE V4 SpokeConfigurator tests.
"""

import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract


ZERO_ADDR = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ"


@pytest.fixture(scope="module")
def configurator(localnet, account):
    authority = encoding.decode_address(account.address)
    return deploy_contract(
        localnet, account, "SpokeConfigurator",
        app_args=[authority],
        extra_pages=1,
    )


_call_counter = 0


def _call(client, method, *args):
    global _call_counter
    _call_counter += 1
    note = f"sc_{_call_counter}".encode()
    result = client.send.call(
        au.AppClientMethodCallParams(method=method, args=list(args), note=note)
    )
    return result.abi_return


def test_deploy(configurator):
    assert configurator.app_id > 0


def test_authority(configurator, account):
    result = _call(configurator, "authority")
    assert result == account.address


def test_isConsumingScheduledOp(configurator):
    result = _call(configurator, "isConsumingScheduledOp")
    assert result == [0, 0, 0, 0] or result == b'\x00\x00\x00\x00'


def test_setAuthority_requires_authority(configurator, account):
    """setAuthority requires the caller to be the authority contract."""
    with pytest.raises(Exception, match="AccessManagedInvalidAuthority"):
        _call(configurator, "setAuthority", account.address)
