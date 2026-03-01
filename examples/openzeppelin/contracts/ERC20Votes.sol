// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Simplified ERC20 voting power contract.
 * Users can delegate their voting power to themselves or others.
 * Tracks voting power per address.
 */
contract ERC20VotesTest {
    string private _name;
    string private _symbol;
    uint256 private _totalSupply;

    mapping(address => uint256) private _balances;
    mapping(address => address) private _delegates;
    mapping(address => uint256) private _votingPower;

    constructor() {
        _name = "VoteToken";
        _symbol = "VOTE";
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

    function balanceOf(address account) external view returns (uint256) {
        return _balances[account];
    }

    function delegates(address account) external view returns (address) {
        return _delegates[account];
    }

    function getVotes(address account) external view returns (uint256) {
        return _votingPower[account];
    }

    function mint(address to, uint256 amount) external {
        _balances[to] += amount;
        _totalSupply += amount;

        // If delegated, add voting power to delegate
        address delegate = _delegates[to];
        if (delegate != address(0)) {
            _votingPower[delegate] += amount;
        }
    }

    function delegate(address delegator, address delegatee) external {
        address oldDelegate = _delegates[delegator];
        uint256 balance = _balances[delegator];

        // Remove voting power from old delegate
        if (oldDelegate != address(0) && balance > 0) {
            _votingPower[oldDelegate] -= balance;
        }

        _delegates[delegator] = delegatee;

        // Add voting power to new delegate
        if (delegatee != address(0) && balance > 0) {
            _votingPower[delegatee] += balance;
        }
    }

    function transfer(address from, address to, uint256 amount) external {
        require(_balances[from] >= amount, "Insufficient balance");

        _balances[from] -= amount;
        _balances[to] += amount;

        // Move voting power if delegates differ
        address fromDelegate = _delegates[from];
        address toDelegate = _delegates[to];

        if (fromDelegate != address(0)) {
            _votingPower[fromDelegate] -= amount;
        }
        if (toDelegate != address(0)) {
            _votingPower[toDelegate] += amount;
        }
    }
}
