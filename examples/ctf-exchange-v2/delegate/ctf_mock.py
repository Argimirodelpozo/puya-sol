"""Stateful CTF (ConditionalTokens / ERC1155-ish) mock for matchOrders body
tests. Selectors must exactly match the inner-call sites — so address args
are arc4.Address (byte[32]) and id/amount/tokenId are arc4.UInt256.

Storage:
  balances              BoxMap[account||tokenId (64B), uint256 (32B)]
  approvals_for_all     BoxMap[owner||operator (64B), bool (1B)]
  yes_no_partition      BoxMap[conditionId (32B), (yesId, noId) (64B)]
                          — registered via prepare_condition; matchOrders'
                            MINT/MERGE paths call splitPosition /
                            mergePositions which look up the partition by
                            conditionId.

Internal arithmetic uses low 64 bits only; test amounts are well below 2^64.
"""
import typing

from algopy import (
    ARC4Contract,
    BoxMap,
    Bytes,
    UInt64,
    arc4,
    op,
    subroutine,
)


# 32-byte byte string used for conditionId / parent (which Solidity uses as
# bytes32 — semantically a hash, not an address).
Bytes32: typing.TypeAlias = arc4.StaticArray[arc4.Byte, typing.Literal[32]]
UInt256Array: typing.TypeAlias = arc4.DynamicArray[arc4.UInt256]


@subroutine
def _amount_to_u64(amount: Bytes) -> UInt64:
    return op.btoi(op.extract(amount, UInt64(24), UInt64(8)))


@subroutine
def _u64_to_amount(amount: UInt64) -> Bytes:
    return Bytes(b"\x00" * 24) + op.itob(amount)


@subroutine
def _empty_balance() -> Bytes:
    return Bytes(b"\x00" * 32)


@subroutine
def _bal_key(account: Bytes, token_id: Bytes) -> Bytes:
    """32-byte hashed key = sha256(account address || tokenId).
    Hashed because 32+32=64B + b_ prefix exceeds the 64B box-name cap."""
    return op.sha256(account + token_id)


@subroutine
def _approval_key(owner: Bytes, operator: Bytes) -> Bytes:
    """32-byte hashed key = sha256(owner || operator). Same reason."""
    return op.sha256(owner + operator)


