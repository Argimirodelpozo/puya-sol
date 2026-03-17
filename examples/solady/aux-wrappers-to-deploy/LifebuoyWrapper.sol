// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/Lifebuoy.sol";

contract LifebuoyWrapper is Lifebuoy {
    function ping() external pure returns (uint256) {
        return 42;
    }
}
