"""Slot-mode packed element dispatch (EvmSlotStorageDispatch).

Surfaced while compiling a real third-party CCTP integration: its
`mapping(uint32 => mapping(address => address))` made every --evm-layout build
warn "__wb potentially used before assignment". The warning was spurious (the
buffer is always established at the first word boundary), but it showed the
dispatcher's packed multi-lane loop had no targeted coverage — the corpus
reaches it only incidentally, and an aliasing bug there would surface as a
neighbouring element quietly changing rather than as a failure.

These run in BOTH modes: the named-cell model must agree with the slot model
element for element, so a slot-dispatch bug cannot hide behind "both legs
agree" — the default mode does not use the dispatcher at all.
"""

import pytest

from framework import as_int

SOURCE = "puyasolRegression/contracts/slot_packed_dispatch.sol"

# 8 uint32 per 32-byte word: crosses a word boundary twice, and the last word
# is deliberately partial so a "rebuild the word from zero" bug shows up.
SMALLS = [1, 2, 3, 0xFFFFFFFF, 5, 6, 7, 8, 9, 10, 0x7FFFFFFF]
# exactly 2 uint128 per word, plus a partial third word
HALVES = [0, 1, (1 << 128) - 1, 12345678901234567890]


def _addr(value):
    """abi_return of an `address` normalized to comparable bytes."""
    from algosdk.encoding import decode_address
    if isinstance(value, str):
        return decode_address(value)
    return bytes(value)[-32:]


def _run(harness, extra_args):
    artifacts = harness.compile(SOURCE, extra_args=extra_args)
    app = harness.deploy(artifacts, "SlotPackedDispatch")
    acct = harness.localnet.account
    other = harness.localnet.client.account.random().address

    # ── nested mapping of address (the shape that surfaced this) ──────────
    harness.call(app, "setPeer(uint32,address,address)", 7, acct.address,
                 other)
    harness.call(app, "setPeer(uint32,address,address)", 8, acct.address,
                 acct.address)
    harness.call(app, "setSimple(address,address)", acct.address, other)

    assert _addr(harness.call(app, "peerOf(uint32,address)", 7,
                              acct.address).abi_return) == _addr(other)
    # A DIFFERENT outer key must not alias the first: the whole point of the
    # nested hash chain.
    assert _addr(harness.call(app, "peerOf(uint32,address)", 8,
                              acct.address).abi_return) == _addr(acct.address)
    # Unset entries read as the zero address, not as a neighbour's value.
    assert _addr(harness.call(app, "peerOf(uint32,address)", 9,
                              acct.address).abi_return) == bytes(32)
    assert _addr(harness.call(app, "simple(address)",
                              acct.address).abi_return) == _addr(other)

    # ── packed dynamic arrays: the loop that owns __wb ────────────────────
    for v in SMALLS:
        harness.call(app, "pushSmall(uint32)", v)
    for v in HALVES:
        harness.call(app, "pushHalf(uint128)", v)

    assert as_int(harness.call(app, "smallsLen()").abi_return) == len(SMALLS)

    # Whole-aggregate read runs the multi-lane loop; element reads take the
    # scalar path. They must agree, and both must match what was written.
    whole = [as_int(v) for v in harness.call(app, "allSmalls()").abi_return]
    assert whole == SMALLS, whole
    for i, want in enumerate(SMALLS):
        got = as_int(harness.call(app, "smallAt(uint256)", i).abi_return)
        assert got == want, f"smalls[{i}] = {got}, want {want}"

    whole_h = [as_int(v) for v in harness.call(app, "allHalves()").abi_return]
    assert whole_h == HALVES, whole_h
    for i, want in enumerate(HALVES):
        assert as_int(
            harness.call(app, "halfAt(uint256)", i).abi_return) == want

    # ── mid-word overwrite must not disturb its neighbours ────────────────
    # Index 3 sits mid-word and currently holds 0xFFFFFFFF (all bits set), so
    # a lane-offset or width error smears into 2 or 4.
    harness.call(app, "setSmallAt(uint256,uint32)", 3, 0x0A0B0C0D)
    expected = list(SMALLS)
    expected[3] = 0x0A0B0C0D
    after = [as_int(v) for v in harness.call(app, "allSmalls()").abi_return]
    assert after == expected, after

    # ── address[] : full 32-byte Algorand accounts, both read paths ───────
    # The aggregate read and the element-wise read must agree. They did not:
    # the aggregate sliced the EVM 20-byte address width out of a slot that
    # holds a whole account, zeroing its high 12 bytes. Addresses here are
    # Algorand accounts, so the whole word is the value.
    harness.call(app, "pushAddr(address)", acct.address)
    harness.call(app, "pushAddr(address)", other)
    assert _addr(harness.call(app, "addrAt(uint256)", 1).abi_return) == \
        _addr(other)
    addrs = harness.call(app, "allAddrs()").abi_return
    assert [_addr(a) for a in addrs] == [_addr(acct.address), _addr(other)]


def test_slot_packed_dispatch_default_mode(harness):
    _run(harness, None)


def test_slot_packed_dispatch_evm_layout(harness):
    _run(harness, ["--evm-layout"])
