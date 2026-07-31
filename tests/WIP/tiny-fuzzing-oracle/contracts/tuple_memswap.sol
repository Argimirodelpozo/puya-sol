// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract MemSwap {
    // uint256 memory array swap (isolates eval-order from arc4.bool)
    function uSwap(uint256 a, uint256 b) external pure returns (uint256, uint256) {
        uint256[] memory m = new uint256[](2); m[0]=a; m[1]=b;
        (m[0], m[1]) = (m[1], m[0]);
        return (m[0], m[1]);
    }
    // bool memory array swap
    function bSwap(bool a, bool b) external pure returns (bool, bool) {
        bool[] memory m = new bool[](2); m[0]=a; m[1]=b;
        (m[0], m[1]) = (m[1], m[0]);
        return (m[0], m[1]);
    }
    // uint8 memory array swap (sub-word, closer to bool)
    function u8Swap(uint8 a, uint8 b) external pure returns (uint8, uint8) {
        uint8[] memory m = new uint8[](2); m[0]=a; m[1]=b;
        (m[0], m[1]) = (m[1], m[0]);
        return (m[0], m[1]);
    }
}
