// SPDX-License-Identifier: MIT
// Inspired by OpenZeppelin ERC1155Supply.sol
// Demonstrates: nested mappings, supply tracking, events, custom errors
pragma solidity ^0.8.20;

contract ERC1155SupplyTest {
    mapping(uint256 => uint256) private _totalSupply;
    uint256 private _totalSupplyAll;

    // Simplified single-key balance tracking (avoids nested mapping complexity)
    // Key = keccak256(abi.encodePacked(account, id))
    mapping(bytes32 => uint256) private _balances;

    event TransferSingle(address indexed operator, address indexed from, address indexed to, uint256 id, uint256 value);

    error InsufficientBalance();

    function _balanceKey(address account, uint256 id) internal pure returns (bytes32) {
        return keccak256(abi.encodePacked(account, id));
    }

    function initBalance(address account, uint256 id) external {
        bytes32 key = _balanceKey(account, id);
        _balances[key] = 0;
    }

    function initSupply(uint256 id) external {
        _totalSupply[id] = 0;
    }

    function mint(address to, uint256 id, uint256 amount) external {
        bytes32 key = _balanceKey(to, id);
        _balances[key] += amount;
        _totalSupply[id] += amount;
        _totalSupplyAll += amount;
        emit TransferSingle(msg.sender, address(0), to, id, amount);
    }

    function burn(address from, uint256 id, uint256 amount) external {
        bytes32 key = _balanceKey(from, id);
        uint256 bal = _balances[key];
        if (bal < amount) revert InsufficientBalance();
        _balances[key] = bal - amount;
        _totalSupply[id] -= amount;
        _totalSupplyAll -= amount;
        emit TransferSingle(msg.sender, from, address(0), id, amount);
    }

    function balanceOf(address account, uint256 id) external view returns (uint256) {
        return _balances[_balanceKey(account, id)];
    }

    function totalSupply(uint256 id) external view returns (uint256) {
        return _totalSupply[id];
    }

    function totalSupplyAll() external view returns (uint256) {
        return _totalSupplyAll;
    }

    function exists(uint256 id) external view returns (bool) {
        return _totalSupply[id] > 0;
    }
}
