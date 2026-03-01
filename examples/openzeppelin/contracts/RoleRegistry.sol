// SPDX-License-Identifier: MIT
// Role-based registry with expiration and hierarchical roles
// Tests: bytes32 keys, block.timestamp comparisons, complex boolean logic

pragma solidity ^0.8.20;

error RoleRegistryUnauthorized(address caller);
error RoleRegistryAlreadyGranted(address account, bytes32 role);
error RoleRegistryNotGranted(address account, bytes32 role);
error RoleRegistryExpired(address account, bytes32 role);
error RoleRegistryInvalidExpiry(uint256 expiry);

contract RoleRegistry {
    event RoleGranted(bytes32 indexed role, address indexed account, uint256 expiry);
    event RoleRevoked(bytes32 indexed role, address indexed account);
    event AdminTransferred(address indexed previousAdmin, address indexed newAdmin);

    bytes32 public constant ADMIN_ROLE = keccak256("ADMIN_ROLE");
    bytes32 public constant OPERATOR_ROLE = keccak256("OPERATOR_ROLE");
    bytes32 public constant MINTER_ROLE = keccak256("MINTER_ROLE");

    address private _admin;

    // role => account => hasRole (uses keccak256(role, account) as flat key)
    mapping(bytes32 => bool) private _roles;

    // role => account => expiry timestamp
    mapping(bytes32 => uint256) private _roleExpiry;

    // Count of active role assignments per role
    mapping(bytes32 => uint256) private _roleMemberCount;

    constructor() {
        _admin = msg.sender;
    }

    modifier onlyAdmin() {
        if (msg.sender != _admin) {
            revert RoleRegistryUnauthorized(msg.sender);
        }
        _;
    }

    function grantRole(bytes32 role, address account, uint256 expiry) public onlyAdmin {
        if (expiry != 0 && expiry <= block.timestamp) {
            revert RoleRegistryInvalidExpiry(expiry);
        }

        bytes32 key = _roleKey(role, account);

        if (_roles[key]) {
            revert RoleRegistryAlreadyGranted(account, role);
        }

        _roles[key] = true;
        _roleExpiry[key] = expiry;
        _roleMemberCount[role] += 1;

        emit RoleGranted(role, account, expiry);
    }

    function revokeRole(bytes32 role, address account) public onlyAdmin {
        bytes32 key = _roleKey(role, account);

        if (!_roles[key]) {
            revert RoleRegistryNotGranted(account, role);
        }

        _roles[key] = false;
        _roleExpiry[key] = 0;
        _roleMemberCount[role] -= 1;

        emit RoleRevoked(role, account);
    }

    function renounceRole(bytes32 role) public {
        bytes32 key = _roleKey(role, msg.sender);

        if (!_roles[key]) {
            revert RoleRegistryNotGranted(msg.sender, role);
        }

        _roles[key] = false;
        _roleExpiry[key] = 0;
        _roleMemberCount[role] -= 1;

        emit RoleRevoked(role, msg.sender);
    }

    function transferAdmin(address newAdmin) public onlyAdmin {
        address oldAdmin = _admin;
        _admin = newAdmin;
        emit AdminTransferred(oldAdmin, newAdmin);
    }

    // --- View functions ---

    function hasRole(bytes32 role, address account) public view returns (bool) {
        bytes32 key = _roleKey(role, account);
        if (!_roles[key]) {
            return false;
        }
        uint256 expiry = _roleExpiry[key];
        if (expiry != 0 && expiry <= block.timestamp) {
            return false;  // expired
        }
        return true;
    }

    function hasActiveRole(bytes32 role, address account) public view returns (bool) {
        return hasRole(role, account);
    }

    function getRoleExpiry(bytes32 role, address account) public view returns (uint256) {
        bytes32 key = _roleKey(role, account);
        return _roleExpiry[key];
    }

    function getRoleMemberCount(bytes32 role) public view returns (uint256) {
        return _roleMemberCount[role];
    }

    function getAdmin() public view returns (address) {
        return _admin;
    }

    function isAdmin(address account) public view returns (bool) {
        return account == _admin;
    }

    // --- Internal ---

    function _roleKey(bytes32 role, address account) private pure returns (bytes32) {
        return keccak256(abi.encodePacked(role, account));
    }
}
