"""Stateful NegRiskAdapter mock for CtfCollateralAdapter (NegRisk variant) tests.

Implements the subset of the EVM NegRiskAdapter interface that
CtfCollateralAdapter's `_splitPosition` / `_mergePositions` /
`_redeemPositions` overrides actually call:

  splitPosition(bytes32 conditionId, uint256 amount)
    Pulls `amount` USDCe from msg.sender via USDCe.transferFrom and mints
    `amount` of YES + NO position tokens on CTFMock to msg.sender.

  mergePositions(bytes32 conditionId, uint256 amount)
    Pulls YES + NO from msg.sender on CTFMock and returns `amount` USDCe.

  redeemPositions(bytes32 conditionId, uint256[] amounts)
    Pulls YES + NO from msg.sender on CTFMock based on `amounts` and pays
    out USDCe based on the registered (yes_payout, no_payout).

  wcol()
    Returns this contract's own address — `WRAPPED_COLLATERAL` in the
    real adapter is a wrapped-USDCe token; for tests we just use the
    NegRiskAdapter mock's address so position IDs derive from a stable
    value.

Storage:
  cfg                   "u" → USDCe app id (8 bytes), "c" → CTF app id
  partition             conditionId → (yesId || noId)  (64 bytes)
  payouts               conditionId → (yes_payout || no_payout) (16 bytes)

Internal arithmetic uses uint64 only; test amounts fit.
"""
import typing

from algopy import (
    ARC4Contract,
    BoxMap,
    Bytes,
    OnCompleteAction,
    OpUpFeeSource,
    UInt64,
    arc4,
    ensure_budget,
    itxn,
    op,
    subroutine,
    urange,
)


Bytes32: typing.TypeAlias = arc4.StaticArray[arc4.Byte, typing.Literal[32]]
UInt256Array: typing.TypeAlias = arc4.DynamicArray[arc4.UInt256]


@subroutine
def _amount_to_u64(amount: Bytes) -> UInt64:
    return op.btoi(op.extract(amount, UInt64(24), UInt64(8)))


@subroutine
def _u64_to_amount(amount: UInt64) -> Bytes:
    return Bytes(b"\x00" * 24) + op.itob(amount)


