// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/auth/EnumerableRoles.sol";

contract EnumerableRolesWrapper is EnumerableRoles {
    address private _owner;

    constructor() {
        _owner = msg.sender;
    }

    function owner() public view returns (address) {
        return _owner;
    }

    function checkRole(address user, uint256 role) external view returns (bool) {
        return hasRole(user, role);
    }
}
