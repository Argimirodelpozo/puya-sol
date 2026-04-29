"""
Vault behavioral tests.
Tests deposit, withdraw, share calculations, and preview functions.
"""
import pytest
import algokit_utils as au
from algosdk import encoding
from conftest import deploy_contract, mapping_box_key


def share_key(addr):
    return mapping_box_key("_shareBalance", encoding.decode_address(addr))


def asset_key(addr):
    return mapping_box_key("_assetDeposited", encoding.decode_address(addr))


def index_key(addr):
    return mapping_box_key("_depositorIndex", encoding.decode_address(addr))


def depositor_boxes(addr):
    return [
        au.BoxReference(app_id=0, name=share_key(addr)),
        au.BoxReference(app_id=0, name=asset_key(addr)),
        au.BoxReference(app_id=0, name=index_key(addr)),
    ]


@pytest.fixture(scope="module")
def vault(localnet, account):
    return deploy_contract(localnet, account, "VaultTest")


def test_deploy(vault):
    assert vault.app_id > 0


def test_admin(vault, account):
    result = vault.send.call(
        au.AppClientMethodCallParams(method="getAdmin")
    )
    assert result.abi_return == account.address


def test_init_and_deposit(vault, account):
    boxes = depositor_boxes(account.address)
    vault.send.call(
        au.AppClientMethodCallParams(
            method="initDepositor",
            args=[account.address],
            box_references=boxes,
        )
    )
    # First deposit: 1:1 shares (totalAssets == 0)
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="deposit",
            args=[account.address, 10000],
            box_references=boxes,
        )
    )
    assert result.abi_return == 10000  # 1:1 shares


def test_share_balance(vault, account):
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="shareBalanceOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=share_key(account.address))],
        )
    )
    assert result.abi_return == 10000


def test_total_assets(vault):
    result = vault.send.call(
        au.AppClientMethodCallParams(method="getTotalAssets")
    )
    assert result.abi_return == 10000


def test_total_shares(vault):
    result = vault.send.call(
        au.AppClientMethodCallParams(method="getTotalShares")
    )
    assert result.abi_return == 10000


def test_depositor_count(vault):
    result = vault.send.call(
        au.AppClientMethodCallParams(method="getDepositorCount")
    )
    assert result.abi_return == 1


def test_preview_deposit(vault):
    # totalAssets=10000, totalShares=10000 → 5000 assets = 5000 shares
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="previewDeposit",
            args=[5000],
        )
    )
    assert result.abi_return == 5000


def test_preview_withdraw(vault):
    # 5000 shares * 10000 assets / 10000 shares = 5000 assets
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="previewWithdraw",
            args=[5000],
        )
    )
    assert result.abi_return == 5000


def test_withdraw(vault, account):
    boxes = depositor_boxes(account.address)
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="withdraw",
            args=[account.address, 3000],  # 3000 shares
            box_references=boxes,
        )
    )
    assert result.abi_return == 3000  # 3000 assets


def test_share_balance_after(vault, account):
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="shareBalanceOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=share_key(account.address))],
            note=b"sb2",
        )
    )
    assert result.abi_return == 7000


def test_assets_of(vault, account):
    # 7000 shares * 7000 totalAssets / 7000 totalShares = 7000
    result = vault.send.call(
        au.AppClientMethodCallParams(
            method="assetsOf",
            args=[account.address],
            box_references=[au.BoxReference(app_id=0, name=share_key(account.address))],
        )
    )
    assert result.abi_return == 7000
