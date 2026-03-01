// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Allowance {
    address private _admin;
    uint256 private _accountCount;
    uint256 private _totalSpent;
    uint256 private _totalBudget;

    mapping(address => uint256) internal _limit;
    mapping(address => uint256) internal _spent;
    mapping(address => uint256) internal _accountIndex;
    mapping(address => bool) internal _isActive;

    constructor() {
        _admin = msg.sender;
        _accountCount = 0;
        _totalSpent = 0;
        _totalBudget = 0;
    }

    function admin() external view returns (address) {
        return _admin;
    }

    function accountCount() external view returns (uint256) {
        return _accountCount;
    }

    function totalSpent() external view returns (uint256) {
        return _totalSpent;
    }

    function totalBudget() external view returns (uint256) {
        return _totalBudget;
    }

    function getLimit(address account) external view returns (uint256) {
        return _limit[account];
    }

    function getSpent(address account) external view returns (uint256) {
        return _spent[account];
    }

    function isActive(address account) external view returns (bool) {
        return _isActive[account];
    }

    function getRemaining(address account) external view returns (uint256) {
        return _limit[account] - _spent[account];
    }

    function setupAccount(address account, uint256 limit) external {
        require(!_isActive[account], "Account already active");
        _accountCount = _accountCount + 1;
        _accountIndex[account] = _accountCount;
        _limit[account] = limit;
        _spent[account] = 0;
        _isActive[account] = true;
        _totalBudget = _totalBudget + limit;
    }

    function spend(address account, uint256 amount) external {
        require(_isActive[account], "Account not active");
        require(_spent[account] + amount <= _limit[account], "Exceeds limit");
        _spent[account] = _spent[account] + amount;
        _totalSpent = _totalSpent + amount;
    }

    function resetPeriod(address account) external {
        require(_isActive[account], "Account not active");
        _spent[account] = 0;
    }

    function updateLimit(address account, uint256 newLimit) external {
        require(_isActive[account], "Account not active");
        uint256 oldLimit = _limit[account];
        _limit[account] = newLimit;
        _totalBudget = _totalBudget - oldLimit + newLimit;
    }

    function deactivateAccount(address account) external {
        require(_isActive[account], "Account not active");
        _isActive[account] = false;
    }

    function activateAccount(address account) external {
        require(!_isActive[account], "Account already active");
        _isActive[account] = true;
    }
}

contract AllowanceTest is Allowance {
    constructor() Allowance() {}

    function initAccount(address account) external {
        _limit[account] = 0;
        _spent[account] = 0;
        _accountIndex[account] = 0;
        _isActive[account] = false;
    }
}
