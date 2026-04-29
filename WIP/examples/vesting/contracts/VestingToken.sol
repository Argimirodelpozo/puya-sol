// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/**
 * @title VestingToken
 * @notice Simple ERC20-like token for vesting tests.
 */
contract VestingToken {
    string public name;
    string public symbol;
    uint256 public totalSupply;
    address public owner;

    mapping(address => uint256) private _balances;
    mapping(address => mapping(address => uint256)) private _allowances;

    constructor(string memory _name, string memory _symbol) {
        name = _name;
        symbol = _symbol;
        owner = msg.sender;
    }

    function mint(address to, uint256 amount) external {
        require(msg.sender == owner, "not owner");
        _balances[to] = _balances[to] + amount;
        totalSupply = totalSupply + amount;
    }

    function balanceOf(address account) external view returns (uint256) {
        return _balances[account];
    }

    function transfer(address to, uint256 amount) external returns (bool) {
        require(_balances[msg.sender] >= amount, "insufficient balance");
        _balances[msg.sender] = _balances[msg.sender] - amount;
        _balances[to] = _balances[to] + amount;
        return true;
    }

    function approve(address spender, uint256 amount) external returns (bool) {
        _allowances[msg.sender][spender] = amount;
        return true;
    }

    function allowance(address tokenOwner, address spender) external view returns (uint256) {
        return _allowances[tokenOwner][spender];
    }

    function transferFrom(address from, address to, uint256 amount) external returns (bool) {
        require(_allowances[from][msg.sender] >= amount, "not approved");
        require(_balances[from] >= amount, "insufficient balance");
        _allowances[from][msg.sender] = _allowances[from][msg.sender] - amount;
        _balances[from] = _balances[from] - amount;
        _balances[to] = _balances[to] + amount;
        return true;
    }
}
