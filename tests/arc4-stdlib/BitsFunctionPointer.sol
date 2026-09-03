// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {Bits} from "libs/AVM.sol";

contract BitsFunctionPointer {
    function bitLength(uint256 value) external pure returns (uint256) {
        function(uint256) internal pure returns (uint256) fn = Bits.bitlen;
        return fn(value);
    }
}
