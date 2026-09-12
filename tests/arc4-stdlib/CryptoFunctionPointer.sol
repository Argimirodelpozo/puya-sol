// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {Crypto} from "libs/AVM.sol";

contract CryptoFunctionPointer {
    function hash(bytes memory value) external pure returns (bytes32) {
        function(bytes memory) internal pure returns (bytes32) fn = Crypto.sha3_256;
        return fn(value);
    }
}
