"""
Tests for MerkleTreeWithHistory contract (via MerkleTreeWithHistoryMock).

Translated from tornado-core/test/MerkleTreeWithHistory.test.js.
Tests the Merkle tree structure used by Tornado Cash for tracking deposits.
"""
import hashlib

import algokit_utils as au
from algosdk import encoding
from algokit_utils.models.account import SigningAccount
import pytest

from conftest import deploy_contract, fund_contract, box_key, load_arc56


# --- Constants matching the Solidity contract ---

FIELD_SIZE = 21888242871839275222246405745257275088548364400416034343698204186575808495617
ZERO_VALUE = 21663839004416932945382355908790599225266501822907911457504978515578255421292
ROOT_HISTORY_SIZE = 30

# Precomputed zero values from Tornado Cash (first few)
ZEROS = [
    0x2fe54c60d3acabf3343a35b6eba15db4821b340f76e741e2249685ed4899af6c,
    0x256a6135777eee2fd26f54b8b7037a25439d5235caee224154186d2b8a52e31d,
    0x1151949895e82ab19924de92c40a3d6f7bcb60d92b00504b8199613683f0c200,
    0x20121ee811489ff8d61f09fb89e313f14959a0f28bb428a20dba6b0b068b3bdb,
]


def int_key(i: int) -> bytes:
    """Mapping key for integer indices — compiler uses itob (8-byte big-endian)."""
    return i.to_bytes(8, "big")


NO_POPULATE = au.SendParams(populate_app_call_resources=False)


def do_insert(
    merkle_tree: au.AppClient,
    hasher: au.AppClient,
    leaf: bytes,
    current_next_index: int,
):
    """Insert a leaf into the Merkle tree with proper box refs and fees."""
    current_root_idx = current_next_index  # after init, rootIdx tracks insertions
    next_root_idx = current_next_index + 1

    box_refs = [
        au.BoxReference(app_id=merkle_tree.app_id, name=box_key("filledSubtrees", int_key(i)))
        for i in range(4)
    ] + [
        au.BoxReference(app_id=merkle_tree.app_id, name=box_key("roots", int_key(current_root_idx))),
        au.BoxReference(app_id=merkle_tree.app_id, name=box_key("roots", int_key(next_root_idx % ROOT_HISTORY_SIZE))),
    ]

    merkle_tree.send.call(
        au.AppClientMethodCallParams(
            method="insert",
            args=[leaf],
            box_references=box_refs,
            app_references=[hasher.app_id],
            extra_fee=au.AlgoAmount(micro_algo=8 * 1000),
        ),
        send_params=NO_POPULATE,
    )


def get_root(merkle_tree: au.AppClient, root_idx: int) -> bytes:
    """Read a root from the tree's history."""
    result = merkle_tree.send.call(
        au.AppClientMethodCallParams(
            method="getLastRoot",
            box_references=[
                au.BoxReference(
                    app_id=merkle_tree.app_id,
                    name=box_key("roots", int_key(root_idx)),
                ),
            ],
        ),
        send_params=NO_POPULATE,
    )
    return bytes(result.abi_return)


@pytest.fixture(scope="module")
def hasher(localnet: au.AlgorandClient, account: SigningAccount) -> au.AppClient:
    """Deploy the HasherMock contract."""
    return deploy_contract(localnet, account, "HasherMock")


@pytest.fixture(scope="module")
def merkle_tree(
    localnet: au.AlgorandClient,
    account: SigningAccount,
    hasher: au.AppClient,
) -> au.AppClient:
    """Deploy MerkleTreeWithHistoryMock with tree height = 4."""
    client = deploy_contract(localnet, account, "MerkleTreeWithHistoryMock",
                             fund_amount=5_000_000)
    hasher_addr = b'\x00' * 24 + hasher.app_id.to_bytes(8, "big")
    client.send.call(
        au.AppClientMethodCallParams(
            method="__postInit",
            args=[4, hasher_addr],
            box_references=[
                au.BoxReference(app_id=client.app_id, name=box_key("filledSubtrees", int_key(i)))
                for i in range(4)
            ] + [
                au.BoxReference(app_id=client.app_id, name=box_key("roots", int_key(0))),
            ],
        ),
        send_params=NO_POPULATE,
    )
    return client


