import pytest
import algokit_utils as au
from conftest import deploy_contract, mapping_box_key, fund_account
from algosdk import encoding


@pytest.fixture(scope="module")
def erc1155_burnable(localnet, account):
    return deploy_contract(localnet, account, "ERC1155BurnableTest")


def addr_bytes(address: str) -> bytes:
    return encoding.decode_address(address)


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def token_id_bytes(token_id: int) -> bytes:
    return token_id.to_bytes(64, "big")


def test_deploy(erc1155_burnable):
    assert erc1155_burnable.app_id > 0


def test_mint(erc1155_burnable, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(1)
    box_key = mapping_box_key("_balances", tid, addr)
    erc1155_burnable.send.call(
        au.AppClientMethodCallParams(
            method="mint",
            args=[addr, 1, 100],
            box_references=[box_ref(erc1155_burnable.app_id, box_key)],
        )
    )
    result = erc1155_burnable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr, 1],
            box_references=[box_ref(erc1155_burnable.app_id, box_key)],
        )
    )
    assert result.abi_return == 100


def test_owner_can_burn(erc1155_burnable, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(1)
    box_key = mapping_box_key("_balances", tid, addr)
    erc1155_burnable.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[addr, 1, 30],
            box_references=[box_ref(erc1155_burnable.app_id, box_key)],
        )
    )
    result = erc1155_burnable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr, 1],
            box_references=[box_ref(erc1155_burnable.app_id, box_key)],
        )
    )
    assert result.abi_return == 70


def test_non_owner_cannot_burn(erc1155_burnable, account, localnet):
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)
    addr = addr_bytes(account.address)

    tid = token_id_bytes(1)
    box_key = mapping_box_key("_balances", tid, addr)
    op_key = mapping_box_key("_operatorApprovals", addr, addr_bytes(account2.address))

    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=erc1155_burnable.app_spec,
            app_id=erc1155_burnable.app_id,
            default_sender=account2.address,
        )
    )
    with pytest.raises(Exception):
        client2.send.call(
            au.AppClientMethodCallParams(
                method="burn",
                args=[addr, 1, 10],
                box_references=[
                    box_ref(erc1155_burnable.app_id, box_key),
                    box_ref(erc1155_burnable.app_id, op_key),
                ],
            )
        )


def test_approved_operator_can_burn(erc1155_burnable, account, localnet):
    account2 = localnet.account.random()
    fund_account(localnet, account, account2)
    addr = addr_bytes(account.address)
    addr2 = addr_bytes(account2.address)
    op_key = mapping_box_key("_operatorApprovals", addr, addr2)

    # Approve account2 as operator
    erc1155_burnable.send.call(
        au.AppClientMethodCallParams(
            method="setApprovalForAll",
            args=[addr2, True],
            box_references=[box_ref(erc1155_burnable.app_id, op_key)],
        )
    )

    # account2 burns on behalf of account
    tid = token_id_bytes(1)
    box_key = mapping_box_key("_balances", tid, addr)
    client2 = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=erc1155_burnable.app_spec,
            app_id=erc1155_burnable.app_id,
            default_sender=account2.address,
        )
    )
    client2.send.call(
        au.AppClientMethodCallParams(
            method="burn",
            args=[addr, 1, 20],
            box_references=[
                box_ref(erc1155_burnable.app_id, box_key),
                box_ref(erc1155_burnable.app_id, op_key),
            ],
        )
    )
    result = erc1155_burnable.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[addr, 1],
            box_references=[box_ref(erc1155_burnable.app_id, box_key)],
        )
    )
    assert result.abi_return == 50


def test_mint_multiple_ids(erc1155_burnable, account):
    addr = addr_bytes(account.address)
    for token_id in [10, 20, 30]:
        tid = token_id_bytes(token_id)
        box_key = mapping_box_key("_balances", tid, addr)
        erc1155_burnable.send.call(
            au.AppClientMethodCallParams(
                method="mint",
                args=[addr, token_id, 200],
                box_references=[box_ref(erc1155_burnable.app_id, box_key)],
            )
        )

    # Verify each
    for token_id in [10, 20, 30]:
        tid = token_id_bytes(token_id)
        box_key = mapping_box_key("_balances", tid, addr)
        result = erc1155_burnable.send.call(
            au.AppClientMethodCallParams(
                method="balanceOf",
                args=[addr, token_id],
                box_references=[box_ref(erc1155_burnable.app_id, box_key)],
            )
        )
        assert result.abi_return == 200


def test_burn_insufficient_balance(erc1155_burnable, account):
    addr = addr_bytes(account.address)
    tid = token_id_bytes(1)
    box_key = mapping_box_key("_balances", tid, addr)
    with pytest.raises(Exception):
        erc1155_burnable.send.call(
            au.AppClientMethodCallParams(
                method="burn",
                args=[addr, 1, 999],
                box_references=[box_ref(erc1155_burnable.app_id, box_key)],
            )
        )
