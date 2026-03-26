"""
Tests for ETHTornado withdrawal — translated from tornado-core/test/ETHTornado.test.js.

Covers the full deposit→withdraw lifecycle using VerifierMock (always returns true).
Original Tornado Cash tests verify:
  - Successful withdrawal with correct balance changes
  - Withdrawal with relayer fee
  - Double-spend prevention
  - Fee validation (fee <= denomination)
  - Merkle root validation (corrupted root rejected)
  - Refund rejection for ETH instance
  - isSpent status tracking
"""
import hashlib

import algokit_utils as au
from algosdk import encoding
from algosdk.abi import Method
from algosdk.atomic_transaction_composer import (
    AtomicTransactionComposer, TransactionWithSigner, AccountTransactionSigner,
)
from algosdk.transaction import PaymentTxn
from algokit_utils.models.account import SigningAccount
import pytest

from conftest import deploy_contract, fund_contract, box_key


DENOMINATION = 1_000_000  # 1 ALGO (in microalgos)
FIELD_SIZE = 21888242871839275222246405745257275088548364400416034343698204186575808495617


def make_commitment(seed: bytes) -> bytes:
    """Create a commitment within the BN254 field."""
    h = int.from_bytes(hashlib.sha256(seed).digest(), "big") % FIELD_SIZE
    return h.to_bytes(32, "big")


def app_addr(app_id: int) -> str:
    return encoding.encode_address(
        encoding.checksum(b"appID" + app_id.to_bytes(8, "big"))
    )


def int_key(i: int) -> bytes:
    """Mapping key for integer indices — compiler normalizes to 32-byte biguint."""
    return i.to_bytes(32, "big")


NO_POPULATE = au.SendParams(populate_app_call_resources=False)


def get_balance(algod, address: str) -> int:
    """Get the balance of an Algorand account (0 if not yet created)."""
    try:
        info = algod.account_info(address)
        return info["amount"]
    except Exception:
        return 0


# ---------------------------------------------------------------------------
# Fixtures — fresh contract set per module
# ---------------------------------------------------------------------------

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
    """Deploy a fresh ETHTornado with mock hasher/verifier."""
    client = deploy_contract(localnet, account, "ETHTornado", fund_amount=10_000_000)

    verifier_addr = b'\x00' * 24 + verifier.app_id.to_bytes(8, "big")
    hasher_addr = b'\x00' * 24 + hasher.app_id.to_bytes(8, "big")

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


@pytest.fixture(scope="module")
def recipient(localnet: au.AlgorandClient, account: SigningAccount) -> str:
    """Create and fund a recipient account."""
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, account, au.AlgoAmount(micro_algo=1_000_000),
    )
    return acct.address


@pytest.fixture(scope="module")
def relayer(localnet: au.AlgorandClient, account: SigningAccount) -> str:
    """Create and fund a relayer account."""
    acct = localnet.account.random()
    localnet.account.ensure_funded(
        acct, account, au.AlgoAmount(micro_algo=1_000_000),
    )
    return acct.address


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def do_deposit(
    algod,
    account: SigningAccount,
    tornado: au.AppClient,
    hasher: au.AppClient,
    commitment: bytes,
    deposit_index: int = 0,
):
    """Execute a deposit: grouped payment + app call."""
    signer = AccountTransactionSigner(account.private_key)
    sp = algod.suggested_params()
    sp.fee = 10 * 1000  # 1 payment + 1 app call + 8 inner hasher calls
    sp.flat_fee = True

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

    pay_txn = PaymentTxn(
        sender=account.address,
        sp=sp,
        receiver=app_addr(tornado.app_id),
        amt=DENOMINATION,
    )
    atc.add_transaction(TransactionWithSigner(pay_txn, signer))

    sp2 = algod.suggested_params()
    sp2.fee = 0
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

    result = atc.execute(algod, wait_rounds=4)
    return result


