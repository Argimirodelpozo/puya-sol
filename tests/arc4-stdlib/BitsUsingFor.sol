// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {Bits} from "libs/AVM.sol";

using Bits for uint256;

contract BitsUsingFor {
    function bitLength(uint256 value) external pure returns (uint256) {
        return value.bitlen();
    }
}
