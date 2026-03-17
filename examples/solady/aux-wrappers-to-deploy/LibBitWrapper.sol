// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/LibBit.sol";

contract LibBitWrapper {
    function fls(uint256 x) external pure returns (uint256) {
        return LibBit.fls(x);
    }

    function ffs(uint256 x) external pure returns (uint256) {
        return LibBit.ffs(x);
    }

    function popCount(uint256 x) external pure returns (uint256) {
        return LibBit.popCount(x);
    }

    function isPo2(uint256 x) external pure returns (bool) {
        return LibBit.isPo2(x);
    }

    function reverseBits(uint256 x) external pure returns (uint256) {
        return LibBit.reverseBits(x);
    }

    function reverseBytes(uint256 x) external pure returns (uint256) {
        return LibBit.reverseBytes(x);
    }
}
