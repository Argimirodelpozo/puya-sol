// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// A PAYABLE function that internally calls a NON-PAYABLE public one. The
// non-payable guard reads a TRANSACTION-level fact (a preceding payment) but
// lives in the method BODY, which an internal `callsub` shares — so the payable
// caller's own legitimate payment tripped the callee's guard. friend.tech's
// `buyShares` (payable) calls `getPrice` (public view) and reverted on every
// buy that carried value. The guard now only fires when the ROUTER dispatched
// that method, which ApplicationArgs[0] identifies.

contract PayableCallsNonPayable {
    uint256 public last;

    /// public + non-payable: carries the guard, and is called internally below
    function price(uint256 n) public pure returns (uint256) { return n * 3; }

    /// payable, and calls the guarded function internally
    function buy(uint256 n) external payable returns (uint256) {
        uint256 p = price(n);
        last = p;
        return p;
    }

    /// must STILL reject value — the guard has to survive for real external calls
    function setLast(uint256 n) external { last = n; }
}
