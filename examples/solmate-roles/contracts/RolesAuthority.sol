// SPDX-License-Identifier: AGPL-3.0-only
pragma solidity ^0.8.0;

/// @notice Role based Authority that supports up to 256 roles.
/// @author Solmate (https://github.com/transmissions11/solmate/blob/main/src/auth/authorities/RolesAuthority.sol)
/// @author Modified from Dappsys (https://github.com/dapphub/ds-roles/blob/master/src/roles.sol)
/// @dev Modifications for AVM: inlined Auth/Authority (removed cross-contract canCall),
///      simplified to owner-only auth, added explicit getters.
///      Uses original bytes32 role bitmasks and bytes4 function signature keys.
contract RolesAuthority {
    /*//////////////////////////////////////////////////////////////
                                 EVENTS
    //////////////////////////////////////////////////////////////*/

    event UserRoleUpdated(address indexed user, uint8 role, bool enabled);

    event PublicCapabilityUpdated(address indexed target, bytes4 functionSig, bool enabled);

    event RoleCapabilityUpdated(uint8 role, address indexed target, bytes4 functionSig, bool enabled);

    event OwnershipTransferred(address indexed user, address indexed newOwner);

    /*//////////////////////////////////////////////////////////////
                               CONSTRUCTOR
    //////////////////////////////////////////////////////////////*/

    address internal _owner;

    constructor() {
        _owner = msg.sender;
        emit OwnershipTransferred(msg.sender, msg.sender);
    }

    modifier requiresAuth() {
        require(msg.sender == _owner, "UNAUTHORIZED");
        _;
    }

    function owner() public view returns (address) {
        return _owner;
    }

    function transferOwnership(address newOwner) public requiresAuth {
        _owner = newOwner;
        emit OwnershipTransferred(msg.sender, newOwner);
    }

    /*//////////////////////////////////////////////////////////////
                            ROLE/USER STORAGE
    //////////////////////////////////////////////////////////////*/

    mapping(address => bytes32) internal _userRoles;

    mapping(address => mapping(bytes4 => bool)) internal _capabilityPublic;

    mapping(address => mapping(bytes4 => bytes32)) internal _rolesWithCapability;

    function getUserRoles(address user) public view returns (bytes32) {
        return _userRoles[user];
    }

    function isCapabilityPublic(address target, bytes4 functionSig) public view returns (bool) {
        return _capabilityPublic[target][functionSig];
    }

    function getRolesWithCapability(address target, bytes4 functionSig) public view returns (bytes32) {
        return _rolesWithCapability[target][functionSig];
    }

    function doesUserHaveRole(address user, uint8 role) public view returns (bool) {
        return (uint256(_userRoles[user]) >> role) & 1 != 0;
    }

    function doesRoleHaveCapability(
        uint8 role,
        address target,
        bytes4 functionSig
    ) public view returns (bool) {
        return (uint256(_rolesWithCapability[target][functionSig]) >> role) & 1 != 0;
    }

    /*//////////////////////////////////////////////////////////////
                           AUTHORIZATION LOGIC
    //////////////////////////////////////////////////////////////*/

    function canCall(
        address user,
        address target,
        bytes4 functionSig
    ) public view returns (bool) {
        if (_capabilityPublic[target][functionSig]) {
            return true;
        }
        bytes32 roles = _userRoles[user];
        bytes32 required = _rolesWithCapability[target][functionSig];
        return roles & required != bytes32(0);
    }

    /*//////////////////////////////////////////////////////////////
                   ROLE CAPABILITY CONFIGURATION LOGIC
    //////////////////////////////////////////////////////////////*/

    function setPublicCapability(
        address target,
        bytes4 functionSig,
        bool enabled
    ) public requiresAuth {
        _capabilityPublic[target][functionSig] = enabled;

        emit PublicCapabilityUpdated(target, functionSig, enabled);
    }

    function setRoleCapability(
        uint8 role,
        address target,
        bytes4 functionSig,
        bool enabled
    ) public requiresAuth {
        if (enabled) {
            _rolesWithCapability[target][functionSig] |= bytes32(1 << role);
        } else {
            _rolesWithCapability[target][functionSig] &= ~bytes32(1 << role);
        }

        emit RoleCapabilityUpdated(role, target, functionSig, enabled);
    }

    /*//////////////////////////////////////////////////////////////
                       USER ROLE ASSIGNMENT LOGIC
    //////////////////////////////////////////////////////////////*/

    function setUserRole(
        address user,
        uint8 role,
        bool enabled
    ) public requiresAuth {
        if (enabled) {
            _userRoles[user] |= bytes32(1 << role);
        } else {
            _userRoles[user] &= ~bytes32(1 << role);
        }

        emit UserRoleUpdated(user, role, enabled);
    }
}
