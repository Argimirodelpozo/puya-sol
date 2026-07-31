// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// OZ EnumerableSet.values() pointer-pun, reduced. `result := store` type-puns a
// bytes32[] memory pointer into an address[]/uint256[] of identical layout.
library Pun {
    function raw(uint256 n) internal pure returns (bytes32[] memory s) {
        s = new bytes32[](n);
        for (uint256 i = 0; i < n; i++) s[i] = bytes32(uint256(i + 1));
    }
    function asAddrs(uint256 n) internal pure returns (address[] memory result) {
        bytes32[] memory store = raw(n);
        assembly { result := store }
    }
    function asUints(uint256 n) internal pure returns (uint256[] memory result) {
        bytes32[] memory store = raw(n);
        assembly { result := store }
    }
}

contract EnumSetValues {
    function countAddrs(uint256 n) external pure returns (uint256) {
        require(n <= 12);
        return Pun.asAddrs(n).length;
    }
    function sumUints(uint256 n) external pure returns (uint256) {
        require(n <= 12);
        uint256[] memory v = Pun.asUints(n);
        uint256 t;
        for (uint256 i = 0; i < v.length; i++) t += v[i];
        return t;
    }
    function firstUint(uint256 n) external pure returns (uint256) {
        require(n >= 1 && n <= 12);
        return Pun.asUints(n)[0];
    }
    function lastUint(uint256 n) external pure returns (uint256) {
        require(n >= 1 && n <= 12);
        uint256[] memory v = Pun.asUints(n);
        return v[v.length - 1];
    }
}
