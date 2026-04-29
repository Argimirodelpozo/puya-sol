// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/auth/OwnableRoles.sol";

contract OwnableRolesWrapper is OwnableRoles {
    constructor() {
        _initializeOwner(msg.sender);
    }

    function getOwner() external view returns (address) {
        return owner();
    }

    function grantRole(address user, uint256 roles) external onlyOwner {
        _grantRoles(user, roles);
    }

    function revokeRole(address user, uint256 roles) external onlyOwner {
        _removeRoles(user, roles);
    }

    function checkRoles(address user, uint256 roles) external view returns (bool) {
        return hasAllRoles(user, roles);
    }

    function checkAnyRole(address user, uint256 roles) external view returns (bool) {
        return hasAnyRole(user, roles);
    }

    function getUserRoles(address user) external view returns (uint256) {
        return rolesOf(user);
    }
}
