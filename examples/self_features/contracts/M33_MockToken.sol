// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M33 callee: Simple mock ERC20 token for cross-contract call testing.
 */
contract MockToken {
    mapping(address => uint256) private _balances;

    function mint(address to, uint256 amount) external {
        _balances[to] += amount;
    }

    function transfer(address to, uint256 amount) external returns (bool) {
        address sender = msg.sender;
        require(_balances[sender] >= amount, "insufficient balance");
        _balances[sender] -= amount;
        _balances[to] += amount;
        return true;
    }

    function balanceOf(address account) external view returns (uint256) {
        return _balances[account];
    }
}
