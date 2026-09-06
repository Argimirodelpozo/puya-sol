// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract UnsupportedMappingPages {
    mapping(uint256 => uint256[1025]) private values;
    function read(uint256 k, uint256 i) external view returns (uint256) { return values[k][i]; }
}
