// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function sarComp(int128 a, uint256 n) external pure returns (int128) { int128 x=a; x >>= (n % 130); return x; }
    function shlComp(int128 a, uint256 n) external pure returns (int128) { int128 x=a; x <<= (n % 130); return x; }
    function modComp(int128 a, int128 b) external pure returns (int128) { int128 x=a; x %= b; return x; }
    function incChecked(int8 a) external pure returns (int8) { int8 x=a; x++; return x; }   // reverts at int8.max
    function decChecked(int8 a) external pure returns (int8) { int8 x=a; x--; return x; }   // reverts at int8.min
    function incU8(uint8 a) external pure returns (uint8) { uint8 x=a; x++; return x; }      // reverts at 255
    function sarU128(uint128 a, uint256 n) external pure returns (uint128) { uint128 x=a; x >>= (n % 130); return x; }
    function shlU64(uint64 a, uint256 n) external pure returns (uint64) { uint64 x=a; x <<= (n % 70); return x; }
    function andComp(int64 a, int64 b) external pure returns (int64) { int64 x=a; x &= b; return x; }
    function xorComp(uint256 a, uint256 b) external pure returns (uint256) { uint256 x=a; x ^= b; return x; }
}
