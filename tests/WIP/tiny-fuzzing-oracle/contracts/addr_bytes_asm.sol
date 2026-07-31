// SPDX-License-Identifier: MIT
pragma solidity >=0.8.17 <0.9.0;
// Isolates ENS AddrResolver's memory-pointer asm addr<->bytes conversions.
contract AddrBytesAsm {
    // address -> 20-byte big-endian (write path: new bytes + mstore at data ptr)
    function a2b(address a) external pure returns (bytes memory b) {
        b = new bytes(20);
        assembly { mstore(add(b, 32), mul(a, exp(256, 12))) }
    }
    // 20-byte -> address (read path: mload at data ptr + shift)
    function b2a(bytes memory b) external pure returns (address payable a) {
        require(b.length == 20);
        assembly { a := div(mload(add(b, 32)), exp(256, 12)) }
    }
    // full round-trip
    function rt(address a) external pure returns (address payable) {
        bytes memory b = new bytes(20);
        assembly { mstore(add(b, 32), mul(a, exp(256, 12))) }
        require(b.length == 20);
        address payable r;
        assembly { r := div(mload(add(b, 32)), exp(256, 12)) }
        return r;
    }
}
