// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract RipemdBoundaries {
    function hash(bytes memory data) external pure returns (bytes20) {
        return ripemd160(data);
    }

    function twice(bytes memory a, bytes memory b) external pure returns (bytes20, bytes20) {
        return (ripemd160(a), ripemd160(b));
    }
}
