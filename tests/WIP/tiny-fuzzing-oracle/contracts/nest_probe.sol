// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    // 1-level dynamic (works): baseline
    function flat(uint256[] calldata a) external pure returns (uint256 s) {
        for (uint i; i < a.length; i++) s += a[i];
    }
    // nested dynamic outer.length only (no inner access)
    function outerLen(uint256[][] calldata a) external pure returns (uint256) { return a.length; }
    // nested dynamic inner.length (one level of indirection)
    function innerLen(uint256[][] calldata a) external pure returns (uint256) {
        return a.length == 0 ? 0 : a[0].length;
    }
    // full nested sum
    function nested(uint256[][] calldata a) external pure returns (uint256 s) {
        for (uint i; i < a.length; i++) for (uint j; j < a[i].length; j++) s += a[i][j];
    }
}
