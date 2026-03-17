"""
Tests for ETHTornado contract — the core Tornado Cash mixer.

Uses HasherMock and VerifierMock for testing without real MiMC or ZK proofs.
"""
import hashlib

import algokit_utils as au
from algosdk import encoding
from algosdk.abi import Method
from algosdk.atomic_transaction_composer import (
    AtomicTransactionComposer, TransactionWithSigner, AccountTransactionSigner,
)
from algosdk.transaction import PaymentTxn, ApplicationCallTxn, OnComplete, wait_for_confirmation
from algokit_utils.models.account import SigningAccount
import pytest

from conftest import deploy_contract, fund_contract, box_key


DENOMINATION = 1_000_000  # 1 ALGO denomination (in microalgos equivalent)
FIELD_SIZE = 21888242871839275222246405745257275088548364400416034343698204186575808495617


def make_commitment(seed: bytes) -> bytes:
    """Create a commitment within the BN254 field (as real Tornado Cash would)."""
    h = int.from_bytes(hashlib.sha256(seed).digest(), "big") % FIELD_SIZE
    return h.to_bytes(32, "big")


def app_addr(app_id: int) -> str:
    return encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )


def int_key(i: int) -> bytes:
    """Mapping key for integer indices — compiler uses itob (8-byte big-endian)."""
    return i.to_bytes(8, "big")


NO_POPULATE = au.SendParams(populate_app_call_resources=False)