class TestZeros:
    """Test the zeros() pure function that returns precomputed empty tree values."""

    def test_zeros_index_0(self, merkle_tree: au.AppClient):
        result = merkle_tree.send.call(
            au.AppClientMethodCallParams(method="zeros", args=[0])
        )
        assert list(result.abi_return) == list(ZEROS[0].to_bytes(32, "big"))

    def test_zeros_index_1(self, merkle_tree: au.AppClient):
        result = merkle_tree.send.call(
            au.AppClientMethodCallParams(method="zeros", args=[1])
        )
        assert list(result.abi_return) == list(ZEROS[1].to_bytes(32, "big"))

    def test_zeros_index_2(self, merkle_tree: au.AppClient):
        result = merkle_tree.send.call(
            au.AppClientMethodCallParams(method="zeros", args=[2])
        )
        assert list(result.abi_return) == list(ZEROS[2].to_bytes(32, "big"))

    def test_zeros_index_3(self, merkle_tree: au.AppClient):
        result = merkle_tree.send.call(
            au.AppClientMethodCallParams(method="zeros", args=[3])
        )
        assert list(result.abi_return) == list(ZEROS[3].to_bytes(32, "big"))


class TestTreeState:
    """Test initial tree state after construction."""

    def test_initial_levels(self, merkle_tree: au.AppClient):
        result = merkle_tree.send.call(
            au.AppClientMethodCallParams(method="levels")
        )
        assert result.abi_return == 4

    def test_initial_next_index(self, merkle_tree: au.AppClient):
        result = merkle_tree.send.call(
            au.AppClientMethodCallParams(method="nextIndex")
        )
        assert result.abi_return == 0

    def test_initial_current_root_index(self, merkle_tree: au.AppClient):
        result = merkle_tree.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        )
        assert result.abi_return == 0


class TestInsert:
    """
    Translated from '#insert' in MerkleTreeWithHistory.test.js.
    Tests sequential insertions and root updates.
    """

    def test_insert_increments_next_index(
        self, hasher: au.AppClient, merkle_tree: au.AppClient,
    ):
        """Insert first leaf — nextIndex should go from 0 to 1."""
        leaf = (1).to_bytes(32, "big")
        do_insert(merkle_tree, hasher, leaf, current_next_index=0)

        result = merkle_tree.send.call(
            au.AppClientMethodCallParams(method="nextIndex")
        )
        assert result.abi_return == 1

    def test_get_last_root_changes_after_insert(
        self, merkle_tree: au.AppClient,
    ):
        """Root should be non-zero after insertion."""
        root_idx = merkle_tree.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return

        root = get_root(merkle_tree, root_idx)
        assert root != b'\x00' * 32

    def test_insert_multiple_leaves(
        self, hasher: au.AppClient, merkle_tree: au.AppClient,
    ):
        """
        Original: 'should correctly insert all leaves'
        Insert several more leaves, each producing a new unique root.
        """
        roots_seen = set()

        # Capture root from first insert
        root = get_root(merkle_tree, 1)
        roots_seen.add(root)

        # Insert leaves 2 through 5 (nextIndex goes from 1 to 5)
        for i in range(1, 5):
            leaf = (i + 1).to_bytes(32, "big")
            do_insert(merkle_tree, hasher, leaf, current_next_index=i)

            root_idx = merkle_tree.send.call(
                au.AppClientMethodCallParams(method="currentRootIndex")
            ).abi_return

            root = get_root(merkle_tree, root_idx)
            # Each insertion should produce a unique root
            assert root not in roots_seen, f"Duplicate root after insert {i+1}"
            roots_seen.add(root)

        result = merkle_tree.send.call(
            au.AppClientMethodCallParams(method="nextIndex")
        )
        assert result.abi_return == 5


