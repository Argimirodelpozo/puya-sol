// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract Cee {
    uint256 public s;
    modifier m() { s += 1; _; }
    // signed negatives through modifier (single + tuple + mixed dyn)
    function msig(int256 a) external m returns (int64) { return int64(a); }
    function mwide(int256 a) external m returns (int128) { return int128(a); }
    function mtup(int256 a) external m returns (int64, int128) { return (int64(a), int128(a)); }
    function mmix(int256 a) external m returns (int32, uint64, uint256) { return (int32(a), uint64(uint256(a)), uint256(a)); }
    function mdyn(int256 a) external m returns (int128, bytes memory) { bytes memory b = new bytes(1); b[0] = 0x07; return (int128(a), b); }
    // non-modifier signed dyn-tuple (residual a control, no modifier)
    function pdyn(int256 a) external pure returns (int128, bytes memory) { bytes memory b = new bytes(1); b[0]=0x09; return (int128(a), b); }
}
contract Cer {
    Cee c;
    constructor() { c = new Cee(); }
    function gsig(int256 a) external returns (int256) { return int256(c.msig(a)); }
    function gwide(int256 a) external returns (int256) { return int256(c.mwide(a)); }
    function gtup(int256 a) external returns (int256) { (int64 x, int128 y) = c.mtup(a); return int256(x) + int256(y); }
    function gmix(int256 a) external returns (int256) { (int32 x, uint64 y, uint256 z) = c.mmix(a); return int256(x) + int256(uint256(y)) + int256(z); }
    function gmdyn(int256 a) external returns (int256) { (int128 x, bytes memory b) = c.mdyn(a); return int256(x) + int256(uint256(uint8(b[0]))); }
    function gpdyn(int256 a) external returns (int256) { (int128 x, bytes memory b) = c.pdyn(a); return int256(x) + int256(uint256(uint8(b[0]))); }
}