class CTFMock(ARC4Contract):
    """ERC1155 + minimal CTF (ConditionalTokens) op shape mock."""

    def __init__(self) -> None:
        self.balances = BoxMap(Bytes, Bytes, key_prefix=b"b_")
        self.approvals = BoxMap(Bytes, Bytes, key_prefix=b"ap_")
        # condId → (yesId || noId), 64 bytes total
        self.partition = BoxMap(Bytes, Bytes, key_prefix=b"p_")

    @arc4.abimethod(create="require")
    def init(self) -> None:
        pass

    # ── Test-only setup ────────────────────────────────────────────────

    @arc4.abimethod
    def prepare_condition(
        self,
        condition_id: Bytes32,
        yes_id: arc4.UInt256,
        no_id: arc4.UInt256,
    ) -> None:
        """Test-only: register the (yesId, noId) partition for a
        conditionId. matchOrders' MINT/MERGE path will look this up."""
        self.partition[condition_id.bytes] = yes_id.bytes + no_id.bytes

    @arc4.abimethod
    def mint(self, to: arc4.Address, token_id: arc4.UInt256, amount: arc4.UInt256) -> None:
        """Test-only deal()."""
        key = _bal_key(to.bytes, token_id.bytes)
        cur = _amount_to_u64(self.balances.get(key, default=_empty_balance()))
        self.balances[key] = _u64_to_amount(cur + _amount_to_u64(amount.bytes))

    # ── ERC1155 read-only ──────────────────────────────────────────────

    @arc4.abimethod
    def balanceOf(self, account: arc4.Address, id: arc4.UInt256) -> arc4.UInt256:
        raw = self.balances.get(_bal_key(account.bytes, id.bytes), default=_empty_balance())
        return arc4.UInt256.from_bytes(raw)

    @arc4.abimethod
    def isApprovedForAll(
        self, account: arc4.Address, operator: arc4.Address
    ) -> arc4.Bool:
        raw = self.approvals.get(_approval_key(account.bytes, operator.bytes),
                                 default=Bytes(b"\x00"))
        return arc4.Bool(raw[:1] != Bytes(b"\x00"))

    # ── ERC1155 write ──────────────────────────────────────────────────

    @arc4.abimethod
    def setApprovalForAll(self, operator: arc4.Address, approved: arc4.Bool) -> None:
        sender = op.Txn.sender.bytes
        self.approvals[_approval_key(sender, operator.bytes)] = (
            Bytes(b"\x01") if approved.native else Bytes(b"\x00"))

    @arc4.abimethod
    def setApproval(
        self, owner: arc4.Address, operator: arc4.Address, approved: arc4.Bool
    ) -> None:
        """Test-only: set `owner`'s setApprovalForAll(operator, approved)
        directly. See USDCMock.setAllowance for rationale."""
        self.approvals[_approval_key(owner.bytes, operator.bytes)] = (
            Bytes(b"\x01") if approved.native else Bytes(b"\x00"))

    @arc4.abimethod
    def safeTransferFrom(
        self,
        from_: arc4.Address,
        to: arc4.Address,
        id: arc4.UInt256,
        amount: arc4.UInt256,
        data: arc4.DynamicBytes,
    ) -> None:
        """Standard ERC1155 transfer. Either msg.sender == from, or
        from has setApprovalForAll(msg.sender, true)."""
        sender = op.Txn.sender.bytes
        if sender != from_.bytes:
            ap_raw = self.approvals.get(_approval_key(from_.bytes, sender),
                                        default=Bytes(b"\x00"))
            assert ap_raw[:1] == Bytes(b"\x01"), "not approved operator"
        amt = _amount_to_u64(amount.bytes)
        from_key = _bal_key(from_.bytes, id.bytes)
        bal = _amount_to_u64(self.balances.get(from_key, default=_empty_balance()))
        assert bal >= amt, "insufficient ERC1155 balance"
        self.balances[from_key] = _u64_to_amount(bal - amt)
        to_key = _bal_key(to.bytes, id.bytes)
        cur_to = _amount_to_u64(self.balances.get(to_key, default=_empty_balance()))
        self.balances[to_key] = _u64_to_amount(cur_to + amt)

    # ── ConditionalTokens MINT/MERGE ───────────────────────────────────
    #
    # CTF's `splitPosition(collateralToken, parent, conditionId, partition,
    # amount)` semantics: msg.sender's collateral (USDC) is consumed and
    # in exchange they get `amount` of YES + `amount` of NO. matchOrders'
    # MINT path then transfers those to the makers/taker.
    #
    # `mergePositions` is the reverse: msg.sender's YES + NO are burned,
    # they get back `amount` of collateral.
    #
    # The `collateralToken` arg is ignored here. The mock just mints the
    # outcome tokens (split) or burns them (merge); collateral movement
    # is verified by the test on the USDC mock side.

    @arc4.abimethod
    def splitPosition(
        self,
        collateralToken: arc4.Address,
        parent: Bytes32,
        condition_id: Bytes32,
        partition: UInt256Array,
        amount: arc4.UInt256,
    ) -> None:
        sender = op.Txn.sender.bytes
        amt = _amount_to_u64(amount.bytes)
        # Look up YES/NO ids for this conditionId.
        part = self.partition[condition_id.bytes]
        yes_id = part[:32]
        no_id = part[32:]
        # Mint amount of YES + NO to sender.
        for tid in (yes_id, no_id):
            key = _bal_key(sender, tid)
            cur = _amount_to_u64(self.balances.get(key, default=_empty_balance()))
            self.balances[key] = _u64_to_amount(cur + amt)

    @arc4.abimethod
    def mergePositions(
        self,
        collateralToken: arc4.Address,
        parent: Bytes32,
        condition_id: Bytes32,
        partition: UInt256Array,
        amount: arc4.UInt256,
    ) -> None:
        sender = op.Txn.sender.bytes
        amt = _amount_to_u64(amount.bytes)
        part = self.partition[condition_id.bytes]
        yes_id = part[:32]
        no_id = part[32:]
        for tid in (yes_id, no_id):
            key = _bal_key(sender, tid)
            bal = _amount_to_u64(self.balances.get(key, default=_empty_balance()))
            assert bal >= amt, "insufficient outcome token to merge"
            self.balances[key] = _u64_to_amount(bal - amt)
