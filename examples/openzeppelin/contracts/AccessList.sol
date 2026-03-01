// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract AccessList {
    address public admin;
    uint256 public memberCount;
    uint256 public roleCount;

    mapping(address => bool) internal _hasRole;
    mapping(address => uint256) internal _roleExpiry;
    mapping(address => uint256) internal _roleLevel;
    mapping(address => uint256) internal _memberIndex;

    constructor() {
        admin = msg.sender;
    }

    function grantRole(address member, uint256 level, uint256 expiry) public {
        require(msg.sender == admin, "Not admin");
        _hasRole[member] = true;
        _roleLevel[member] = level;
        _roleExpiry[member] = expiry;
        if (_memberIndex[member] == 0) {
            memberCount = memberCount + 1;
            _memberIndex[member] = memberCount;
        }
    }

    function revokeRole(address member) public {
        require(msg.sender == admin, "Not admin");
        _hasRole[member] = false;
    }

    function isActive(address member, uint256 currentTime) public view returns (bool) {
        bool hasRoleFlag = _hasRole[member];
        uint256 expiry = _roleExpiry[member];
        return hasRoleFlag && (expiry == 0 || currentTime < expiry);
    }

    function hasRole(address member) public view returns (bool) {
        return _hasRole[member];
    }

    function getRoleLevel(address member) public view returns (uint256) {
        return _roleLevel[member];
    }

    function getRoleExpiry(address member) public view returns (uint256) {
        return _roleExpiry[member];
    }

    function getMemberCount() public view returns (uint256) {
        return memberCount;
    }

    function getAdmin() public view returns (address) {
        return admin;
    }

    function setRoleLevel(address member, uint256 newLevel) public {
        require(msg.sender == admin, "Not admin");
        _roleLevel[member] = newLevel;
    }

    function extendExpiry(address member, uint256 newExpiry) public {
        require(msg.sender == admin, "Not admin");
        _roleExpiry[member] = newExpiry;
    }
}

contract AccessListTest is AccessList {
    constructor() AccessList() {}

    function initMember(address member) public {
        _hasRole[member] = false;
        _roleExpiry[member] = 0;
        _roleLevel[member] = 0;
        _memberIndex[member] = 0;
    }
}
