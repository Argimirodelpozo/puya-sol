// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/auth/TimedRoles.sol";

contract TimedRolesWrapper is TimedRoles {
    address private _owner;

    constructor() {
        _owner = msg.sender;
    }

    function owner() public view returns (address) {
        return _owner;
    }

    function checkTimedRole(address holder, uint256 role) external view returns (bool isActive, uint40 start, uint40 expires) {
        return timedRoleActive(holder, role);
    }
}
