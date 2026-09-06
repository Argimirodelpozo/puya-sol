// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract UnsupportedPageAlias {
    uint256[1025] private values;
    function read(uint256 i) external view returns (uint256) { return readAlias(values, i); }
    function readAlias(uint256[1025] storage a, uint256 i) internal view returns (uint256) { return a[i]; }
}
