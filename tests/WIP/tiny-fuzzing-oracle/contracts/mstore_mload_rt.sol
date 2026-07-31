// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
// Purest form: asm mstore to a `new bytes` buffer, then asm mload of the same
// buffer in the same function. No exp, no address math.
contract MstoreMloadRT {
    function rtSameBlock(uint256 v) external pure returns (uint256 r) {
        bytes memory b = new bytes(32);
        assembly { mstore(add(b, 32), v)  r := mload(add(b, 32)) }
    }
    function rtTwoBlocks(uint256 v) external pure returns (uint256 r) {
        bytes memory b = new bytes(32);
        assembly { mstore(add(b, 32), v) }
        assembly { r := mload(add(b, 32)) }
    }
}
