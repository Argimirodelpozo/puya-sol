// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    uint256 public cnt;
    function u(uint256 v) internal returns (uint256) { cnt++; return v; }
    function s(int256 v) internal returns (int256) { cnt++; return v; }
    // checked uint256 compound subtraction: RHS once (buildWrappingSubtract)
    function subOnce() external returns (uint256, uint256) {
        cnt = 0;
        uint256 x = 100;
        x -= u(30);
        return (x, cnt);    // (70, 1)
    }
    // signed compound mod/div: RHS once (buildSignedModDiv)
    function smodOnce() external returns (int256, uint256) {
        cnt = 0;
        int256 x = -7;
        x %= s(3);
        return (x, cnt);    // (-1, 1)
    }
    function sdivOnce() external returns (int256, uint256) {
        cnt = 0;
        int256 x = -7;
        x /= s(2);
        return (x, cnt);    // (-3, 1)
    }
    // builtin modulus arg once (addmod/mulmod z)
    function addmodOnce() external returns (uint256, uint256) {
        cnt = 0;
        uint256 r = addmod(10, 5, u(7));
        return (r, cnt);    // (1, 1)
    }
    function mulmodOnce() external returns (uint256, uint256) {
        cnt = 0;
        uint256 r = mulmod(10, 5, u(7));
        return (r, cnt);    // (1, 1)
    }
}
