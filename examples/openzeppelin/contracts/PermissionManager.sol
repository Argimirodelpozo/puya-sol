// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Role-based permission system.
 * Admin creates roles identified by a hash, then grants or revokes
 * permissions (also identified by hash) per role.
 * Nested mapping simulated via key = roleId * 1000000 + permHash.
 */
abstract contract PermissionManager {
    address private _admin;
    uint256 private _roleCount;
    uint256 private _totalGrants;

    mapping(uint256 => uint256) internal _roleHash;
    mapping(uint256 => bool) internal _permissions;

    constructor() {
        _admin = msg.sender;
        _roleCount = 0;
        _totalGrants = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getRoleCount() external view returns (uint256) {
        return _roleCount;
    }

    function getTotalGrants() external view returns (uint256) {
        return _totalGrants;
    }

    function getRoleHash(uint256 roleId) external view returns (uint256) {
        return _roleHash[roleId];
    }

    function hasPermission(uint256 roleId, uint256 permHash) external view returns (bool) {
        uint256 permKey = roleId * 1000000 + permHash;
        return _permissions[permKey];
    }

    function createRole(uint256 roleHash) external returns (uint256) {
        require(msg.sender == _admin, "Not admin");
        uint256 id = _roleCount;
        _roleHash[id] = roleHash;
        _roleCount = id + 1;
        return id;
    }

    function grantPermission(uint256 roleId, uint256 permHash) external {
        require(msg.sender == _admin, "Not admin");
        require(roleId < _roleCount, "Role does not exist");
        uint256 permKey = roleId * 1000000 + permHash;
        require(!_permissions[permKey], "Permission already granted");
        _permissions[permKey] = true;
        _totalGrants = _totalGrants + 1;
    }

    function revokePermission(uint256 roleId, uint256 permHash) external {
        require(msg.sender == _admin, "Not admin");
        require(roleId < _roleCount, "Role does not exist");
        uint256 permKey = roleId * 1000000 + permHash;
        require(_permissions[permKey], "Permission not granted");
        _permissions[permKey] = false;
        _totalGrants = _totalGrants - 1;
    }
}

contract PermissionManagerTest is PermissionManager {
    constructor() PermissionManager() {}

    function initRole(uint256 roleId) external {
        _roleHash[roleId] = 0;
    }

    function initPermission(uint256 permKey) external {
        _permissions[permKey] = false;
    }
}