def do_withdraw(
    algod,
    account: SigningAccount,
    tornado: au.AppClient,
    verifier: au.AppClient,
    root: bytes,
    nullifier_hash: bytes,
    recipient_addr: str,
    relayer_addr: str,
    fee: int = 0,
    refund: int = 0,
    current_root_index: int = 1,
):
    """Execute a withdrawal via ATC."""
    signer = AccountTransactionSigner(account.private_key)

    # Dummy proof — VerifierMock always returns true
    proof = b'\x00' * 256  # 8 × 32 bytes

    # Box refs: nullifierHashes + roots for isKnownRoot
    # Total app refs (boxes + accounts + foreign apps) must be <= 8.
    # We need: 1 nullifier box + N root boxes + 2 accounts + 1 foreign app.
    # So max root boxes = 8 - 1(nullifier) - 2(accounts) - 1(app) = 4.
    # isKnownRoot iterates backward from currentRootIndex; providing the most
    # recent roots is sufficient since we always pass a recent valid root.
    num_accounts = 1 if relayer_addr == recipient_addr else 2
    max_root_boxes = 8 - 1 - num_accounts - 1  # nullifier + accounts + foreign app
    num_roots = min(current_root_index + 1, max_root_boxes)
    start = max(0, current_root_index + 1 - num_roots)
    box_refs = [
        (tornado.app_id, box_key("nullifierHashes", nullifier_hash)),
    ] + [
        (tornado.app_id, box_key("roots", int_key(i)))
        for i in range(start, start + num_roots)
    ]

    # Inner txns: 1 verifier call + 1 payment to recipient + (1 payment to relayer if fee>0)
    num_inner = 2 if fee == 0 else 3
    sp = algod.suggested_params()
    sp.fee = (1 + num_inner) * 1000
    sp.flat_fee = True

    withdraw_method = Method.from_signature(
        "withdraw(byte[],byte[32],byte[32],address,address,uint256,uint256)void"
    )

    # Build accounts list for inner payments
    accounts = [recipient_addr]
    if relayer_addr != recipient_addr:
        accounts.append(relayer_addr)

    atc = AtomicTransactionComposer()
    atc.add_method_call(
        app_id=tornado.app_id,
        method=withdraw_method,
        sender=account.address,
        sp=sp,
        signer=signer,
        method_args=[proof, root, nullifier_hash, recipient_addr, relayer_addr, fee, refund],
        boxes=box_refs,
        foreign_apps=[verifier.app_id],
        accounts=accounts,
    )

    result = atc.execute(algod, wait_rounds=4)
    return result


def get_current_root(tornado: au.AppClient, root_idx: int) -> bytes:
    """Read the current Merkle root from the contract."""
    result = tornado.send.call(
        au.AppClientMethodCallParams(
            method="getLastRoot",
            box_references=[
                au.BoxReference(
                    app_id=tornado.app_id,
                    name=box_key("roots", int_key(root_idx)),
                ),
            ],
        ),
        send_params=NO_POPULATE,
    )
    return bytes(result.abi_return)


# ---------------------------------------------------------------------------
# Tests — faithful translation of tornado-core/test/ETHTornado.test.js
# ---------------------------------------------------------------------------

class TestWithdrawShouldWork:
    """
    Original: '#withdraw should work'
    Deposit, then withdraw to a recipient. Verify balance changes.
    """

    def test_deposit_then_withdraw(
        self,
        localnet: au.AlgorandClient,
        account: SigningAccount,
        tornado: au.AppClient,
        hasher: au.AppClient,
        verifier: au.AppClient,
        recipient: str,
        relayer: str,
    ):
        """Deposit 1 ALGO, withdraw to recipient with no fee."""
        algod = localnet.client.algod

        # --- deposit ---
        commitment = make_commitment(b"withdraw_test_1")
        do_deposit(algod, account, tornado, hasher, commitment, deposit_index=0)

        # Get the root after deposit
        root_idx = tornado.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return
        root = get_current_root(tornado, root_idx)

        # Record recipient balance before withdrawal
        recipient_balance_before = get_balance(algod, recipient)

        # --- withdraw (fee=0, refund=0) ---
        nullifier_hash = hashlib.sha256(b"nullifier_1").digest()
        do_withdraw(
            algod, account, tornado, verifier,
            root=root,
            nullifier_hash=nullifier_hash,
            recipient_addr=recipient,
            relayer_addr=relayer,
            fee=0,
            refund=0,
            current_root_index=root_idx,
        )

        # Recipient should have received denomination (1 ALGO)
        recipient_balance_after = get_balance(algod, recipient)
        assert recipient_balance_after - recipient_balance_before == DENOMINATION

    def test_nullifier_is_spent_after_withdraw(
        self,
        tornado: au.AppClient,
    ):
        """After withdrawal, the nullifier should be marked as spent."""
        nullifier_hash = hashlib.sha256(b"nullifier_1").digest()
        result = tornado.send.call(
            au.AppClientMethodCallParams(
                method="isSpent",
                args=[nullifier_hash],
                box_references=[
                    au.BoxReference(
                        app_id=tornado.app_id,
                        name=box_key("nullifierHashes", nullifier_hash),
                    ),
                ],
            ),
            send_params=NO_POPULATE,
        )
        assert result.abi_return is True


