// SPDX-License-Identifier: MIT
// Token vault with deposit/withdrawal tracking and fee collection
// Tests: compound assignments on multiple mappings, percentage math, tuple returns

pragma solidity ^0.8.20;

error VaultZeroAmount();
error VaultInsufficientBalance(uint256 available, uint256 requested);
error VaultZeroAddress();
error VaultNotOwner(address caller);
error VaultFeeExceedsMax(uint256 fee, uint256 max);

contract TokenVaultV2 {
    event Deposited(address indexed account, uint256 amount, uint256 shares);
    event Withdrawn(address indexed account, uint256 amount, uint256 fee);
    event FeeCollected(address indexed collector, uint256 amount);
    event FeeUpdated(uint256 oldFee, uint256 newFee);

    address private _owner;

    // Deposit tracking
    mapping(address => uint256) private _balances;
    mapping(address => uint256) private _depositCount;
    mapping(address => uint256) private _totalDeposited;
    mapping(address => uint256) private _totalWithdrawn;

    // Global stats
    uint256 private _totalBalance;
    uint256 private _totalFees;
    uint256 private _withdrawalFeeBps; // basis points (100 = 1%)
    uint256 private _depositorsCount;

    uint256 private constant MAX_FEE_BPS = 1000; // 10% max
    uint256 private constant BPS_DENOMINATOR = 10000;

    constructor() {
        _owner = msg.sender;
        _withdrawalFeeBps = 200; // 2% default fee
    }

    modifier onlyOwner() {
        if (msg.sender != _owner) {
            revert VaultNotOwner(msg.sender);
        }
        _;
    }

    function deposit(uint256 amount) public {
        if (amount == 0) {
            revert VaultZeroAmount();
        }

        if (_balances[msg.sender] == 0 && _depositCount[msg.sender] == 0) {
            _depositorsCount += 1;
        }

        _balances[msg.sender] += amount;
        _totalDeposited[msg.sender] += amount;
        _depositCount[msg.sender] += 1;
        _totalBalance += amount;

        emit Deposited(msg.sender, amount, _balances[msg.sender]);
    }

    function withdraw(uint256 amount) public {
        if (amount == 0) {
            revert VaultZeroAmount();
        }

        uint256 balance = _balances[msg.sender];
        if (balance < amount) {
            revert VaultInsufficientBalance(balance, amount);
        }

        uint256 fee = (amount * _withdrawalFeeBps) / BPS_DENOMINATOR;
        uint256 netAmount = amount - fee;

        _balances[msg.sender] = balance - amount;
        _totalWithdrawn[msg.sender] += netAmount;
        _totalBalance -= amount;
        _totalFees += fee;

        emit Withdrawn(msg.sender, netAmount, fee);
    }

    function collectFees() public onlyOwner {
        uint256 fees = _totalFees;
        _totalFees = 0;
        emit FeeCollected(msg.sender, fees);
    }

    function setFee(uint256 newFeeBps) public onlyOwner {
        if (newFeeBps > MAX_FEE_BPS) {
            revert VaultFeeExceedsMax(newFeeBps, MAX_FEE_BPS);
        }
        uint256 oldFee = _withdrawalFeeBps;
        _withdrawalFeeBps = newFeeBps;
        emit FeeUpdated(oldFee, newFeeBps);
    }

    // --- View functions ---

    function balanceOf(address account) public view returns (uint256) {
        return _balances[account];
    }

    function depositCount(address account) public view returns (uint256) {
        return _depositCount[account];
    }

    function totalDeposited(address account) public view returns (uint256) {
        return _totalDeposited[account];
    }

    function totalWithdrawn(address account) public view returns (uint256) {
        return _totalWithdrawn[account];
    }

    function accountInfo(address account) public view returns (
        uint256 balance,
        uint256 deposits,
        uint256 deposited,
        uint256 withdrawn
    ) {
        return (
            _balances[account],
            _depositCount[account],
            _totalDeposited[account],
            _totalWithdrawn[account]
        );
    }

    function totalBalance() public view returns (uint256) {
        return _totalBalance;
    }

    function totalFees() public view returns (uint256) {
        return _totalFees;
    }

    function withdrawalFeeBps() public view returns (uint256) {
        return _withdrawalFeeBps;
    }

    function depositorsCount() public view returns (uint256) {
        return _depositorsCount;
    }

    function owner() public view returns (address) {
        return _owner;
    }

    function calculateFee(uint256 amount) public view returns (uint256) {
        return (amount * _withdrawalFeeBps) / BPS_DENOMINATOR;
    }
}
