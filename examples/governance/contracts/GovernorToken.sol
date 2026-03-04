// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * Governor Token — ERC20-style token with voting power delegation.
 * Inspired by Compound's COMP token.
 * Members hold tokens that grant voting power. Delegation is supported.
 */
contract GovernorToken {
    string public name;
    string public symbol;
    uint256 public totalSupply;

    mapping(address => uint256) private _balances;
    mapping(address => mapping(address => uint256)) private _allowances;
    mapping(address => address) private _delegates;
    mapping(address => uint256) private _votingPower;

    event Transfer(address indexed from, address indexed to, uint256 value);
    event Approval(address indexed owner, address indexed spender, uint256 value);
    event DelegateChanged(address indexed delegator, address indexed fromDelegate, address indexed toDelegate);

    address public owner;

    constructor(string memory _name, string memory _symbol) {
        name = _name;
        symbol = _symbol;
        owner = msg.sender;
    }

    function balanceOf(address account) external view returns (uint256) {
        return _balances[account];
    }

    function transfer(address to, uint256 amount) external returns (bool) {
        _transfer(msg.sender, to, amount);
        return true;
    }

    function approve(address spender, uint256 amount) external returns (bool) {
        _allowances[msg.sender][spender] = amount;
        emit Approval(msg.sender, spender, amount);
        return true;
    }

    function allowance(address _owner, address spender) external view returns (uint256) {
        return _allowances[_owner][spender];
    }

    function transferFrom(address from, address to, uint256 amount) external returns (bool) {
        uint256 currentAllowance = _allowances[from][msg.sender];
        require(currentAllowance >= amount, "insufficient allowance");
        _allowances[from][msg.sender] = currentAllowance - amount;
        _transfer(from, to, amount);
        return true;
    }

    /// Delegate voting power to another address. Self-delegation to use own power.
    function delegate(address delegatee) external {
        address currentDelegate = _delegates[msg.sender];
        _delegates[msg.sender] = delegatee;

        // Move voting power from old delegate to new
        uint256 balance = _balances[msg.sender];
        if (currentDelegate != address(0) && balance > 0) {
            _votingPower[currentDelegate] -= balance;
        }
        if (delegatee != address(0) && balance > 0) {
            _votingPower[delegatee] += balance;
        }

        emit DelegateChanged(msg.sender, currentDelegate, delegatee);
    }

    function getVotingPower(address account) external view returns (uint256) {
        return _votingPower[account];
    }

    function delegates(address account) external view returns (address) {
        return _delegates[account];
    }

    function mint(address to, uint256 amount) external {
        require(msg.sender == owner, "only owner");
        _mint(to, amount);
    }

    function _mint(address to, uint256 amount) internal {
        totalSupply += amount;
        _balances[to] += amount;
        // If delegated, add voting power to delegate
        address delegatee = _delegates[to];
        if (delegatee != address(0)) {
            _votingPower[delegatee] += amount;
        }
        emit Transfer(address(0), to, amount);
    }

    function _transfer(address from, address to, uint256 amount) internal {
        require(_balances[from] >= amount, "insufficient balance");
        _balances[from] -= amount;
        _balances[to] += amount;

        // Move voting power if delegated
        address fromDelegate = _delegates[from];
        address toDelegate = _delegates[to];
        if (fromDelegate != address(0) && amount > 0) {
            _votingPower[fromDelegate] -= amount;
        }
        if (toDelegate != address(0) && amount > 0) {
            _votingPower[toDelegate] += amount;
        }

        emit Transfer(from, to, amount);
    }
}
