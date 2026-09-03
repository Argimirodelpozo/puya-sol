// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// This is intentionally not the library from libs/AVM.sol. Declaration-based
// matching must leave it as an ordinary Solidity library call.
library ARC4 {
    function encode(bytes memory data) internal pure returns (bytes memory) {
        return data;
    }
}

contract UserNamedArc4 {
    function encodeEvm(uint16 value) external pure returns (bytes memory) {
        return ARC4.encode(abi.encode(value));
    }
}
