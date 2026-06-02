// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/// AVM transaction-group inspection. Bodies are stubs; puya-sol intercepts the
/// calls by library name (see AsaIntrinsics.cpp dispatchGroup/dispatchTxn).
library Group {
    function size() internal view returns (uint64) { revert("puya-sol"); }
    function index() internal view returns (uint64) { revert("puya-sol"); }
    function txnApplicationId(uint64 idx) internal view returns (uint64) { idx; revert("puya-sol"); }
}

library Txn {
    function applicationId() internal view returns (uint64) { revert("puya-sol"); }
}

/// @title FlashAccounting — Uniswap-V4-style flash accounting WITHOUT the
/// unlock-callback re-entrancy that the AVM forbids.
///
/// On EVM, V4 opens a transient lock, calls back into the integrator, and the
/// integrator RE-ENTERS the PoolManager (swap/take/settle/...) before a final
/// net-zero check. The callback+re-entrancy only exist because EVM has no
/// native atomic multi-call. The AVM does: an atomic transaction GROUP.
///
/// So the same guarantee is expressed directly as a group of top-level calls:
///
///     [ unlock(), op(+x), op(-y), settle(z), ... ]   // all to THIS app
///
/// No method ever calls back into this app, so this app never appears twice in
/// the call stack — no re-entrancy. Deltas accumulate in global state (visible
/// to later txns in the same group), and whichever op is LAST asserts the net
/// delta is zero, reverting the whole atomic group if anything is unsettled.
contract FlashAccounting {
    bool private unlocked;
    int256 private netDelta; // net owed across the open group; must end at 0

    /// Guard for in-group operations: require the lock is open, run the body,
    /// then run the trailing settlement check if this is the group's last txn.
    modifier flash() {
        require(unlocked, "LOCKED");
        _;
        _settleIfLast();
    }

    /// Open the lock (group txn 0 by convention). The group's LAST txn must be
    /// a call to THIS app, so the trailing net-zero check is guaranteed to run
    /// — you cannot append a non-app txn to escape settlement.
    function unlock() external {
        require(!unlocked, "ALREADY_UNLOCKED");
        require(netDelta == 0, "DIRTY_START");
        uint64 n = Group.size();
        require(Group.txnApplicationId(n - 1) == Txn.applicationId(), "LAST_NOT_SELF");
        unlocked = true;
        _settleIfLast();
    }

    /// Simulate swap / modifyLiquidity creating a balance delta ("on credit").
    function op(int256 delta) external flash {
        netDelta += delta;
    }

    /// Simulate paying down what is owed.
    function settle(int256 amount) external flash {
        netDelta -= amount;
    }

    /// Explicit no-op settlement point (typically the last txn of a group).
    function finalize() external flash {}

    /// Whichever op is LAST in the group asserts the net delta is zero and
    /// closes the lock. If unsettled, the require reverts the atomic group and
    /// every earlier op is undone — exactly V4's net-zero invariant.
    function _settleIfLast() internal {
        if (Group.index() == Group.size() - 1) {
            require(netDelta == 0, "UNSETTLED");
            unlocked = false;
        }
    }

    function isUnlocked() external view returns (bool) { return unlocked; }
    function getNetDelta() external view returns (int256) { return netDelta; }
}
