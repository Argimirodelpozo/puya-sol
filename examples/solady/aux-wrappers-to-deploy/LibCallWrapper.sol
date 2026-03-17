// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/LibCall.sol";

contract LibCallWrapper {
    function ping() external pure returns (uint256) {
        return 1;
    }
}