class TestIsKnownRoot:
    """
    Translated from '#isKnownRoot' in MerkleTreeWithHistory.test.js.
    Tests root history lookup after insertions.
    """

    def test_current_root_is_known(self, merkle_tree: au.AppClient):
        """The most recent root should be known."""
        root_idx = merkle_tree.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return

        root = get_root(merkle_tree, root_idx)

        # Provide refs for roots that isKnownRoot will scan
        box_refs = [
            au.BoxReference(app_id=merkle_tree.app_id, name=box_key("roots", int_key(i)))
            for i in range(root_idx + 1)
        ]
        result = merkle_tree.send.call(
            au.AppClientMethodCallParams(
                method="isKnownRoot",
                args=[root],
                box_references=box_refs,
            ),
            send_params=NO_POPULATE,
        )
        assert result.abi_return is True

    def test_old_root_still_known(self, merkle_tree: au.AppClient):
        """
        Original: 'should find an old root'
        A previous root (not the latest) should still be in history.
        """
        # Root from the very first insert (index 1)
        old_root = get_root(merkle_tree, 1)

        current_root_idx = merkle_tree.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return

        box_refs = [
            au.BoxReference(app_id=merkle_tree.app_id, name=box_key("roots", int_key(i)))
            for i in range(current_root_idx + 1)
        ]
        result = merkle_tree.send.call(
            au.AppClientMethodCallParams(
                method="isKnownRoot",
                args=[old_root],
                box_references=box_refs,
            ),
            send_params=NO_POPULATE,
        )
        assert result.abi_return is True

    def test_unknown_root_returns_false(self, merkle_tree: au.AppClient):
        """
        Original: 'should not find an uninitialized root'
        A random root that was never in the tree should not be known.
        """
        fake_root = hashlib.sha256(b"not_a_real_root").digest()

        current_root_idx = merkle_tree.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return

        box_refs = [
            au.BoxReference(app_id=merkle_tree.app_id, name=box_key("roots", int_key(i)))
            for i in range(current_root_idx + 1)
        ]
        result = merkle_tree.send.call(
            au.AppClientMethodCallParams(
                method="isKnownRoot",
                args=[fake_root],
                box_references=box_refs,
            ),
            send_params=NO_POPULATE,
        )
        assert result.abi_return is False

    def test_zero_root_not_known(self, merkle_tree: au.AppClient):
        """Zero root always returns false (early exit, no box access)."""
        zero_root = b'\x00' * 32
        result = merkle_tree.send.call(
            au.AppClientMethodCallParams(
                method="isKnownRoot",
                args=[zero_root],
            ),
        )
        assert result.abi_return is False


class TestTreeFull:
    """
    Original: '#insert should not let you go over the tree size'
    Fill a tree of height 4 (capacity = 2^4 = 16), then try inserting one more.
    Uses a separate small tree fixture to avoid polluting the shared merkle_tree.
    """

    @pytest.fixture(scope="class")
    def small_tree(
        self, localnet: au.AlgorandClient, account: SigningAccount, hasher: au.AppClient,
    ) -> au.AppClient:
        """Deploy a fresh tree just for the full-tree test."""
        client = deploy_contract(
            localnet, account, "MerkleTreeWithHistoryMock", fund_amount=10_000_000,
        )
        hasher_addr = b'\x00' * 24 + hasher.app_id.to_bytes(8, "big")
        client.send.call(
            au.AppClientMethodCallParams(
                method="__postInit",
                args=[4, hasher_addr],
                box_references=[
                    au.BoxReference(app_id=client.app_id, name=box_key("filledSubtrees", int_key(i)))
                    for i in range(4)
                ] + [
                    au.BoxReference(app_id=client.app_id, name=box_key("roots", int_key(0))),
                ],
            ),
            send_params=NO_POPULATE,
        )
        return client

    def test_insert_16_then_reject_17th(
        self,
        hasher: au.AppClient,
        small_tree: au.AppClient,
    ):
        """Insert 16 leaves (fills the tree), then verify the 17th is rejected."""
        # Insert 16 leaves — tree height 4 has capacity 2^4 = 16
        for i in range(16):
            do_insert(small_tree, hasher, (i + 1).to_bytes(32, "big"), current_next_index=i)

        # Verify tree is full
        result = small_tree.send.call(
            au.AppClientMethodCallParams(method="nextIndex")
        )
        assert result.abi_return == 16

        # 17th insert should fail: "Merkle tree is full. No more leaves can be added"
        with pytest.raises(Exception):
            do_insert(small_tree, hasher, (17).to_bytes(32, "big"), current_next_index=16)


