// SPDX-License-Identifier: MIT
pragma solidity >=0.7.0;

/**
 * M36: Constructor parameters flowing to box-writing code (Gap 12 verification).
 * Tests that constructor params are properly passed through __postInit.
 */

contract TokenWithSupply {
    mapping(address => uint256) private _balances;
    uint256 private _totalSupply;
    address private _owner;

    constructor(address initialOwner, uint256 initialSupply) {
        _owner = initialOwner;
        _totalSupply = initialSupply;
        _balances[initialOwner] = initialSupply;  // writes to box → triggers __postInit
    }

    function balanceOf(address account) external view returns (uint256) {
        return _balances[account];
    }

    function totalSupply() external view returns (uint256) {
        return _totalSupply;
    }

    function owner() external view returns (address) {
        return _owner;
    }

    function transfer(address to, uint256 amount) external {
        address sender = _owner;  // simplified: only owner can transfer
        require(_balances[sender] >= amount, "insufficient");
        _balances[sender] -= amount;
        _balances[to] += amount;
    }
}
