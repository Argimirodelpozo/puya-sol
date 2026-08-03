// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// `return <void external call>;` — legal Solidity in a function with no return
// values, and how forwarding wrappers are written (Polymarket NegRiskAdapter's
// `return ctf.safeTransferFrom(...)`). The call must still EXECUTE: carrying it
// as the return VALUE handed puya an inner-txn handle where a stack value
// belongs ("itxn_group_idx cannot be mapped to AVM stack type").

contract Sink {
    uint256 public v;
    function set(uint256 x) external { v = x; }
}

contract Forwarder {
    Sink public s;
    constructor() { s = new Sink(); }

    /// the shape under test
    function fwd(uint256 x) external { return s.set(x); }
    /// same, from a plain expression statement — must stay equivalent
    function plain(uint256 x) external { s.set(x); }

    function read() external view returns (uint256) { return s.v(); }
}