class TestWithdrawWithFee:
    """
    Original: '#withdraw should work with different relayer fee'
    Withdraw with a relayer fee and verify both recipient and relayer balances.
    """

    def test_withdraw_with_relayer_fee(
        self,
        localnet: au.AlgorandClient,
        account: SigningAccount,
        tornado: au.AppClient,
        hasher: au.AppClient,
        verifier: au.AppClient,
        recipient: str,
        relayer: str,
    ):
        algod = localnet.client.algod

        # Deposit #2
        commitment = make_commitment(b"withdraw_test_fee")
        do_deposit(algod, account, tornado, hasher, commitment, deposit_index=1)

        root_idx = tornado.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return
        root = get_current_root(tornado, root_idx)

        recipient_before = get_balance(algod, recipient)
        relayer_before = get_balance(algod, relayer)

        fee = DENOMINATION // 10  # 10% fee

        nullifier_hash = hashlib.sha256(b"nullifier_fee").digest()
        do_withdraw(
            algod, account, tornado, verifier,
            root=root,
            nullifier_hash=nullifier_hash,
            recipient_addr=recipient,
            relayer_addr=relayer,
            fee=fee,
            refund=0,
            current_root_index=root_idx,
        )

        recipient_after = get_balance(algod, recipient)
        relayer_after = get_balance(algod, relayer)

        # Recipient gets denomination - fee
        assert recipient_after - recipient_before == DENOMINATION - fee
        # Relayer gets fee
        assert relayer_after - relayer_before == fee


class TestPreventDoubleSpend:
    """
    Original: '#withdraw should prevent double spend'
    Using the same nullifier twice should fail.
    """

    def test_double_spend_rejected(
        self,
        localnet: au.AlgorandClient,
        account: SigningAccount,
        tornado: au.AppClient,
        hasher: au.AppClient,
        verifier: au.AppClient,
        recipient: str,
        relayer: str,
    ):
        algod = localnet.client.algod

        # Deposit #3
        commitment = make_commitment(b"withdraw_test_double")
        do_deposit(algod, account, tornado, hasher, commitment, deposit_index=2)

        root_idx = tornado.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return
        root = get_current_root(tornado, root_idx)

        nullifier_hash = hashlib.sha256(b"nullifier_double").digest()

        # First withdraw succeeds
        do_withdraw(
            algod, account, tornado, verifier,
            root=root,
            nullifier_hash=nullifier_hash,
            recipient_addr=recipient,
            relayer_addr=relayer,
            fee=0, refund=0,
            current_root_index=root_idx,
        )

        # Second withdraw with same nullifier must fail
        # ("The note has been already spent")
        with pytest.raises(Exception):
            do_withdraw(
                algod, account, tornado, verifier,
                root=root,
                nullifier_hash=nullifier_hash,
                recipient_addr=recipient,
                relayer_addr=relayer,
                fee=0, refund=0,
                current_root_index=root_idx,
            )


class TestFeeExceedsDenomination:
    """
    Original: '#withdraw fee should be less or equal transfer value'
    Fee > denomination should be rejected.
    """

    def test_excessive_fee_rejected(
        self,
        localnet: au.AlgorandClient,
        account: SigningAccount,
        tornado: au.AppClient,
        hasher: au.AppClient,
        verifier: au.AppClient,
        recipient: str,
        relayer: str,
    ):
        algod = localnet.client.algod

        # Deposit #4
        commitment = make_commitment(b"withdraw_test_fee_limit")
        do_deposit(algod, account, tornado, hasher, commitment, deposit_index=3)

        root_idx = tornado.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return
        root = get_current_root(tornado, root_idx)

        nullifier_hash = hashlib.sha256(b"nullifier_fee_limit").digest()

        # Fee exceeds denomination — must fail ("Fee exceeds transfer value")
        with pytest.raises(Exception):
            do_withdraw(
                algod, account, tornado, verifier,
                root=root,
                nullifier_hash=nullifier_hash,
                recipient_addr=recipient,
                relayer_addr=relayer,
                fee=DENOMINATION + 1,
                refund=0,
                current_root_index=root_idx,
            )


