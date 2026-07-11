// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract C {
    function paramOff(uint256 off, uint256 v) external pure returns (uint256 r) {
        off = off % 256;
        assembly { mstore(off, v) r := mload(off) }   // should return v
    }
    function paramOffAdd(uint256 off, uint256 v) external pure returns (uint256 r) {
        off = off % 256;
        assembly { mstore(add(off, 0), v) r := mload(off) }
    }
    function constOff(uint256 v) external pure returns (uint256 r) {
        assembly { mstore(64, v) r := mload(64) }    // control: works
    }
    function letOff(uint256 o, uint256 v) external pure returns (uint256 r) {
        assembly { let off := mod(o, 256) mstore(off, v) r := mload(off) }  // control: works
    }
}
