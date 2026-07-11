// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function alen(address[] calldata a)      external pure returns (uint256) { return a.length; }
    function firstAddr(address[] calldata a) external pure returns (address) { return a.length > 0 ? a[0] : address(0); }
    function contains(address[] calldata a, address x) external pure returns (bool) {
        for (uint i; i < a.length; i++) if (a[i] == x) return true;
        return false;
    }
    function rev2(address[2] calldata a) external pure returns (address[2] memory r) { r[0] = a[1]; r[1] = a[0]; }
}
