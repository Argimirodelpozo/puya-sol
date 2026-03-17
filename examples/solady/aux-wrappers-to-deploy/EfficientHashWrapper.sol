// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/EfficientHashLib.sol";

contract EfficientHashWrapper {
    function hash(bytes32 a) external pure returns (bytes32) {
        return EfficientHashLib.hash(a);
    }

    function hash2(bytes32 a, bytes32 b) external pure returns (bytes32) {
        return EfficientHashLib.hash(a, b);
    }

    function hash3(bytes32 a, bytes32 b, bytes32 c) external pure returns (bytes32) {
        return EfficientHashLib.hash(a, b, c);
    }

    function hash4(bytes32 a, bytes32 b, bytes32 c, bytes32 d) external pure returns (bytes32) {
        return EfficientHashLib.hash(a, b, c, d);
    }
}
