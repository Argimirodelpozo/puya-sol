// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract BlockBaseFee {
    function currentBaseFee() external view returns (uint256) {
        return block.basefee;
    }
}
