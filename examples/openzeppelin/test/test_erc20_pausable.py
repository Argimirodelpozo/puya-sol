import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key, fund_account
from algosdk import encoding


@pytest.fixture(scope="module")
def erc20_pausable(localnet, account):
    return deploy_contract(localnet, account, "ERC20PausableTest")


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def test_deploy(erc20_pausable):
    assert erc20_pausable.app_id > 0


def test_not_paused_initially(erc20_pausable):
    result = erc20_pausable.send.call(au.AppClientMethodCallParams(method="paused"))
    assert result.abi_return == 0


def test_mint_when_not_paused(erc20_pausable, account):
    addr = addr_bytes(account.address)
    box_key = mapping_box_key("_balances", addr)
    erc20_pausable.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr, 1000],
            box_references=[box_ref(erc20_pausable.app_id, box_key)],
        )
    )
    result = erc20_pausable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr],
            box_references=[box_ref(erc20_pausable.app_id, box_key)],
        )
    )
    assert result.abi_return == 1000


def test_pause(erc20_pausable):
    erc20_pausable.send.call(au.AppClientMethodCallParams(method="pause"))
    result = erc20_pausable.send.call(au.AppClientMethodCallParams(method="paused"))
    assert result.abi_return == 1


def test_mint_reverts_when_paused(erc20_pausable, account):
    addr = addr_bytes(account.address)
    box_key = mapping_box_key("_balances", addr)
    with pytest.raises(Exception):
        erc20_pausable.send.call(
            au.AppClientMethodCallParams(
                method="mint",
                args=[addr, 500],
                box_references=[box_ref(erc20_pausable.app_id, box_key)],
            )
        )


def test_transfer_reverts_when_paused(erc20_pausable, account, localnet):
    addr = addr_bytes(account.address)
    account2 = localnet.account.random()
    addr2 = addr_bytes(account2.address)
    box_src = mapping_box_key("_balances", addr)
    box_dst = mapping_box_key("_balances", addr2)
    with pytest.raises(Exception):
        erc20_pausable.send.call(
            au.AppClientMethodCallParams(
                method="transfer",
                args=[addr2, 100],
                box_references=[
                    box_ref(erc20_pausable.app_id, box_src),
                    box_ref(erc20_pausable.app_id, box_dst),
                ],
            )
        )


def test_burn_reverts_when_paused(erc20_pausable, account):
    addr = addr_bytes(account.address)
    box_key = mapping_box_key("_balances", addr)
    with pytest.raises(Exception):
        erc20_pausable.send.call(
            au.AppClientMethodCallParams(
                method="burn",
                args=[addr, 100],
                box_references=[box_ref(erc20_pausable.app_id, box_key)],
            )
        )


def test_unpause(erc20_pausable):
    erc20_pausable.send.call(au.AppClientMethodCallParams(method="unpause"))
    result = erc20_pausable.send.call(au.AppClientMethodCallParams(method="paused"))
    assert result.abi_return == 0


def test_mint_after_unpause(erc20_pausable, account):
    addr = addr_bytes(account.address)
    box_key = mapping_box_key("_balances", addr)
    erc20_pausable.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr, 500],
            box_references=[box_ref(erc20_pausable.app_id, box_key)],
        )
    )
    result = erc20_pausable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr],
            box_references=[box_ref(erc20_pausable.app_id, box_key)],
        )
    )
    assert result.abi_return == 1500


def test_transfer_after_unpause(erc20_pausable, account, localnet):
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)
    addr = addr_bytes(account.address)
    addr2 = addr_bytes(account2.address)
    box_src = mapping_box_key("_balances", addr)
    box_dst = mapping_box_key("_balances", addr2)
    erc20_pausable.send.call(
        au.AppClientMethodCallParams(
            method="transfer",
            args=[addr2, 200],
            box_references=[
                box_ref(erc20_pausable.app_id, box_src),
                box_ref(erc20_pausable.app_id, box_dst),
            ],
        )
    )
    result = erc20_pausable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr2],
            box_references=[box_ref(erc20_pausable.app_id, box_dst)],
        )
    )
    assert result.abi_return == 200
