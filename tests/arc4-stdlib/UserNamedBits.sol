// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Declaration-based matching must not intercept a same-named user library.
library Bits {
    function bitlen(uint256 value) internal pure returns (uint256) {
        return value + 1;
    }
}

contract UserNamedBits {
    function bitLength(uint256 value) external pure returns (uint256) {
        return Bits.bitlen(value);
    }
}
