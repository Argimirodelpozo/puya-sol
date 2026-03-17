// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/accounts/Receiver.sol";

contract ReceiverWrapper is Receiver {
    function ping() external pure returns (uint256) {
        return 1;
    }
}
