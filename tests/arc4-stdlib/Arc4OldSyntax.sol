// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract Arc4OldSyntax {
    function encode(uint16 value) external pure returns (bytes memory) {
        return arc4.encode(value);
    }
}
