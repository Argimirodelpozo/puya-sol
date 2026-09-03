// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {ARC4} from "libs/AVM.sol";

contract Arc4FunctionPointer {
    function encode(bytes memory data) external pure returns (bytes memory) {
        function(bytes memory) internal pure returns (bytes memory) codec =
            ARC4.encode;
        return codec(data);
    }
}
