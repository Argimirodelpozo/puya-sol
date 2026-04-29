// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simplified ERC20 wrapper - wraps one token into another 1:1.
 * Simulates deposit/withdraw pattern without actual token transfers.
 */
contract ERC20WrapperTest {
    string private _name;
    string private _symbol;
    uint256 private _totalSupply;
    uint256 private _totalDeposited;

    mapping(address => uint256) private _balances;
    mapping(address => uint256) private _deposited;

    constructor() {
        _name = "Wrapped Token";
        _symbol = "wTOK";
    }

    function name() external view returns (string memory) {
        return _name;
    }

    function symbol() external view returns (string memory) {
        return _symbol;
    }

    function totalSupply() external view returns (uint256) {
        return _totalSupply;
    }

    function totalDeposited() external view returns (uint256) {
        return _totalDeposited;
    }

    function balanceOf(address account) external view returns (uint256) {
        return _balances[account];
    }

    function depositedOf(address account) external view returns (uint256) {
        return _deposited[account];
    }

    function depositFor(address account, uint256 amount) external returns (bool) {
        require(amount > 0, "Amount must be > 0");

        _deposited[account] += amount;
        _totalDeposited += amount;

        _balances[account] += amount;
        _totalSupply += amount;

        return true;
    }

    function withdrawTo(address account, uint256 amount) external returns (bool) {
        require(_balances[account] >= amount, "Insufficient balance");

        _balances[account] -= amount;
        _totalSupply -= amount;

        _deposited[account] -= amount;
        _totalDeposited -= amount;

        return true;
    }

    function transfer(address from, address to, uint256 amount) external returns (bool) {
        require(_balances[from] >= amount, "Insufficient balance");

        _balances[from] -= amount;
        _balances[to] += amount;

        return true;
    }
}