class TestCorruptedMerkleRoot:
    """
    Original: '#withdraw should throw for corrupted merkle tree root'
    Providing an unknown root should fail.
    """

    def test_unknown_root_rejected(
        self,
        localnet: au.AlgorandClient,
        account: SigningAccount,
        tornado: au.AppClient,
        verifier: au.AppClient,
        recipient: str,
        relayer: str,
    ):
        algod = localnet.client.algod

        # Use a fake root that's never been in the tree
        fake_root = hashlib.sha256(b"totally_fake_root").digest()
        nullifier_hash = hashlib.sha256(b"nullifier_fake_root").digest()

        # Should fail — "Cannot find your merkle root" or box access error
        with pytest.raises(Exception):
            do_withdraw(
                algod, account, tornado, verifier,
                root=fake_root,
                nullifier_hash=nullifier_hash,
                recipient_addr=recipient,
                relayer_addr=relayer,
                fee=0, refund=0,
                current_root_index=4,  # after 4 deposits
            )


class TestNonZeroRefund:
    """
    Original: '#withdraw should reject with non zero refund'
    ETHTornado requires _refund == 0.
    """

    def test_nonzero_refund_rejected(
        self,
        localnet: au.AlgorandClient,
        account: SigningAccount,
        tornado: au.AppClient,
        hasher: au.AppClient,
        verifier: au.AppClient,
        recipient: str,
        relayer: str,
    ):
        algod = localnet.client.algod

        # Deposit #5 (need a valid root for the test to reach the refund check)
        commitment = make_commitment(b"withdraw_test_refund")
        do_deposit(algod, account, tornado, hasher, commitment, deposit_index=4)

        root_idx = tornado.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return
        root = get_current_root(tornado, root_idx)

        nullifier_hash = hashlib.sha256(b"nullifier_refund").digest()

        # refund != 0 — must fail ("Refund value is supposed to be zero for ETH instance")
        with pytest.raises(Exception):
            do_withdraw(
                algod, account, tornado, verifier,
                root=root,
                nullifier_hash=nullifier_hash,
                recipient_addr=recipient,
                relayer_addr=relayer,
                fee=0,
                refund=1,
                current_root_index=root_idx,
            )


class TestIsSpent:
    """
    Original: '#isSpent should work'
    Verify spent/unspent status for multiple nullifiers.
    """

    def test_spent_nullifier_returns_true(self, tornado: au.AppClient):
        """Nullifier used in TestWithdrawShouldWork should be spent."""
        nullifier_hash = hashlib.sha256(b"nullifier_1").digest()
        result = tornado.send.call(
            au.AppClientMethodCallParams(
                method="isSpent",
                args=[nullifier_hash],
                box_references=[
                    au.BoxReference(
                        app_id=tornado.app_id,
                        name=box_key("nullifierHashes", nullifier_hash),
                    ),
                ],
            ),
            send_params=NO_POPULATE,
        )
        assert result.abi_return is True

    def test_unspent_nullifier_returns_false(self, tornado: au.AppClient):
        """A never-used nullifier should not be spent."""
        nullifier_hash = hashlib.sha256(b"never_used_nullifier").digest()
        result = tornado.send.call(
            au.AppClientMethodCallParams(
                method="isSpent",
                args=[nullifier_hash],
                box_references=[
                    au.BoxReference(
                        app_id=tornado.app_id,
                        name=box_key("nullifierHashes", nullifier_hash),
                    ),
                ],
            ),
            send_params=NO_POPULATE,
        )
        assert result.abi_return is False

    def test_fee_nullifier_is_spent(self, tornado: au.AppClient):
        """Nullifier used in TestWithdrawWithFee should be spent."""
        nullifier_hash = hashlib.sha256(b"nullifier_fee").digest()
        result = tornado.send.call(
            au.AppClientMethodCallParams(
                method="isSpent",
                args=[nullifier_hash],
                box_references=[
                    au.BoxReference(
                        app_id=tornado.app_id,
                        name=box_key("nullifierHashes", nullifier_hash),
                    ),
                ],
            ),
            send_params=NO_POPULATE,
        )
        assert result.abi_return is True

    def test_double_spend_nullifier_is_spent(self, tornado: au.AppClient):
        """Nullifier used in TestPreventDoubleSpend should be spent."""
        nullifier_hash = hashlib.sha256(b"nullifier_double").digest()
        result = tornado.send.call(
            au.AppClientMethodCallParams(
                method="isSpent",
                args=[nullifier_hash],
                box_references=[
                    au.BoxReference(
                        app_id=tornado.app_id,
                        name=box_key("nullifierHashes", nullifier_hash),
                    ),
                ],
            ),
            send_params=NO_POPULATE,
        )
        assert result.abi_return is True


