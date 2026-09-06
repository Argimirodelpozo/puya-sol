// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract UnsupportedInteriorArrayReference {
    mapping(uint256 => uint256[129][2]) private values;
    function read(uint256 i) external view returns (uint256) { return readAlias(values[0][1], i); }
    function readAlias(uint256[129] storage a, uint256 i) internal view returns (uint256) { return a[i]; }
}
