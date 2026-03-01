import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key, fund_account
from algosdk import encoding


@pytest.fixture(scope="module")
def erc20_burnable(localnet, account):
    return deploy_contract(localnet, account, "ERC20BurnableTest")


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def test_deploy(erc20_burnable):
    assert erc20_burnable.app_id > 0


def test_name(erc20_burnable):
    result = erc20_burnable.send.call(au.AppClientMethodCallParams(method="name"))
    assert result.abi_return == "BurnableToken"


def test_symbol(erc20_burnable):
    result = erc20_burnable.send.call(au.AppClientMethodCallParams(method="symbol"))
    assert result.abi_return == "BT"


def test_mint(erc20_burnable, account):
    addr = addr_bytes(account.address)
    box_key = mapping_box_key("_balances", addr)
    erc20_burnable.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr, 1000],
            box_references=[box_ref(erc20_burnable.app_id, box_key)],
        )
    )
    result = erc20_burnable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr],
            box_references=[box_ref(erc20_burnable.app_id, box_key)],
        )
    )
    assert result.abi_return == 1000


def test_burn(erc20_burnable, account):
    addr = addr_bytes(account.address)
    box_key = mapping_box_key("_balances", addr)
    # Burn 300 from sender's balance
    erc20_burnable.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[300],
            box_references=[box_ref(erc20_burnable.app_id, box_key)],
        )
    )
    result = erc20_burnable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr],
            box_references=[box_ref(erc20_burnable.app_id, box_key)],
        )
    )
    assert result.abi_return == 700


def test_burn_from_with_allowance(erc20_burnable, account, localnet):
    addr = addr_bytes(account.address)
    # Create second account
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)
    addr2 = addr_bytes(account2.address)

    # Mint to account2
    box_key2 = mapping_box_key("_balances", addr2)
    erc20_burnable.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr2, 500],
            box_references=[box_ref(erc20_burnable.app_id, box_key2)],
        )
    )

    # account2 approves account to spend 200
    allowance_key = mapping_box_key("_allowances", addr2, addr)
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=erc20_burnable.app_spec,
            app_id=erc20_burnable.app_id,
            default_sender=account2.address,
        )
    )
    client2.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[addr, 200],
            box_references=[box_ref(erc20_burnable.app_id, allowance_key)],
        )
    )

    # account calls burnFrom to burn 150 from account2
    erc20_burnable.send.call(
        au.AppClientMethodCallParams(
            method="burnFrom",
            args=[addr2, 150],
            box_references=[
                box_ref(erc20_burnable.app_id, box_key2),
                box_ref(erc20_burnable.app_id, allowance_key),
            ],
        )
    )

    # Check account2 balance reduced
    result = erc20_burnable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr2],
            box_references=[box_ref(erc20_burnable.app_id, box_key2)],
        )
    )
    assert result.abi_return == 350

    # Check allowance reduced
    result = erc20_burnable.send.call(
        au.AppClientMethodCallParams(
            method="allowance",
            args=[addr2, addr],
            box_references=[box_ref(erc20_burnable.app_id, allowance_key)],
        )
    )
    assert result.abi_return == 50


def test_burn_from_insufficient_allowance(erc20_burnable, account, localnet):
    addr = addr_bytes(account.address)
    account3 = localnet.account.random()
    fund_account(localnet, account, account3)
    addr3 = addr_bytes(account3.address)

    box_key3 = mapping_box_key("_balances", addr3)
    erc20_burnable.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr3, 100],
            box_references=[box_ref(erc20_burnable.app_id, box_key3)],
        )
    )

    allowance_key = mapping_box_key("_allowances", addr3, addr)
    with pytest.raises(Exception):
        erc20_burnable.send.call(
            au.AppClientMethodCallParams(
                method="burnFrom",
                args=[addr3, 50],
                box_references=[
                    box_ref(erc20_burnable.app_id, box_key3),
                    box_ref(erc20_burnable.app_id, allowance_key),
                ],
            )
        )


def test_total_supply_after_burn(erc20_burnable):
    result = erc20_burnable.send.call(
        au.AppClientMethodCallParams(method="totalSupply")
    )
    # 1000 (initial) - 300 (burn) + 500 (account2) - 150 (burnFrom) + 100 (account3) = 1150
    assert result.abi_return == 1150