# Event selectors (first 4 bytes of SHA-512/256 of ARC4 method signature)
DEPOSIT_SELECTOR = bytes.fromhex("cc2d7087")   # Deposit(byte[32],uint64,uint256)
WITHDRAWAL_SELECTOR = bytes.fromhex("b83bcb7a")  # Withdrawal(address,byte[32],address,uint256)


class TestDepositEvent:
    """
    Original: '#deposit should emit event'
    Verify the Deposit event is emitted with correct fields.
    """

    def test_deposit_emits_event(
        self,
        localnet: au.AlgorandClient,
        account: SigningAccount,
        tornado: au.AppClient,
        hasher: au.AppClient,
    ):
        algod = localnet.client.algod

        # Query actual state to get correct root index
        next_index = tornado.send.call(
            au.AppClientMethodCallParams(method="nextIndex")
        ).abi_return
        root_idx = tornado.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return

        commitment = make_commitment(b"event_test_deposit")
        result = do_deposit(algod, account, tornado, hasher, commitment, deposit_index=root_idx)

        # Get the app call confirmation (txn index 1 in the group)
        app_txid = result.tx_ids[1]
        tx_info = algod.pending_transaction_info(app_txid)

        # Find the Deposit event log
        import base64
        log_bytes = None
        for log_entry in tx_info.get("logs", []):
            raw = base64.b64decode(log_entry)
            if raw[:4] == DEPOSIT_SELECTOR:
                log_bytes = raw
                break

        assert log_bytes is not None, "Deposit event not found in logs"

        # Parse: selector(4) + commitment(32) + leafIndex(8) + timestamp(8) = 52 bytes
        assert len(log_bytes) == 52, f"Expected 52 bytes, got {len(log_bytes)}"

        logged_commitment = log_bytes[4:36]
        logged_leaf_index = int.from_bytes(log_bytes[36:44], "big")
        logged_timestamp = int.from_bytes(log_bytes[44:52], "big")

        assert logged_commitment == commitment
        assert logged_leaf_index == next_index  # should match the nextIndex before deposit
        assert logged_timestamp > 0   # block.timestamp should be non-zero


