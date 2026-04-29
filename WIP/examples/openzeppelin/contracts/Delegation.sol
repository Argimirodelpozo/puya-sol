// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Delegation {
    address private _admin;
    uint256 private _userCount;
    uint256 private _totalPower;

    mapping(address => uint256) internal _power;
    mapping(address => address) internal _delegateTo;
    mapping(address => uint256) internal _delegatedPower;
    mapping(address => uint256) internal _userIndex;
    mapping(address => bool) internal _isRegistered;

    constructor() {
        _admin = msg.sender;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getUserCount() external view returns (uint256) {
        return _userCount;
    }

    function getTotalPower() external view returns (uint256) {
        return _totalPower;
    }

    function getPower(address user) external view returns (uint256) {
        return _power[user];
    }

    function getDelegateTo(address user) external view returns (address) {
        return _delegateTo[user];
    }

    function getDelegatedPower(address user) external view returns (uint256) {
        return _delegatedPower[user];
    }

    function getEffectivePower(address user) external view returns (uint256) {
        if (_delegateTo[user] != address(0)) {
            return _delegatedPower[user];
        }
        return _power[user] + _delegatedPower[user];
    }

    function isRegistered(address user) external view returns (bool) {
        return _isRegistered[user];
    }

    function register(address user, uint256 power) external {
        require(!_isRegistered[user], "already registered");
        _isRegistered[user] = true;
        _power[user] = power;
        _userCount = _userCount + 1;
        _userIndex[user] = _userCount;
        _totalPower = _totalPower + power;
    }

    function delegate(address from, address to) external {
        require(_isRegistered[from], "from not registered");
        require(_isRegistered[to], "to not registered");
        require(_delegateTo[from] == address(0), "already delegating");
        require(from != to, "cannot self-delegate");
        _delegateTo[from] = to;
        _delegatedPower[to] = _delegatedPower[to] + _power[from];
    }

    function undelegate(address user) external {
        address currentDelegate = _delegateTo[user];
        require(currentDelegate != address(0), "not delegating");
        _delegatedPower[currentDelegate] = _delegatedPower[currentDelegate] - _power[user];
        _delegateTo[user] = address(0);
    }

    function setPower(address user, uint256 newPower) external {
        require(msg.sender == _admin, "not admin");
        require(_isRegistered[user], "not registered");
        uint256 oldPower = _power[user];
        _power[user] = newPower;
        if (newPower > oldPower) {
            _totalPower = _totalPower + (newPower - oldPower);
        } else {
            _totalPower = _totalPower - (oldPower - newPower);
        }
        address currentDelegate = _delegateTo[user];
        if (currentDelegate != address(0)) {
            if (newPower > oldPower) {
                _delegatedPower[currentDelegate] = _delegatedPower[currentDelegate] + (newPower - oldPower);
            } else {
                _delegatedPower[currentDelegate] = _delegatedPower[currentDelegate] - (oldPower - newPower);
            }
        }
    }
}

contract DelegationTest is Delegation {
    constructor() Delegation() {}

    function initUser(address user) external {
        _power[user] = 0;
        _delegateTo[user] = address(0);
        _delegatedPower[user] = 0;
        _userIndex[user] = 0;
        _isRegistered[user] = false;
    }
}