class TestRootHistoryWrapping:
    """
    Test that root history circular buffer works correctly when wrapping.
    After ROOT_HISTORY_SIZE (30) inserts, old roots should be evicted and
    no longer recognized by isKnownRoot.
    """

    @pytest.fixture(scope="class")
    def wrap_tree(
        self, localnet: au.AlgorandClient, account: SigningAccount, hasher: au.AppClient,
    ) -> au.AppClient:
        """Deploy a tree with height 5 (capacity 32) so we can exceed ROOT_HISTORY_SIZE=30."""
        client = deploy_contract(
            localnet, account, "MerkleTreeWithHistoryMock", fund_amount=20_000_000,
        )
        hasher_addr = b'\x00' * 24 + hasher.app_id.to_bytes(8, "big")

        box_refs = [
            au.BoxReference(app_id=client.app_id, name=box_key("filledSubtrees", int_key(i)))
            for i in range(5)
        ] + [
            au.BoxReference(app_id=client.app_id, name=box_key("roots", int_key(0))),
        ]

        client.send.call(
            au.AppClientMethodCallParams(
                method="__postInit",
                args=[5, hasher_addr],
                box_references=box_refs,
            ),
            send_params=NO_POPULATE,
        )
        return client

    def _do_insert_h5(self, tree, hasher, leaf, current_next_index):
        """Insert for height-5 tree (5 filledSubtrees instead of 4)."""
        current_root_idx = current_next_index
        next_root_idx = current_next_index + 1

        box_refs = [
            au.BoxReference(app_id=tree.app_id, name=box_key("filledSubtrees", int_key(i)))
            for i in range(5)
        ] + [
            au.BoxReference(app_id=tree.app_id, name=box_key("roots", int_key(current_root_idx % ROOT_HISTORY_SIZE))),
            au.BoxReference(app_id=tree.app_id, name=box_key("roots", int_key(next_root_idx % ROOT_HISTORY_SIZE))),
        ]

        tree.send.call(
            au.AppClientMethodCallParams(
                method="insert",
                args=[leaf],
                box_references=box_refs,
                app_references=[hasher.app_id],
                extra_fee=au.AlgoAmount(micro_algo=10 * 1000),
            ),
            send_params=NO_POPULATE,
        )

    def test_old_root_evicted_after_wrapping(
        self,
        hasher: au.AppClient,
        wrap_tree: au.AppClient,
    ):
        """Insert 31 leaves so the circular buffer wraps; root from insert #1 should be evicted."""
        # Capture root after first insert (slot 1)
        self._do_insert_h5(wrap_tree, hasher, (1).to_bytes(32, "big"), current_next_index=0)
        first_root = get_root(wrap_tree, 1)

        # Insert 30 more leaves (indices 1..30), total 31 inserts
        # After 31 inserts, currentRootIndex = 31, which maps to slot 31 % 30 = 1
        # This overwrites slot 1 with a new root, evicting first_root
        for i in range(1, 31):
            self._do_insert_h5(wrap_tree, hasher, (i + 1).to_bytes(32, "big"), current_next_index=i)

        # currentRootIndex stores the slot index (mod ROOT_HISTORY_SIZE)
        # After 31 inserts: slot = 31 % 30 = 1
        current_root_idx = wrap_tree.send.call(
            au.AppClientMethodCallParams(method="currentRootIndex")
        ).abi_return
        assert current_root_idx == 31 % ROOT_HISTORY_SIZE  # slot 1

        # The current root should be known
        current_root = get_root(wrap_tree, current_root_idx)
        result = wrap_tree.send.call(
            au.AppClientMethodCallParams(
                method="isKnownRoot",
                args=[current_root],
                box_references=[
                    au.BoxReference(app_id=wrap_tree.app_id,
                                    name=box_key("roots", int_key(current_root_idx))),
                ],
            ),
            send_params=NO_POPULATE,
        )
        assert result.abi_return is True

        # first_root was in slot 1, which got overwritten by insert #31.
        # It should no longer be known.
        # Provide root refs for the loop to search through (it goes backward).
        search_refs = [
            au.BoxReference(app_id=wrap_tree.app_id, name=box_key("roots", int_key(i)))
            for i in range(8)  # max 8 box refs
        ]
        result = wrap_tree.send.call(
            au.AppClientMethodCallParams(
                method="isKnownRoot",
                args=[first_root],
                box_references=search_refs,
            ),
            send_params=NO_POPULATE,
        )
        assert result.abi_return is False
