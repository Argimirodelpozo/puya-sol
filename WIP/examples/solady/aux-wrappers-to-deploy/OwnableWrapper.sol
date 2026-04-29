// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/auth/Ownable.sol";

contract OwnableWrapper is Ownable {
    constructor() {
        _initializeOwner(msg.sender);
    }

    function getOwner() external view returns (address) {
        return owner();
    }

    function getHandoverExpiry(address pendingOwner) external view returns (uint256) {
        return ownershipHandoverExpiresAt(pendingOwner);
    }
}
