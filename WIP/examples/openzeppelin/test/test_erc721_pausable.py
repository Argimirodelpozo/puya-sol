import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key, fund_account
from algosdk import encoding


@pytest.fixture(scope="module")
def erc721_pausable(localnet, account):
    return deploy_contract(localnet, account, "ERC721PausableTest")


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def token_id_bytes(token_id: int) -> bytes:
    return token_id.to_bytes(64, "big")


ZERO_ADDR = b"\x00" * 32


def test_deploy(erc721_pausable):
    assert erc721_pausable.app_id > 0


def test_not_paused_initially(erc721_pausable):
    result = erc721_pausable.send.call(au.AppClientMethodCallParams(method="paused"))
    assert result.abi_return == 0


def test_mint_when_not_paused(erc721_pausable, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(1)
    owner_key = mapping_box_key("_owners", tid)
    balance_key = mapping_box_key("_balances", addr)
    erc721_pausable.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr, 1],
            box_references=[
                box_ref(erc721_pausable.app_id, owner_key),
                box_ref(erc721_pausable.app_id, balance_key),
            ],
        )
    )
    result = erc721_pausable.send.call(
        au.AppClientMethodCallParams(
            method="ownerOf",
            args=[1],
            box_references=[box_ref(erc721_pausable.app_id, owner_key)],
        )
    )
    assert result.abi_return == account.address


def test_pause(erc721_pausable):
    erc721_pausable.send.call(au.AppClientMethodCallParams(method="pause"))
    result = erc721_pausable.send.call(au.AppClientMethodCallParams(method="paused"))
    assert result.abi_return == 1


def test_mint_reverts_when_paused(erc721_pausable, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(2)
    owner_key = mapping_box_key("_owners", tid)
    balance_key = mapping_box_key("_balances", addr)
    with pytest.raises(Exception):
        erc721_pausable.send.call(
            au.AppClientMethodCallParams(
                method="mint",
                args=[addr, 2],
                box_references=[
                    box_ref(erc721_pausable.app_id, owner_key),
                    box_ref(erc721_pausable.app_id, balance_key),
                ],
            )
        )


def test_transfer_reverts_when_paused(erc721_pausable, account, localnet):
    account2 = localnet.account.random()
    addr2 = addr_bytes(account2.address)
    addr = addr_bytes(account.address)
    tid = token_id_bytes(1)
    owner_key = mapping_box_key("_owners", tid)
    balance_src = mapping_box_key("_balances", addr)
    balance_dst = mapping_box_key("_balances", addr2)
    approval_key = mapping_box_key("_tokenApprovals", tid)
    with pytest.raises(Exception):
        erc721_pausable.send.call(
            au.AppClientMethodCallParams(
                method="transferFrom",
                args=[addr, addr2, 1],
                box_references=[
                    box_ref(erc721_pausable.app_id, owner_key),
                    box_ref(erc721_pausable.app_id, balance_src),
                    box_ref(erc721_pausable.app_id, balance_dst),
                    box_ref(erc721_pausable.app_id, approval_key),
                ],
            )
        )


def test_burn_reverts_when_paused(erc721_pausable, account):
    tid = token_id_bytes(1)
    owner_key = mapping_box_key("_owners", tid)
    balance_key = mapping_box_key("_balances", addr_bytes(account.address))
    approval_key = mapping_box_key("_tokenApprovals", tid)
    with pytest.raises(Exception):
        erc721_pausable.send.call(
            au.AppClientMethodCallParams(
                method="burn",
                args=[1],
                box_references=[
                    box_ref(erc721_pausable.app_id, owner_key),
                    box_ref(erc721_pausable.app_id, balance_key),
                    box_ref(erc721_pausable.app_id, approval_key),
                ],
            )
        )


def test_unpause(erc721_pausable):
    erc721_pausable.send.call(au.AppClientMethodCallParams(method="unpause"))
    result = erc721_pausable.send.call(au.AppClientMethodCallParams(method="paused"))
    assert result.abi_return == 0


def test_mint_after_unpause(erc721_pausable, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(2)
    owner_key = mapping_box_key("_owners", tid)
    balance_key = mapping_box_key("_balances", addr)
    erc721_pausable.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr, 2],
            box_references=[
                box_ref(erc721_pausable.app_id, owner_key),
                box_ref(erc721_pausable.app_id, balance_key),
            ],
        )
    )
    result = erc721_pausable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr],
            box_references=[box_ref(erc721_pausable.app_id, balance_key)],
        )
    )
    assert result.abi_return == 2


def test_transfer_after_unpause(erc721_pausable, account, localnet):
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)
    addr = addr_bytes(account.address)
    addr2 = addr_bytes(account2.address)
    tid = token_id_bytes(2)
    owner_key = mapping_box_key("_owners", tid)
    balance_src = mapping_box_key("_balances", addr)
    balance_dst = mapping_box_key("_balances", addr2)
    approval_key = mapping_box_key("_tokenApprovals", tid)

    erc721_pausable.send.call(
        au.AppClientMethodCallParams(
            method="transferFrom",
            args=[addr, addr2, 2],
            box_references=[
                box_ref(erc721_pausable.app_id, owner_key),
                box_ref(erc721_pausable.app_id, balance_src),
                box_ref(erc721_pausable.app_id, balance_dst),
                box_ref(erc721_pausable.app_id, approval_key),
            ],
        )
    )
    result = erc721_pausable.send.call(
        au.AppClientMethodCallParams(
            method="ownerOf",
            args=[2],
            box_references=[box_ref(erc721_pausable.app_id, owner_key)],
        )
    )
    assert result.abi_return == account2.address
