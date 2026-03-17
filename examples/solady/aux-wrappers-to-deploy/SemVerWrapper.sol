// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/SemVerLib.sol";

contract SemVerWrapper {
    function cmp(bytes32 a, bytes32 b) external pure returns (int256) {
        return SemVerLib.cmp(a, b);
    }
}
