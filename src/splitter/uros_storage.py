"""puya-sol --uros-splitter __storage contract template.

This algopy contract holds the SPLIT contract's state. Its default
bytecode is a thin admit-update receiver that only accepts
UpdateApplication calls from the orchestrator.

Per call, the orchestrator:
  1. installs a chunk's bytecode on this contract via UpdateApplication
  2. inner-calls this contract with the user's selector + args
       — chunk runs the real method body, mutates state on this app
  3. restores this contract's bytecode to the default (admit-update)

State (boxes, app-global) lives on this app. Chunks reference state
via app_global_get / box_get just like a normal compiled contract.

## Why a separate storage contract?

The previous design swapped chunks onto the main contract itself, so
inner-txn callers (Spoke→main.foo) couldn't trigger the dance —
inner txns can't read the outer group context. Splitting into
main+__storage+orch lets main's stubs do the dance via inner txns
themselves: main inner-calls orch.dispatch which inner-installs
chunk on __storage, inner-calls __storage, inner-restores.

## Initialization

AppCreate runs `init()`, which sets `orch_app_id = 0` (placeholder).
`set_orch(orch_app_id)` pins the authority. After that, any
UpdateApplication on this contract must come from a NoOp inner txn
sent by the orch app's address — enforced by the
`__delegate_update()` selector check. The actual program-bytes swap
is the inner txn's `approval_program` field; the body of this
default bytecode is irrelevant to the dance once the chunk is in.
"""

from algopy import (
    ARC4Contract,
    Account,
    Application,
    Global,
    OnCompleteAction,
    Txn,
    UInt64,
    arc4,
    op,
)


# `__delegate_update()void` ABI selector: sha512_256 of the sig, [:4].
# The orch sends inner UpdateApplication txns with this selector so the
# dance's swap-in/swap-out land on a known entry point.
DELEGATE_UPDATE_SELECTOR = b"\xdc\x5e\x37\x98"


class UrosStorage(ARC4Contract):
    """State holder for split contracts. Default bytecode admits
    UpdateApplication from orch and rejects everything else."""

    def __init__(self) -> None:
        # Pinned at deploy time via set_orch(). UpdateApplication only
        # accepted when sender == orch_app's address.
        self.orch_app_id = UInt64(0)

    @arc4.abimethod(create="require")
    def init(self) -> None:
        """One-time AppCreate anchor."""

    @arc4.abimethod
    def set_orch(self, orch_app_id: UInt64) -> None:
        """Pin which orchestrator app may swap in chunks. Call once
        right after AppCreate.

        Note: __storage's own __init__ never ran here — AppCreate was
        for main's bytecode (so the user contract's state-var inits
        run on __storage). Then the harness UpdateApplications to
        this thin default. So orch_app_id starts uninitialized; we
        just write it without checking — the deployer's first call
        sets it. After this, only the pinned orch can UpdateApplication
        per __delegate_update's sender check."""
        self.orch_app_id = orch_app_id

    @arc4.abimethod(allow_actions=["UpdateApplication"])
    def __delegate_update(self) -> None:  # noqa: N801
        """The only path through which a chunk can be installed. Caller
        must be the pinned orch app's address; any other sender is
        rejected. The new program bytes ride on the inner txn's
        approval_program field — this method's body never reads them."""
        sender = Txn.sender
        orch_addr = Application(self.orch_app_id).address
        assert sender == orch_addr, "uros_storage: only orch may update"
