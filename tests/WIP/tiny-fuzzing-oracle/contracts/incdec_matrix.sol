// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // checked
    function cIncU8(uint8 a) external pure returns (uint8) { uint8 x=a; x++; return x; }
    function cDecU8(uint8 a) external pure returns (uint8) { uint8 x=a; x--; return x; }
    function cIncU256(uint256 a) external pure returns (uint256) { uint256 x=a; x++; return x; }
    function cDecU256(uint256 a) external pure returns (uint256) { uint256 x=a; x--; return x; }
    function cIncI16(int16 a) external pure returns (int16) { int16 x=a; x++; return x; }
    function cDecI16(int16 a) external pure returns (int16) { int16 x=a; x--; return x; }
    function cIncU64(uint64 a) external pure returns (uint64) { uint64 x=a; x++; return x; }
    function cDecU64(uint64 a) external pure returns (uint64) { uint64 x=a; x--; return x; }
    // unchecked
    function uIncU8(uint8 a) external pure returns (uint8) { unchecked { uint8 x=a; x++; return x; } }
    function uDecU8(uint8 a) external pure returns (uint8) { unchecked { uint8 x=a; x--; return x; } }
    function uIncU16(uint16 a) external pure returns (uint16) { unchecked { uint16 x=a; x++; return x; } }
    function uDecU16(uint16 a) external pure returns (uint16) { unchecked { uint16 x=a; x--; return x; } }
    function uIncU64(uint64 a) external pure returns (uint64) { unchecked { uint64 x=a; x++; return x; } }
    function uDecU64(uint64 a) external pure returns (uint64) { unchecked { uint64 x=a; x--; return x; } }
    function uIncU128(uint128 a) external pure returns (uint128) { unchecked { uint128 x=a; x++; return x; } }
    function uDecU128(uint128 a) external pure returns (uint128) { unchecked { uint128 x=a; x--; return x; } }
    function uIncU256(uint256 a) external pure returns (uint256) { unchecked { uint256 x=a; x++; return x; } }
    function uDecU256(uint256 a) external pure returns (uint256) { unchecked { uint256 x=a; x--; return x; } }
    function uIncI8(int8 a) external pure returns (int8) { unchecked { int8 x=a; x++; return x; } }
    function uDecI8(int8 a) external pure returns (int8) { unchecked { int8 x=a; x--; return x; } }
    // pre-inc/dec variants
    function uPreIncU8(uint8 a) external pure returns (uint8) { unchecked { uint8 x=a; ++x; return x; } }
    function uPreDecU8(uint8 a) external pure returns (uint8) { unchecked { uint8 x=a; --x; return x; } }
    // storage unchecked dec (state var path)
    uint128 s;
    function setS(uint128 v) external { s = v; }
    function uDecS() external returns (uint128) { unchecked { s--; } return s; }
}
