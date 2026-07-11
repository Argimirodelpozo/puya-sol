// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function incU256(uint256 a) external pure returns (uint256) { uint256 x=a; x++; return x; }
    function incU256Unchk(uint256 a) external pure returns (uint256) { unchecked { uint256 x=a; x++; return x; } }
    function incU8Unchk(uint8 a) external pure returns (uint8) { unchecked { uint8 x=a; x++; return x; } }
    function incU128Unchk(uint128 a) external pure returns (uint128) { unchecked { uint128 x=a; x++; return x; } }
    function loopU8(uint8 n) external pure returns (uint256) {
        uint256 c = 0;
        for (uint8 i = 0; i < n; i++) { c += i; }
        return c;
    }
}