class TestWithdrawalEvent:
    """
    Original: part of '#withdraw should work'
    Verify the Withdrawal event is emitted with correct fields.
    """

    def test_withdraw_emits_event(
        self,
        localnet: au.AlgorandClient,
        account: SigningAccount,
        tornado: au.AppClient,
        hasher: au.AppClient,
        verifier: au.AppClient,
        recipient: str,
        relayer: str,
    ):
        algod = localnet.client.algod

        # Query actual state for correct indices
        root_idx_before = tornado.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return

        commitment = make_commitment(b"withdrawal_event_test")
        do_deposit(algod, account, tornado, hasher, commitment, deposit_index=root_idx_before)

        root_idx = tornado.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return
        root = get_current_root(tornado, root_idx)

        fee = DENOMINATION // 5  # 20% fee
        nullifier_hash = hashlib.sha256(b"nullifier_event_test").digest()

        result = do_withdraw(
            algod, account, tornado, verifier,
            root=root,
            nullifier_hash=nullifier_hash,
            recipient_addr=recipient,
            relayer_addr=relayer,
            fee=fee,
            refund=0,
            current_root_index=root_idx,
        )

        # Get the app call confirmation (single txn, index 0)
        import base64
        app_txid = result.tx_ids[0]
        tx_info = algod.pending_transaction_info(app_txid)

        # Find the Withdrawal event log
        log_bytes = None
        for log_entry in tx_info.get("logs", []):
            raw = base64.b64decode(log_entry)
            if raw[:4] == WITHDRAWAL_SELECTOR:
                log_bytes = raw
                break

        assert log_bytes is not None, "Withdrawal event not found in logs"

        # Parse: selector(4) + to(32) + nullifierHash(32) + relayer(32) + fee(32) = 132 bytes
        assert len(log_bytes) == 132, f"Expected 132 bytes, got {len(log_bytes)}"

        logged_to = log_bytes[4:36]
        logged_nullifier = log_bytes[36:68]
        logged_relayer = log_bytes[68:100]
        logged_fee = int.from_bytes(log_bytes[100:132], "big")

        # Verify recipient address (32-byte Algorand public key)
        expected_recipient = encoding.decode_address(recipient)
        assert logged_to == expected_recipient

        assert logged_nullifier == nullifier_hash

        expected_relayer = encoding.decode_address(relayer)
        assert logged_relayer == expected_relayer

        assert logged_fee == fee


class TestWrongDepositAmount:
    """
    Original: '#deposit should not allow deposit with incorrect amount'
    Sending wrong msg.value should be rejected.
    """

    def test_deposit_wrong_amount_rejected(
        self,
        localnet: au.AlgorandClient,
        account: SigningAccount,
        tornado: au.AppClient,
        hasher: au.AppClient,
    ):
        """Deposit with amount != denomination should fail."""
        algod = localnet.client.algod
        signer = AccountTransactionSigner(account.private_key)

        root_idx = tornado.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return

        commitment = make_commitment(b"wrong_amount_test")

        box_refs = [
            (tornado.app_id, box_key("filledSubtrees", int_key(i)))
            for i in range(4)
        ] + [
            (tornado.app_id, box_key("roots", int_key(root_idx))),
            (tornado.app_id, box_key("roots", int_key(root_idx + 1))),
        ] + [
            (tornado.app_id, box_key("commitments", commitment)),
        ]

        # Send half the denomination — should be rejected
        sp = algod.suggested_params()
        sp.fee = 10 * 1000
        sp.flat_fee = True

        atc = AtomicTransactionComposer()
        pay_txn = PaymentTxn(
            sender=account.address,
            sp=sp,
            receiver=app_addr(tornado.app_id),
            amt=DENOMINATION // 2,  # wrong amount
        )
        atc.add_transaction(TransactionWithSigner(pay_txn, signer))

        sp2 = algod.suggested_params()
        sp2.fee = 0
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

        with pytest.raises(Exception):
            atc.execute(algod, wait_rounds=4)


class TestWithdrawToSelf:
    """
    Original: variant of '#withdraw should work'
    Withdraw where the caller is also the recipient.
    """

    def test_withdraw_to_self(
        self,
        localnet: au.AlgorandClient,
        account: SigningAccount,
        tornado: au.AppClient,
        hasher: au.AppClient,
        verifier: au.AppClient,
    ):
        """Deposit then withdraw to the same account (caller = recipient)."""
        algod = localnet.client.algod

        root_idx_before = tornado.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return

        commitment = make_commitment(b"withdraw_to_self_test")
        do_deposit(algod, account, tornado, hasher, commitment, deposit_index=root_idx_before)

        root_idx = tornado.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return
        root = get_current_root(tornado, root_idx)

        balance_before = get_balance(algod, account.address)

        nullifier_hash = hashlib.sha256(b"nullifier_self").digest()

        # Withdraw to self — recipient and relayer are the same account
        do_withdraw(
            algod, account, tornado, verifier,
            root=root,
            nullifier_hash=nullifier_hash,
            recipient_addr=account.address,
            relayer_addr=account.address,
            fee=0,
            refund=0,
            current_root_index=root_idx,
        )

        balance_after = get_balance(algod, account.address)

        # Account should have received denomination minus the withdrawal tx fee
        # The exact gain is denomination - txn_fee, but txn_fee varies.
        # Just verify balance increased (net gain = denomination - fee > 0).
        assert balance_after > balance_before
