// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function viaCalldataRef(uint256[][] calldata a) external pure returns (uint256 s) {
        for (uint i; i < a.length; i++) { uint256[] calldata x = a[i]; for (uint j; j < x.length; j++) s += x[j]; }
    }
    function viaMemCopy(uint256[][] calldata a) external pure returns (uint256 s) {
        for (uint i; i < a.length; i++) { uint256[] memory x = a[i]; for (uint j; j < x.length; j++) s += x[j]; }
    }
    function direct(uint256[][] calldata a) external pure returns (uint256 s) {
        for (uint i; i < a.length; i++) for (uint j; j < a[i].length; j++) s += a[i][j];
    }
}
