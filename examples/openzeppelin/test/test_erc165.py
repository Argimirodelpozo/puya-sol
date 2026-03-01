"""
ERC165 behavioral tests.
Tests interface ID computation and supportsInterface pattern.
"""
import pytest
import algokit_utils as au
from conftest import deploy_contract


@pytest.fixture(scope="module")
def erc165(localnet, account):
    return deploy_contract(localnet, account, "ERC165Test")


def test_deploy(erc165):
    assert erc165.app_id > 0


def test_get_erc165_interface_id(erc165):
    """ERC165 interface ID should be 0x01ffc9a7."""
    result = erc165.send.call(
        au.AppClientMethodCallParams(method="getERC165InterfaceId")
    )
    val = bytes(result.abi_return)
    assert len(val) == 4
    assert val == bytes.fromhex("01ffc9a7")


def test_get_erc20_interface_id(erc165):
    """ERC20 interface ID should be 0x36372b07."""
    result = erc165.send.call(
        au.AppClientMethodCallParams(method="getERC20InterfaceId")
    )
    val = bytes(result.abi_return)
    assert len(val) == 4
    assert val == bytes.fromhex("36372b07")


def test_get_erc721_interface_id(erc165):
    """ERC721 interface ID (just balanceOf + ownerOf) should be non-zero."""
    result = erc165.send.call(
        au.AppClientMethodCallParams(method="getERC721InterfaceId")
    )
    val = bytes(result.abi_return)
    assert len(val) == 4
    assert val != b'\x00' * 4


def test_supports_erc165(erc165):
    """Contract supports IERC165."""
    erc165_id = bytes.fromhex("01ffc9a7")
    result = erc165.send.call(
        au.AppClientMethodCallParams(
            method="supportsInterface",
            args=[erc165_id],
        )
    )
    assert result.abi_return is True


def test_does_not_support_random(erc165):
    """Contract does not support a random interface."""
    random_id = bytes.fromhex("deadbeef")
    result = erc165.send.call(
        au.AppClientMethodCallParams(
            method="supportsInterface",
            args=[random_id],
        )
    )
    assert result.abi_return is False


def test_interface_ids_equal(erc165):
    """Same bytes4 values are equal."""
    val = bytes.fromhex("01ffc9a7")
    result = erc165.send.call(
        au.AppClientMethodCallParams(
            method="interfaceIdsEqual",
            args=[val, val],
        )
    )
    assert result.abi_return is True


def test_interface_ids_not_equal(erc165):
    """Different bytes4 values are not equal."""
    val1 = bytes.fromhex("01ffc9a7")
    val2 = bytes.fromhex("deadbeef")
    result = erc165.send.call(
        au.AppClientMethodCallParams(
            method="interfaceIdsEqual",
            args=[val1, val2],
        )
    )
    assert result.abi_return is False
