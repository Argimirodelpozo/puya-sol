import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key, fund_account
from algosdk import encoding


@pytest.fixture(scope="module")
def erc1155_pausable(localnet, account):
    return deploy_contract(localnet, account, "ERC1155PausableTest")


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def token_id_bytes(token_id: int) -> bytes:
    return token_id.to_bytes(64, "big")


def test_deploy(erc1155_pausable):
    assert erc1155_pausable.app_id > 0


def test_not_paused_initially(erc1155_pausable):
    result = erc1155_pausable.send.call(au.AppClientMethodCallParams(method="paused"))
    assert result.abi_return == 0


def test_mint_when_not_paused(erc1155_pausable, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(1)
    box_key = mapping_box_key("_balances", tid, addr)
    erc1155_pausable.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr, 1, 100],
            box_references=[box_ref(erc1155_pausable.app_id, box_key)],
        )
    )
    result = erc1155_pausable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr, 1],
            box_references=[box_ref(erc1155_pausable.app_id, box_key)],
        )
    )
    assert result.abi_return == 100


def test_pause(erc1155_pausable):
    erc1155_pausable.send.call(au.AppClientMethodCallParams(method="pause"))
    result = erc1155_pausable.send.call(au.AppClientMethodCallParams(method="paused"))
    assert result.abi_return == 1


def test_mint_reverts_when_paused(erc1155_pausable, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(2)
    box_key = mapping_box_key("_balances", tid, addr)
    with pytest.raises(Exception):
        erc1155_pausable.send.call(
            au.AppClientMethodCallParams(
                method="mint",
                args=[addr, 2, 50],
                box_references=[box_ref(erc1155_pausable.app_id, box_key)],
            )
        )


def test_burn_reverts_when_paused(erc1155_pausable, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(1)
    box_key = mapping_box_key("_balances", tid, addr)
    with pytest.raises(Exception):
        erc1155_pausable.send.call(
            au.AppClientMethodCallParams(
                method="burn",
                args=[addr, 1, 10],
                box_references=[box_ref(erc1155_pausable.app_id, box_key)],
            )
        )


def test_unpause(erc1155_pausable):
    erc1155_pausable.send.call(au.AppClientMethodCallParams(method="unpause"))
    result = erc1155_pausable.send.call(au.AppClientMethodCallParams(method="paused"))
    assert result.abi_return == 0


def test_mint_after_unpause(erc1155_pausable, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(2)
    box_key = mapping_box_key("_balances", tid, addr)
    erc1155_pausable.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr, 2, 50],
            box_references=[box_ref(erc1155_pausable.app_id, box_key)],
        )
    )
    result = erc1155_pausable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr, 2],
            box_references=[box_ref(erc1155_pausable.app_id, box_key)],
        )
    )
    assert result.abi_return == 50


def test_burn_after_unpause(erc1155_pausable, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(1)
    box_key = mapping_box_key("_balances", tid, addr)
    erc1155_pausable.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[addr, 1, 30],
            box_references=[box_ref(erc1155_pausable.app_id, box_key)],
        )
    )
    result = erc1155_pausable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr, 1],
            box_references=[box_ref(erc1155_pausable.app_id, box_key)],
        )
    )
    assert result.abi_return == 70


def test_transfer_after_unpause(erc1155_pausable, account, localnet):
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)
    addr = addr_bytes(account.address)
    addr2 = addr_bytes(account2.address)
    tid = token_id_bytes(2)
    box_src = mapping_box_key("_balances", tid, addr)
    box_dst = mapping_box_key("_balances", tid, addr2)

    erc1155_pausable.send.call(
        au.AppClientMethodCallParams(
            method="safeTransferFrom",
            args=[addr, addr2, 2, 20, b""],
            box_references=[
                box_ref(erc1155_pausable.app_id, box_src),
                box_ref(erc1155_pausable.app_id, box_dst),
            ],
        )
    )
    result = erc1155_pausable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr2, 2],
            box_references=[box_ref(erc1155_pausable.app_id, box_dst)],
        )
    )
    assert result.abi_return == 20
