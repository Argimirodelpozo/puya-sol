// SPDX-License-Identifier: MIT
// OpenZeppelin Contracts (last updated v5.0.0)
// Flattened: Context + IERC165 + ERC165 + IAccessControl + AccessControl
// Source: https://github.com/OpenZeppelin/openzeppelin-contracts/blob/v5.0.0/contracts/
// MODIFICATION: RoleData struct decomposed into flat mappings (struct-with-mapping
// not yet supported). All public API, events, errors, modifiers are EXACT OZ v5.0.0.

pragma solidity ^0.8.20;

// --- Context.sol (utils/Context.sol) ---

abstract contract Context {
    function _msgSender() internal view virtual returns (address) {
        return msg.sender;
    }

    function _msgData() internal view virtual returns (bytes calldata) {
        return msg.data;
    }
}

// --- IERC165.sol (utils/introspection/IERC165.sol) ---

interface IERC165 {
    function supportsInterface(bytes4 interfaceId) external view returns (bool);
}

// --- ERC165.sol (utils/introspection/ERC165.sol) ---

abstract contract ERC165 is IERC165 {
    function supportsInterface(bytes4 interfaceId) public view virtual returns (bool) {
        return interfaceId == type(IERC165).interfaceId;
    }
}

// --- IAccessControl.sol (access/IAccessControl.sol) ---

interface IAccessControl {
    error AccessControlUnauthorizedAccount(address account, bytes32 neededRole);
    error AccessControlBadConfirmation();

    event RoleAdminChanged(bytes32 indexed role, bytes32 indexed previousAdminRole, bytes32 indexed newAdminRole);
    event RoleGranted(bytes32 indexed role, address indexed account, address indexed sender);
    event RoleRevoked(bytes32 indexed role, address indexed account, address indexed sender);

    function hasRole(bytes32 role, address account) external view returns (bool);
    function getRoleAdmin(bytes32 role) external view returns (bytes32);
    function grantRole(bytes32 role, address account) external;
    function revokeRole(bytes32 role, address account) external;
    function renounceRole(bytes32 role, address callerConfirmation) external;
}

// --- AccessControl.sol (access/AccessControl.sol) ---
// NOTE: Original uses `struct RoleData { mapping(address => bool) hasRole; bytes32 adminRole; }`
// and `mapping(bytes32 => RoleData) private _roles;`. Decomposed to flat mappings below
// because struct-with-mapping storage is not yet supported. All behavior is identical.

abstract contract AccessControl is Context, IAccessControl, ERC165 {
    mapping(bytes32 role => mapping(address account => bool)) private _hasRole;
    mapping(bytes32 role => bytes32) private _adminRoles;

    bytes32 public constant DEFAULT_ADMIN_ROLE = 0x00;

    modifier onlyRole(bytes32 role) {
        _checkRole(role);
        _;
    }

    function supportsInterface(bytes4 interfaceId) public view virtual override returns (bool) {
        return interfaceId == type(IAccessControl).interfaceId || super.supportsInterface(interfaceId);
    }

    function hasRole(bytes32 role, address account) public view virtual returns (bool) {
        return _hasRole[role][account];
    }

    function _checkRole(bytes32 role) internal view virtual {
        _checkRole(role, _msgSender());
    }

    function _checkRole(bytes32 role, address account) internal view virtual {
        if (!hasRole(role, account)) {
            revert AccessControlUnauthorizedAccount(account, role);
        }
    }

    function getRoleAdmin(bytes32 role) public view virtual returns (bytes32) {
        return _adminRoles[role];
    }

    function grantRole(bytes32 role, address account) public virtual onlyRole(getRoleAdmin(role)) {
        _grantRole(role, account);
    }

    function revokeRole(bytes32 role, address account) public virtual onlyRole(getRoleAdmin(role)) {
        _revokeRole(role, account);
    }

    function renounceRole(bytes32 role, address callerConfirmation) public virtual {
        if (callerConfirmation != _msgSender()) {
            revert AccessControlBadConfirmation();
        }
        _revokeRole(role, callerConfirmation);
    }

    function _setRoleAdmin(bytes32 role, bytes32 adminRole) internal virtual {
        bytes32 previousAdminRole = getRoleAdmin(role);
        _adminRoles[role] = adminRole;
        emit RoleAdminChanged(role, previousAdminRole, adminRole);
    }

    function _grantRole(bytes32 role, address account) internal virtual returns (bool) {
        if (!hasRole(role, account)) {
            _hasRole[role][account] = true;
            emit RoleGranted(role, account, _msgSender());
            return true;
        } else {
            return false;
        }
    }

    function _revokeRole(bytes32 role, address account) internal virtual returns (bool) {
        if (hasRole(role, account)) {
            _hasRole[role][account] = false;
            emit RoleRevoked(role, account, _msgSender());
            return true;
        } else {
            return false;
        }
    }
}

// Test contract — standard usage (not part of OZ source)
contract AccessControlTest is AccessControl {
    constructor() {
        _grantRole(DEFAULT_ADMIN_ROLE, msg.sender);
    }
}
