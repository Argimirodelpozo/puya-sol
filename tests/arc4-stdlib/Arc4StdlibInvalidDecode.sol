// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {ARC4} from "libs/AVM.sol";

contract Arc4StdlibInvalidDecode {
    function decode(bytes memory data) external pure returns (bytes memory) {
        return ARC4.decode(data);
    }
}
