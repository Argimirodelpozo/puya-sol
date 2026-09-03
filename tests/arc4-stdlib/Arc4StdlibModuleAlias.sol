// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "libs/AVM.sol" as Algorand;

contract Arc4StdlibModuleAlias {
    function bitLength(uint256 value) external pure returns (uint256) {
        return Algorand.Bits.bitlen(value);
    }

    function encode() external pure returns (bytes memory) {
        uint16 value = 9;
        return Algorand.ARC4.encode(abi.encode(value));
    }
}
