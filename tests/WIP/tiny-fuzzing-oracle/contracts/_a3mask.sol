// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract M {
    enum E { A, B, C }
    function u8(uint256 a) external pure returns (uint8) { return uint8(a); }
    function u16(uint256 a) external pure returns (uint16) { return uint16(a); }
    function u24(uint256 a) external pure returns (uint24) { return uint24(a); }
    function u32(uint256 a) external pure returns (uint32) { return uint32(a); }
    function en(uint256 a) external pure returns (E) { return E(a % 3); }
    function tup(uint256 a) external pure returns (uint32, uint128) { return (uint32(a), uint128(a)); }  // mask + biguint tuple
    function tup2(uint256 a) external pure returns (uint8, uint16, uint256) { return (uint8(a), uint16(a), a); }
    function nmask(uint256 a) external pure returns (uint24 r) { r = uint24(a); }  // named implicit, sub-word
}
