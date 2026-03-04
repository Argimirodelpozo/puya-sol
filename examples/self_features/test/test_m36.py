"""
M36: Constructor parameters flowing to box-writing code (Gap 12 verification).
Tests that constructor params are properly passed through __postInit.
"""

import hashlib

import pytest
import algokit_utils as au
from algosdk import encoding
from algosdk.transaction import (
    ApplicationCreateTxn, OnComplete, PaymentTxn, StateSchema,
    wait_for_confirmation,
)
from algokit_utils.models.account import SigningAccount

from conftest import load_arc56, OUT_DIR


def mapping_box_key(mapping_name: str, *keys: bytes) -> bytes:
    concat_keys = b"".join(keys)
    key_hash = hashlib.sha256(concat_keys).digest()
    return mapping_name.encode() + key_hash


def box_ref(app_id: int, name: bytes) -> au.BoxReference:
    return au.BoxReference(app_id=app_id, name=name)


def fund_contract(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    app_id: int,
    amount: int = 1_000_000,
) -> None:
    algod = localnet.client.algod
    app_addr = encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )
    sp = algod.suggested_params()
    txn = PaymentTxn(account.address, sp, app_addr, amount)
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    wait_for_confirmation(algod, txid, 4)


def deploy_with_post_init(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    name: str,
    post_init_args: list,
    post_init_boxes: list[au.BoxReference],
) -> au.AppClient:
    """Deploy a contract that requires __postInit with constructor params."""
    algod = localnet.client.algod
    app_spec = load_arc56(name)

    # Step 1: Create the app (bare create, no app args needed)
    approval_path = OUT_DIR / name / f"{name}.approval.teal"
    clear_path = OUT_DIR / name / f"{name}.clear.teal"
    approval_result = algod.compile(approval_path.read_text())
    clear_result = algod.compile(clear_path.read_text())
    approval_program = encoding.base64.b64decode(approval_result["result"])
    clear_program = encoding.base64.b64decode(clear_result["result"])

    sp = algod.suggested_params()
    txn = ApplicationCreateTxn(
        sender=account.address,
        sp=sp,
        on_complete=OnComplete.NoOpOC,
        approval_program=approval_program,
        clear_program=clear_program,
        global_schema=StateSchema(num_uints=16, num_byte_slices=16),
        local_schema=StateSchema(num_uints=0, num_byte_slices=0),
    )
    signed = txn.sign(account.private_key)
    txid = algod.send_transaction(signed)
    result = wait_for_confirmation(algod, txid, 4)
    app_id = result["application-index"]

    # Step 2: Fund the contract (needed for box storage)
    fund_contract(localnet, account, app_id, 2_000_000)

    # Step 3: Call __postInit with constructor parameters
    client = au.AppClient(
        au.AppClientParams(
            algorand=localnet,
            app_spec=app_spec,
            app_id=app_id,
            default_sender=account.address,
        )
    )

    client.send.call(
        au.AppClientMethodCallParams(
            method="__postInit",
            args=post_init_args,
            box_references=post_init_boxes,
        )
    )

    return client


@pytest.fixture(scope="module")
def client(
    localnet: au.AlgorandClient, account: SigningAccount
) -> au.AppClient:
    """Deploy TokenWithSupply with initialOwner=account, initialSupply=1000."""
    owner_addr = account.address
    owner_bytes = encoding.decode_address(owner_addr)
    initial_supply = 1000

    # Box for _balances[owner]
    balance_box = box_ref(0, mapping_box_key("_balances", owner_bytes))

    c = deploy_with_post_init(
        localnet, account, "TokenWithSupply",
        post_init_args=[owner_addr, initial_supply],
        post_init_boxes=[balance_box],
    )

    # Update box refs to use actual app_id
    return c


@pytest.mark.localnet
def test_owner_set(client: au.AppClient, account: SigningAccount) -> None:
    """Constructor should set _owner to initialOwner."""
    result = client.send.call(
        au.AppClientMethodCallParams(method="owner", args=[])
    )
    assert result.abi_return == account.address


@pytest.mark.localnet
def test_total_supply(client: au.AppClient) -> None:
    """Constructor should set _totalSupply to initialSupply."""
    result = client.send.call(
        au.AppClientMethodCallParams(method="totalSupply", args=[])
    )
    assert result.abi_return == 1000


@pytest.mark.localnet
def test_initial_balance(client: au.AppClient, account: SigningAccount) -> None:
    """Constructor should mint initialSupply to initialOwner's balance."""
    app_id = client.app_id
    owner_bytes = encoding.decode_address(account.address)
    box = box_ref(app_id, mapping_box_key("_balances", owner_bytes))

    result = client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[box],
        )
    )
    assert result.abi_return == 1000


@pytest.mark.localnet
def test_transfer(client: au.AppClient, account: SigningAccount) -> None:
    """Transfer from owner should update balances."""
    app_id = client.app_id
    owner_bytes = encoding.decode_address(account.address)

    # Create a recipient address (use zero address for simplicity)
    recipient = encoding.encode_address(b"\x00" * 32)
    recipient_bytes = encoding.decode_address(recipient)

    owner_box = box_ref(app_id, mapping_box_key("_balances", owner_bytes))
    recipient_box = box_ref(app_id, mapping_box_key("_balances", recipient_bytes))

    client.send.call(
        au.AppClientMethodCallParams(
            method="transfer",
            args=[recipient, 100],
            box_references=[owner_box, recipient_box],
        )
    )

    # Owner should have 900
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[account.address],
            box_references=[owner_box],
        )
    )
    assert result.abi_return == 900

    # Recipient should have 100
    result = client.send.call(
        au.AppClientMethodCallParams(
            method="balanceOf",
            args=[recipient],
            box_references=[recipient_box],
        )
    )
    assert result.abi_return == 100


@pytest.mark.localnet
def test_post_init_cannot_be_called_twice(
    client: au.AppClient, account: SigningAccount
) -> None:
    """__postInit should reject a second call (ctor_pending cleared)."""
    owner_bytes = encoding.decode_address(account.address)
    box = box_ref(client.app_id, mapping_box_key("_balances", owner_bytes))

    with pytest.raises(Exception):
        client.send.call(
            au.AppClientMethodCallParams(
                method="__postInit",
                args=[account.address, 999],
                box_references=[box],
            )
        )
