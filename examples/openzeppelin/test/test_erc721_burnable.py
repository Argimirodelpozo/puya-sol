import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key, fund_account
from algosdk import encoding


@pytest.fixture(scope="module")
def erc721_burnable(localnet, account):
    return deploy_contract(localnet, account, "ERC721BurnableTest")


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def token_id_bytes(token_id: int) -> bytes:
    return token_id.to_bytes(64, "big")


ZERO_ADDR = b"\x00" * 32


def test_deploy(erc721_burnable):
    assert erc721_burnable.app_id > 0


def test_mint(erc721_burnable, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(1)
    owner_key = mapping_box_key("_owners", tid)
    balance_key = mapping_box_key("_balances", addr)
    erc721_burnable.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr, 1],
            box_references=[
                box_ref(erc721_burnable.app_id, owner_key),
                box_ref(erc721_burnable.app_id, balance_key),
            ],
        )
    )
    result = erc721_burnable.send.call(
        au.AppClientMethodCallParams(
            method="ownerOf",
            args=[1],
            box_references=[box_ref(erc721_burnable.app_id, owner_key)],
        )
    )
    assert result.abi_return == account.address


def test_owner_can_burn(erc721_burnable, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(1)
    owner_key = mapping_box_key("_owners", tid)
    balance_key = mapping_box_key("_balances", addr)
    approval_key = mapping_box_key("_tokenApprovals", tid)

    erc721_burnable.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[1],
            box_references=[
                box_ref(erc721_burnable.app_id, owner_key),
                box_ref(erc721_burnable.app_id, balance_key),
                box_ref(erc721_burnable.app_id, approval_key),
            ],
        )
    )

    # ownerOf should fail for burned token
    with pytest.raises(Exception):
        erc721_burnable.send.call(
            au.AppClientMethodCallParams(
                method="ownerOf",
                args=[1],
                box_references=[box_ref(erc721_burnable.app_id, owner_key)],
            )
        )


def test_balance_decremented_after_burn(erc721_burnable, account):
    addr = addr_bytes(account.address)
    balance_key = mapping_box_key("_balances", addr)
    result = erc721_burnable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr],
            box_references=[box_ref(erc721_burnable.app_id, balance_key)],
        )
    )
    assert result.abi_return == 0


def test_non_owner_cannot_burn(erc721_burnable, account, localnet):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(2)
    owner_key = mapping_box_key("_owners", tid)
    balance_key = mapping_box_key("_balances", addr)

    # Mint token 2
    erc721_burnable.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr, 2],
            box_references=[
                box_ref(erc721_burnable.app_id, owner_key),
                box_ref(erc721_burnable.app_id, balance_key),
            ],
        )
    )

    # Non-owner tries to burn
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)
    approval_key = mapping_box_key("_tokenApprovals", tid)
    op_key = mapping_box_key("_operatorApprovals", addr, addr_bytes(account2.address))

    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=erc721_burnable.app_spec,
            app_id=erc721_burnable.app_id,
            default_sender=account2.address,
        )
    )
    with pytest.raises(Exception):
        client2.send.call(
            au.AppClientMethodCallParams(
                method="burn",
                args=[2],
                box_references=[
                    box_ref(erc721_burnable.app_id, owner_key),
                    box_ref(erc721_burnable.app_id, balance_key),
                    box_ref(erc721_burnable.app_id, approval_key),
                    box_ref(erc721_burnable.app_id, op_key),
                ],
            )
        )


def test_approved_can_burn(erc721_burnable, account, localnet):
    addr = addr_bytes(account.address)
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)
    addr2 = addr_bytes(account2.address)

    # Mint token 3
    tid = token_id_bytes(3)
    owner_key = mapping_box_key("_owners", tid)
    balance_key = mapping_box_key("_balances", addr)
    approval_key = mapping_box_key("_tokenApprovals", tid)

    erc721_burnable.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr, 3],
            box_references=[
                box_ref(erc721_burnable.app_id, owner_key),
                box_ref(erc721_burnable.app_id, balance_key),
            ],
        )
    )

    # Approve account2 for token 3
    erc721_burnable.send.call(
        au.AppClientMethodCallParams(
            method="approve",
            args=[addr2, 3],
            box_references=[
                box_ref(erc721_burnable.app_id, owner_key),
                box_ref(erc721_burnable.app_id, approval_key),
            ],
        )
    )

    # account2 burns token 3
    op_key = mapping_box_key("_operatorApprovals", addr, addr2)
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=erc721_burnable.app_spec,
            app_id=erc721_burnable.app_id,
            default_sender=account2.address,
        )
    )
    client2.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[3],
            box_references=[
                box_ref(erc721_burnable.app_id, owner_key),
                box_ref(erc721_burnable.app_id, balance_key),
                box_ref(erc721_burnable.app_id, approval_key),
                box_ref(erc721_burnable.app_id, op_key),
            ],
        )
    )

    with pytest.raises(Exception):
        erc721_burnable.send.call(
            au.AppClientMethodCallParams(
                method="ownerOf",
                args=[3],
                box_references=[box_ref(erc721_burnable.app_id, owner_key)],
            )
        )