@pytest.fixture(scope="module")
def hasher(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_contract(localnet, account, "HasherMock")


@pytest.fixture(scope="module")
def verifier(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    return deploy_contract(localnet, account, "VerifierMock")


@pytest.fixture(scope="module")
def tornado(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    hasher: au.AppClient,
    verifier: au.AppClient,
) -> au.AppClient:
    """Deploy ETHTornado with mock hasher and verifier."""
    client = deploy_contract(localnet, account, "ETHTornado", fund_amount=5_000_000)

    # Embed app IDs at byte position 24 (where contract does extract_uint64)
    verifier_addr = b'\x00' * 24 + verifier.app_id.to_bytes(8, "big")
    hasher_addr = b'\x00' * 24 + hasher.app_id.to_bytes(8, "big")

    # Initialize: __postInit(verifier, hasher, denomination, merkleTreeHeight)
    # Need boxes for filledSubtrees[0..3] and roots[0]
    box_refs = [
        au.BoxReference(app_id=client.app_id, name=box_key("filledSubtrees", int_key(i)))
        for i in range(4)
    ] + [
        au.BoxReference(app_id=client.app_id, name=box_key("roots", int_key(0))),
    ]

    client.send.call(
        au.AppClientMethodCallParams(
            method="__postInit",
            args=[verifier_addr, hasher_addr, DENOMINATION, 4],
            box_references=box_refs,
        ),
        send_params=NO_POPULATE,
    )
    return client


def do_deposit(
    algod,
    account: SigningAccount,
    tornado: au.AppClient,
    hasher: au.AppClient,
    commitment: bytes,
    deposit_index: int = 0,
):
    """Execute a deposit via ATC: grouped payment + app call."""
    signer = AccountTransactionSigner(account.private_key)
    sp = algod.suggested_params()
    # Extra fee on payment txn to cover inner txns:
    # hashLeftRight does 2 inner txns per tree level, 4 levels = 8 inner txns
    sp.fee = 10 * 1000  # 1 for payment + 1 for app call + 8 for inner txns
    sp.flat_fee = True

    # Only need current root + next root boxes (not all history)
    next_root = deposit_index + 1
    box_refs = [
        (tornado.app_id, box_key("filledSubtrees", int_key(i)))
        for i in range(4)
    ] + [
        (tornado.app_id, box_key("roots", int_key(deposit_index))),
        (tornado.app_id, box_key("roots", int_key(next_root))),
    ] + [
        (tornado.app_id, box_key("commitments", commitment)),
    ]

    atc = AtomicTransactionComposer()

    # txn 0: payment to contract (msg.value = gtxn 0 Amount)
    pay_txn = PaymentTxn(
        sender=account.address,
        sp=sp,
        receiver=app_addr(tornado.app_id),
        amt=DENOMINATION,
    )
    atc.add_transaction(TransactionWithSigner(pay_txn, signer))

    # txn 1: app call to deposit(byte[32])
    sp2 = algod.suggested_params()
    sp2.fee = 0  # fee covered by payment txn
    sp2.flat_fee = True
    deposit_method = Method.from_signature("deposit(byte[32])void")
    atc.add_method_call(
        app_id=tornado.app_id,
        method=deposit_method,
        sender=account.address,
        sp=sp2,
        signer=signer,
        method_args=[commitment],
        boxes=box_refs,
        foreign_apps=[hasher.app_id],
    )

    atc.execute(algod, wait_rounds=4)


class TestDeployment:
    """Test contract deployment and initialization."""

    def test_denomination(self, tornado: au.AppClient):
        result = tornado.send.call(
            au.AppClientMethodCallParams(method="denomination")
        )
        assert result.abi_return == DENOMINATION

    def test_levels(self, tornado: au.AppClient):
        result = tornado.send.call(
            au.AppClientMethodCallParams(method="levels")
        )
        assert result.abi_return == 4

    def test_initial_next_index(self, tornado: au.AppClient):
        result = tornado.send.call(
            au.AppClientMethodCallParams(method="nextIndex")
        )
        assert result.abi_return == 0


class TestDeposit:
    """Test the deposit functionality."""

    def test_deposit_updates_commitment(
        self,
        localnet: au.AlgorandClient,
        account: SigningAccount,
        tornado: au.AppClient,
        hasher: au.AppClient,
    ):
        """Depositing with a commitment should store it and update the tree."""
        commitment = make_commitment(b"test_commitment_1")
        do_deposit(localnet.client.algod, account, tornado, hasher, commitment)

        # nextIndex should be 1 after first deposit
        result = tornado.send.call(
            au.AppClientMethodCallParams(method="nextIndex")
        )
        assert result.abi_return == 1

    def test_deposit_duplicate_fails(
        self,
        localnet: au.AlgorandClient,
        account: SigningAccount,
        tornado: au.AppClient,
        hasher: au.AppClient,
    ):
        """Depositing the same commitment twice should fail."""
        commitment = make_commitment(b"test_commitment_1")
        with pytest.raises(Exception):
            do_deposit(localnet.client.algod, account, tornado, hasher, commitment)

    def test_multiple_deposits(
        self,
        localnet: au.AlgorandClient,
        account: SigningAccount,
        tornado: au.AppClient,
        hasher: au.AppClient,
    ):
        """Multiple unique deposits should all succeed."""
        for i in range(2, 5):
            commitment = make_commitment(f"test_commitment_{i}".encode())
            do_deposit(
                localnet.client.algod, account, tornado, hasher, commitment,
                deposit_index=i - 1,  # 0-indexed: deposits 1,2,3
            )

        result = tornado.send.call(
            au.AppClientMethodCallParams(method="nextIndex")
        )
        assert result.abi_return == 4  # 1 from previous test + 3 more


class TestRootHistory:
    """Test the Merkle root history tracking."""

    def test_is_known_root(self, tornado: au.AppClient):
        """The current root should be known."""
        root_idx = tornado.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return

        root = tornado.send.call(
            au.AppClientMethodCallParams(
                method="getLastRoot",
                box_references=[
                    au.BoxReference(
                        app_id=tornado.app_id,
                        name=box_key("roots", int_key(root_idx))
                    ),
                ],
            ),
            send_params=NO_POPULATE,
        ).abi_return

        # Check it's known — need all root history boxes
        box_refs = [
            au.BoxReference(app_id=tornado.app_id, name=box_key("roots", int_key(i)))
            for i in range(min(root_idx + 2, 30))
        ]
        result = tornado.send.call(
            au.AppClientMethodCallParams(
                method="isKnownRoot",
                args=[bytes(root)],
                box_references=box_refs,
            ),
            send_params=NO_POPULATE,
        )
        assert result.abi_return is True

    def test_zero_root_not_known(self, tornado: au.AppClient):
        """The zero root should never be known."""
        zero_root = b'\x00' * 32
        box_refs = [
            au.BoxReference(app_id=tornado.app_id, name=box_key("roots", int_key(i)))
            for i in range(5)
        ]
        result = tornado.send.call(
            au.AppClientMethodCallParams(
                method="isKnownRoot",
                args=[zero_root],
                box_references=box_refs,
            ),
            send_params=NO_POPULATE,
        )
        assert result.abi_return is False


class TestIsSpent:
    """Test the isSpent nullifier check."""

    def test_unspent_nullifier(self, tornado: au.AppClient):
        """An unused nullifier should not be spent."""
        nullifier = hashlib.sha256(b"unused_nullifier").digest()
        result = tornado.send.call(
            au.AppClientMethodCallParams(
                method="isSpent",
                args=[nullifier],
                box_references=[
                    au.BoxReference(
                        app_id=tornado.app_id,
                        name=box_key("nullifierHashes", nullifier),
                    ),
                ],
            ),
            send_params=NO_POPULATE,
        )
        assert result.abi_return is False


class TestZeros:
    """Test the precomputed zero values."""

    def test_zeros_0(self, tornado: au.AppClient):
        expected = 0x2fe54c60d3acabf3343a35b6eba15db4821b340f76e741e2249685ed4899af6c
        result = tornado.send.call(
            au.AppClientMethodCallParams(method="zeros", args=[0])
        )
        assert bytes(result.abi_return) == expected.to_bytes(32, "big")

    def test_zeros_31(self, tornado: au.AppClient):
        expected = 0x2c7a07d20dff79d01fecedc1134284a8d08436606c93693b67e333f671bf69cc
        result = tornado.send.call(
            au.AppClientMethodCallParams(method="zeros", args=[31])
        )
        assert bytes(result.abi_return) == expected.to_bytes(32, "big")
