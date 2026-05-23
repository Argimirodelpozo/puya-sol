"""Stateful USDC-like ERC20 mock for matchOrders body tests.

Responds to the ABI shape puya-sol generates from `IERC20(token).transfer(...)`
and `transferFrom(...)` calls. Selectors must exactly match the inner-call
sites — that means typed `address` (32-byte byte[] in ARC-4) and `uint256`
(also 32-byte) args, NOT bare `byte[]` (which would compute different
selectors).

Storage:
  balances    BoxMap[address (32B), uint256 (32B)]
  allowances  BoxMap[owner||spender (64B), uint256 (32B)]

Internal arithmetic uses only the low 64 bits of the uint256 amount —
test values are ≤ 1e8 so this fits, and avoids the AVM bigint machinery.
"""
from algopy import (
    ARC4Contract,
    BoxMap,
    Bytes,
    UInt64,
    arc4,
    op,
    subroutine,
)


@subroutine
def _amount_to_u64(amount: Bytes) -> UInt64:
    """Treat the low 8 bytes of a 32-byte ARC-4 uint256 as the actual
    amount. Test values are well below 2^64; upper 24 bytes must be zero."""
    return op.btoi(op.extract(amount, UInt64(24), UInt64(8)))


@subroutine
def _u64_to_amount(amount: UInt64) -> Bytes:
    """Encode a UInt64 as 32-byte big-endian uint256."""
    return Bytes(b"\x00" * 24) + op.itob(amount)


@subroutine
def _empty_balance() -> Bytes:
    return Bytes(b"\x00" * 32)


class USDCMock(ARC4Contract):
    """ERC20-like stateful mock. Each balance/allowance is a separate box
    keyed by the account address(es)."""

    def __init__(self) -> None:
        self.balances = BoxMap(Bytes, Bytes, key_prefix=b"b_")
        self.allowances = BoxMap(Bytes, Bytes, key_prefix=b"a_")

    @arc4.abimethod(create="require")
    def init(self) -> None:
        """No-op init so the create txn carries an ABI selector."""

    # ── Test-only helper: mint into account ─────────────────────────────

    @arc4.abimethod
    def mint(self, to: arc4.Address, amount: arc4.UInt256) -> None:
        """Test-only: credit `to` with `amount` directly. Mirrors the
        Foundry `deal()` cheat. No allowance / approval needed."""
        to_b = to.bytes
        amt_b = amount.bytes
        cur = self.balances.get(to_b, default=_empty_balance())
        new_amt = _amount_to_u64(cur) + _amount_to_u64(amt_b)
        self.balances[to_b] = _u64_to_amount(new_amt)

    @arc4.abimethod
    def setAllowance(
        self, owner: arc4.Address, spender: arc4.Address, amount: arc4.UInt256
    ) -> None:
        """Test-only: set `owner`'s allowance for `spender` to `amount`.
        Foundry's `vm.prank(maker); usdc.approve(exchange, amt)` has no
        AVM equivalent because maker is an eth-style identity without an
        algorand private key. Use this cheat instead."""
        self.allowances[op.sha256(owner.bytes + spender.bytes)] = amount.bytes

    # ── ERC20 read-only ─────────────────────────────────────────────────

    @arc4.abimethod
    def balanceOf(self, account: arc4.Address) -> arc4.UInt256:
        raw = self.balances.get(account.bytes, default=_empty_balance())
        return arc4.UInt256.from_bytes(raw)

    @arc4.abimethod
    def allowance(self, owner: arc4.Address, spender: arc4.Address) -> arc4.UInt256:
        raw = self.allowances.get(op.sha256(owner.bytes + spender.bytes), default=_empty_balance())
        return arc4.UInt256.from_bytes(raw)

    # ── ERC20 write ─────────────────────────────────────────────────────

    @arc4.abimethod
    def approve(self, spender: arc4.Address, amount: arc4.UInt256) -> arc4.Bool:
        """msg.sender approves spender for amount."""
        sender = op.Txn.sender.bytes
        self.allowances[op.sha256(sender + spender.bytes)] = amount.bytes
        return arc4.Bool(True)

    @arc4.abimethod
    def transfer(self, to: arc4.Address, amount: arc4.UInt256) -> arc4.Bool:
        """Move tokens from msg.sender to `to`."""
        sender = op.Txn.sender.bytes
        amt = _amount_to_u64(amount.bytes)
        bal = _amount_to_u64(self.balances.get(sender, default=_empty_balance()))
        assert bal >= amt, "insufficient balance"
        self.balances[sender] = _u64_to_amount(bal - amt)
        cur_to = _amount_to_u64(self.balances.get(to.bytes, default=_empty_balance()))
        self.balances[to.bytes] = _u64_to_amount(cur_to + amt)
        return arc4.Bool(True)

    @arc4.abimethod
    def transferFrom(
        self, owner: arc4.Address, to: arc4.Address, amount: arc4.UInt256
    ) -> arc4.Bool:
        """Standard ERC20 transferFrom — checks allowance for msg.sender
        as the spender, decrements both allowance and `owner`'s balance,
        credits `to`."""
        spender = op.Txn.sender.bytes
        amt = _amount_to_u64(amount.bytes)
        all_key = op.sha256(owner.bytes + spender)
        cur_allow = _amount_to_u64(self.allowances.get(all_key, default=_empty_balance()))
        assert cur_allow >= amt, "insufficient allowance"
        self.allowances[all_key] = _u64_to_amount(cur_allow - amt)
        bal = _amount_to_u64(self.balances.get(owner.bytes, default=_empty_balance()))
        assert bal >= amt, "insufficient balance"
        self.balances[owner.bytes] = _u64_to_amount(bal - amt)
        cur_to = _amount_to_u64(self.balances.get(to.bytes, default=_empty_balance()))
        self.balances[to.bytes] = _u64_to_amount(cur_to + amt)
        return arc4.Bool(True)
