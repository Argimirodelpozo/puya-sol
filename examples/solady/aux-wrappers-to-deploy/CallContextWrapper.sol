// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/CallContextChecker.sol";

contract CallContextWrapper is CallContextChecker {
    function ping() external pure returns (uint256) {
        return 1;
    }
}