class NegRiskAdapterMock(ARC4Contract):

    def __init__(self) -> None:
        self.cfg = BoxMap(Bytes, Bytes, key_prefix=b"cfg_")
        self.partition = BoxMap(Bytes, Bytes, key_prefix=b"p_")
        self.payouts = BoxMap(Bytes, Bytes, key_prefix=b"po_")

    @arc4.abimethod(create="require")
    def init(self) -> None:
        pass

    @arc4.abimethod
    def configure(self, usdce_app_id: arc4.UInt64, ctf_app_id: arc4.UInt64) -> None:
        """Test-only: pin the USDCe + CTFMock app ids the mock will
        delegate to. Called once after deploy."""
        self.cfg[Bytes(b"u")] = op.itob(usdce_app_id.native)
        self.cfg[Bytes(b"c")] = op.itob(ctf_app_id.native)

    @arc4.abimethod
    def wcol(self) -> arc4.Address:
        """`WRAPPED_COLLATERAL` for the adapter — return our own address
        so position IDs derive from a stable value."""
        return arc4.Address(op.Global.current_application_address)

    @arc4.abimethod
    def prepare_condition(
        self,
        condition_id: Bytes32,
        yes_id: arc4.UInt256,
        no_id: arc4.UInt256,
    ) -> None:
        """Test-only: register the (yes_id, no_id) partition for a
        condition_id. splitPosition / mergePositions / redeemPositions
        look this up to know which CTFMock token IDs to mint/burn."""
        self.partition[condition_id.bytes] = yes_id.bytes + no_id.bytes

    @arc4.abimethod
    def report_payouts(
        self,
        condition_id: Bytes32,
        yes_payout: arc4.UInt64,
        no_payout: arc4.UInt64,
    ) -> None:
        """Test-only: record the resolved payouts for redeemPositions."""
        self.payouts[condition_id.bytes] = (
            op.itob(yes_payout.native) + op.itob(no_payout.native))

    # ── Adapter-facing API ──────────────────────────────────────────────

    @arc4.abimethod
    def splitPosition(self, condition_id: Bytes32, amount: arc4.UInt256) -> None:
        """Pull `amount` USDCe from msg.sender; mint YES+NO on CTFMock
        to msg.sender."""
        ensure_budget(40000, OpUpFeeSource.GroupCredit)
        sender = op.Txn.sender.bytes
        usdce_app_id = op.btoi(self.cfg[Bytes(b"u")])
        ctf_app_id = op.btoi(self.cfg[Bytes(b"c")])
        self_addr = op.Global.current_application_address.bytes

        # USDCe.transferFrom(sender, this, amount)
        sel_tf = arc4.arc4_signature("transferFrom(address,address,uint256)bool")
        itxn.ApplicationCall(
            app_id=usdce_app_id,
            on_completion=OnCompleteAction.NoOp,
            app_args=(sel_tf, sender, self_addr, amount.bytes),
            fee=0,
        ).submit()

        # Mint YES + NO on CTFMock to sender.
        part = self.partition[condition_id.bytes]
        yes_id = part[:32]
        no_id = part[32:]
        sel_mint = arc4.arc4_signature("mint(address,uint256,uint256)void")
        for tid in (yes_id, no_id):
            itxn.ApplicationCall(
                app_id=ctf_app_id,
                on_completion=OnCompleteAction.NoOp,
                app_args=(sel_mint, sender, tid, amount.bytes),
                fee=0,
            ).submit()

    @arc4.abimethod
    def mergePositions(self, condition_id: Bytes32, amount: arc4.UInt256) -> None:
        """Pull YES+NO from msg.sender on CTFMock; return `amount` USDCe."""
        ensure_budget(40000, OpUpFeeSource.GroupCredit)
        sender = op.Txn.sender.bytes
        usdce_app_id = op.btoi(self.cfg[Bytes(b"u")])
        ctf_app_id = op.btoi(self.cfg[Bytes(b"c")])
        self_addr = op.Global.current_application_address.bytes
        part = self.partition[condition_id.bytes]
        yes_id = part[:32]
        no_id = part[32:]

        # Pull each position from sender to this via CTFMock.safeTransferFrom.
        sel_stf = arc4.arc4_signature(
            "safeTransferFrom(address,address,uint256,uint256,byte[])void")
        for tid in (yes_id, no_id):
            itxn.ApplicationCall(
                app_id=ctf_app_id,
                on_completion=OnCompleteAction.NoOp,
                app_args=(sel_stf, sender, self_addr, tid, amount.bytes,
                          arc4.DynamicBytes(b"").bytes),
                fee=0,
            ).submit()

        # Return USDCe to sender.
        sel_t = arc4.arc4_signature("transfer(address,uint256)bool")
        itxn.ApplicationCall(
            app_id=usdce_app_id,
            on_completion=OnCompleteAction.NoOp,
            app_args=(sel_t, sender, amount.bytes),
            fee=0,
        ).submit()

    @arc4.abimethod
    def redeemPositions(
        self,
        condition_id: Bytes32,
        amounts: UInt256Array,
    ) -> None:
        """Pull YES+NO based on `amounts`, pay out USDCe based on payouts.
        Total payout = amounts[0]*yes_payout + amounts[1]*no_payout."""
        ensure_budget(40000, OpUpFeeSource.GroupCredit)
        sender = op.Txn.sender.bytes
        usdce_app_id = op.btoi(self.cfg[Bytes(b"u")])
        ctf_app_id = op.btoi(self.cfg[Bytes(b"c")])
        self_addr = op.Global.current_application_address.bytes
        part = self.partition[condition_id.bytes]
        yes_id = part[:32]
        no_id = part[32:]
        pay = self.payouts[condition_id.bytes]
        yes_pay = op.btoi(pay[:8])
        no_pay = op.btoi(pay[8:16])

        amt_yes = _amount_to_u64(amounts[0].bytes)
        amt_no = _amount_to_u64(amounts[1].bytes)
        total = amt_yes * yes_pay + amt_no * no_pay

        # Pull YES + NO if non-zero.
        sel_stf = arc4.arc4_signature(
            "safeTransferFrom(address,address,uint256,uint256,byte[])void")
        if amt_yes > UInt64(0):
            itxn.ApplicationCall(
                app_id=ctf_app_id,
                on_completion=OnCompleteAction.NoOp,
                app_args=(sel_stf, sender, self_addr, yes_id,
                          _u64_to_amount(amt_yes),
                          arc4.DynamicBytes(b"").bytes),
                fee=0,
            ).submit()
        if amt_no > UInt64(0):
            itxn.ApplicationCall(
                app_id=ctf_app_id,
                on_completion=OnCompleteAction.NoOp,
                app_args=(sel_stf, sender, self_addr, no_id,
                          _u64_to_amount(amt_no),
                          arc4.DynamicBytes(b"").bytes),
                fee=0,
            ).submit()

        # Return USDCe = total.
        sel_t = arc4.arc4_signature("transfer(address,uint256)bool")
        itxn.ApplicationCall(
            app_id=usdce_app_id,
            on_completion=OnCompleteAction.NoOp,
            app_args=(sel_t, sender, _u64_to_amount(total)),
            fee=0,
        ).submit()
