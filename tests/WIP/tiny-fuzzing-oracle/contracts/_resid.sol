// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract Cee {
    uint256 public s;
    modifier m() { s += 1; _; }
    // (a) DYNAMIC-element tuple with biguint element
    function tdyn(uint256 a) external pure returns (uint128, bytes memory) {
        bytes memory b = new bytes(2); b[0] = 0xaa; b[1] = 0xbb;
        return (uint128(a), b);
    }
    // (b) MODIFIER'D biguint returns (chain-lowered; single + tuple)
    function mret(uint256 a) external m returns (uint128) { return uint128(a); }
    function mtup(uint256 a) external m returns (uint64, uint128) { return (uint64(a), uint128(a)); }
    // controls
    function pret(uint256 a) external pure returns (uint128) { return uint128(a); }  // no modifier: known-good
}
contract Cer {
    Cee c;
    constructor() { c = new Cee(); }
    function gtdyn(uint256 a) external returns (uint256) { (uint128 x, bytes memory b) = c.tdyn(a); return uint256(x) + uint256(uint8(b[0])) + b.length; }
    function gmret(uint256 a) external returns (uint256) { return uint256(c.mret(a)); }
    function gmtup(uint256 a) external returns (uint256) { (uint64 x, uint128 y) = c.mtup(a); return uint256(x) + uint256(y); }
    function gpret(uint256 a) external returns (uint256) { return uint256(c.pret(a)); }
}
