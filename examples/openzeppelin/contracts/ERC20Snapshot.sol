// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev ERC20 with snapshot functionality.
 * Captures balances at specific snapshot IDs for governance voting.
 */
contract ERC20SnapshotTest {
    string private _name;
    string private _symbol;

    mapping(address => uint256) private _balances;
    uint256 private _totalSupply;

    // Snapshot tracking
    uint256 private _currentSnapshotId;
    // snapshotId => address => balance at that snapshot
    mapping(uint256 => mapping(address => uint256)) private _snapshotBalances;
    // snapshotId => totalSupply at that snapshot
    mapping(uint256 => uint256) private _snapshotTotalSupply;
    // snapshotId => address => whether balance was snapshotted
    mapping(uint256 => mapping(address => bool)) private _snapshotted;

    constructor() {
        _name = "SnapshotToken";
        _symbol = "SNAP";
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

    function currentSnapshotId() external view returns (uint256) {
        return _currentSnapshotId;
    }

    function mint(address to, uint256 amount) external {
        _balances[to] += amount;
        _totalSupply += amount;
    }

    function transfer(address to, uint256 amount) external returns (bool) {
        require(_balances[msg.sender] >= amount, "ERC20: insufficient balance");
        _updateSnapshot(msg.sender);
        _updateSnapshot(to);
        _balances[msg.sender] -= amount;
        _balances[to] += amount;
        return true;
    }

    // Take a snapshot — captures current state
    function snapshot() external returns (uint256) {
        _currentSnapshotId += 1;
        _snapshotTotalSupply[_currentSnapshotId] = _totalSupply;
        return _currentSnapshotId;
    }

    // Query balance at a specific snapshot
    function balanceOfAt(address account, uint256 snapshotId) external view returns (uint256) {
        require(snapshotId > 0 && snapshotId <= _currentSnapshotId, "ERC20Snapshot: nonexistent id");
        if (_snapshotted[snapshotId][account]) {
            return _snapshotBalances[snapshotId][account];
        }
        // If not snapshotted, current balance is the snapshot balance
        return _balances[account];
    }

    // Query total supply at a specific snapshot
    function totalSupplyAt(uint256 snapshotId) external view returns (uint256) {
        require(snapshotId > 0 && snapshotId <= _currentSnapshotId, "ERC20Snapshot: nonexistent id");
        return _snapshotTotalSupply[snapshotId];
    }

    // Internal: snapshot an account's current balance before modification
    function _updateSnapshot(address account) private {
        if (_currentSnapshotId > 0 && !_snapshotted[_currentSnapshotId][account]) {
            _snapshotBalances[_currentSnapshotId][account] = _balances[account];
            _snapshotted[_currentSnapshotId][account] = true;
        }
    }
}
