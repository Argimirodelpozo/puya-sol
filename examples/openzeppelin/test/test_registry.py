"""
Registry behavioral tests.
Tests name registration, transfer, primary name, renewal, and release.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def name_owner_key(name_hash):
    return mapping_box_key("_nameToOwner", name_hash.to_bytes(64, "big"))


def name_expiry_key(name_hash):
    return mapping_box_key("_nameToExpiry", name_hash.to_bytes(64, "big"))


def primary_key(addr):
    return mapping_box_key("_primaryName", encoding.decode_address(addr))


@pytest.fixture(scope="module")
def registry(localnet, account):
    return deploy_contract(localnet, account, "RegistryTest")


def test_deploy(registry):
    assert registry.app_id > 0


def test_owner(registry, account):
    result = registry.send.call(
        au.AppClientMethodCallParams(method="owner")
    )
    assert result.abi_return == account.address


def test_initial_count(registry):
    result = registry.send.call(
        au.AppClientMethodCallParams(method="registrationCount")
    )
    assert result.abi_return == 0


def test_initial_fee(registry):
    result = registry.send.call(
        au.AppClientMethodCallParams(method="registrationFee")
    )
    assert result.abi_return == 100


def test_register(registry, account):
    name_hash = 42
    boxes = [
        au.BoxReference(app_id=0, name=name_owner_key(name_hash)),
        au.BoxReference(app_id=0, name=name_expiry_key(name_hash)),
    ]
    registry.send.call(
        au.AppClientMethodCallParams(
            method="register",
            args=[name_hash, account.address, 365],
            box_references=boxes,
        )
    )


def test_count_after_register(registry):
    result = registry.send.call(
        au.AppClientMethodCallParams(method="registrationCount", note=b"c1")
    )
    assert result.abi_return == 1


def test_owner_of_name(registry, account):
    result = registry.send.call(
        au.AppClientMethodCallParams(
            method="ownerOfName",
            args=[42],
            box_references=[au.BoxReference(app_id=0, name=name_owner_key(42))],
        )
    )
    assert result.abi_return == account.address


def test_expiry(registry):
    result = registry.send.call(
        au.AppClientMethodCallParams(
            method="expiryOf",
            args=[42],
            box_references=[au.BoxReference(app_id=0, name=name_expiry_key(42))],
        )
    )
    assert result.abi_return == 365


def test_set_primary_name(registry, account):
    boxes = [
        au.BoxReference(app_id=0, name=name_owner_key(42)),
        au.BoxReference(app_id=0, name=primary_key(account.address)),
    ]
    registry.send.call(
        au.AppClientMethodCallParams(
            method="setPrimaryName",
            args=[account.address, 42],
            box_references=boxes,
        )
    )


def test_primary_name(registry, account):
    result = registry.send.call(
        au.AppClientMethodCallParams(
            method="primaryName",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=primary_key(account.address))],
        )
    )
    assert result.abi_return == 42


def test_renew_name(registry):
    boxes = [
        au.BoxReference(app_id=0, name=name_owner_key(42)),
        au.BoxReference(app_id=0, name=name_expiry_key(42)),
    ]
    registry.send.call(
        au.AppClientMethodCallParams(
            method="renewName",
            args=[42, 100],
            box_references=boxes,
        )
    )


def test_expiry_after_renew(registry):
    result = registry.send.call(
        au.AppClientMethodCallParams(
            method="expiryOf",
            args=[42],
            box_references=[au.BoxReference(app_id=0, name=name_expiry_key(42))],
            note=b"exp2",
        )
    )
    assert result.abi_return == 465


def test_release(registry, account):
    boxes = [
        au.BoxReference(app_id=0, name=name_owner_key(42)),
        au.BoxReference(app_id=0, name=name_expiry_key(42)),
        au.BoxReference(app_id=0, name=primary_key(account.address)),
    ]
    registry.send.call(
        au.AppClientMethodCallParams(
            method="releaseName",
            args=[42],
            box_references=boxes,
        )
    )


def test_count_after_release(registry):
    result = registry.send.call(
        au.AppClientMethodCallParams(method="registrationCount", note=b"c2")
    )
    assert result.abi_return == 0
