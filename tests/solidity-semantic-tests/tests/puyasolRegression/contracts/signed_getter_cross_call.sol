// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Guard: a SIGNED public state var's getter kept a bare biguint return, so
// puya's router published `received()uint512` while callers (and the arc56
// spec) compute `received()uint256` -- the cross-contract read then fell to
// the callee's FALLBACK (empty return log -> caller extraction panic), or
// err'd without one. The getter return must remap to arc4.uint256 (canonical
// 256-bit TC) for signed widths, incl. int256.
contract holder {
    int24 public small;
    int256 public wide;
    fallback() external { small = 99; }
    function set(int24 a, int256 b) public { small = a; wide = b; }
}
contract reader {
    holder h;
    constructor() { h = new holder(); }
    function set(int24 a, int256 b) public { h.set(a, b); }
    function readSmall() public returns (int24) { return h.small(); }
    function readWide() public returns (int256) { return h.wide(); }
}
