// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract FlashLoan {
    address private _admin;
    uint256 private _feeRate;
    uint256 private _totalPool;
    uint256 private _totalFeesEarned;
    uint256 private _loanCount;
    uint256 private _activeLoanCount;

    mapping(address => uint256) internal _deposited;
    mapping(uint256 => uint256) internal _loanAmount;
    mapping(uint256 => address) internal _loanBorrower;
    mapping(uint256 => bool) internal _loanRepaid;

    constructor(uint256 feeRate_) {
        _admin = msg.sender;
        _feeRate = feeRate_;
        _totalPool = 0;
        _totalFeesEarned = 0;
        _loanCount = 0;
        _activeLoanCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getFeeRate() external view returns (uint256) {
        return _feeRate;
    }

    function getTotalPool() external view returns (uint256) {
        return _totalPool;
    }

    function getTotalFeesEarned() external view returns (uint256) {
        return _totalFeesEarned;
    }

    function getLoanCount() external view returns (uint256) {
        return _loanCount;
    }

    function getActiveLoanCount() external view returns (uint256) {
        return _activeLoanCount;
    }

    function getDeposited(address provider) external view returns (uint256) {
        return _deposited[provider];
    }

    function getLoanAmount(uint256 loanId) external view returns (uint256) {
        return _loanAmount[loanId];
    }

    function getLoanBorrower(uint256 loanId) external view returns (address) {
        return _loanBorrower[loanId];
    }

    function isLoanRepaid(uint256 loanId) external view returns (bool) {
        return _loanRepaid[loanId];
    }

    function calculateFee(uint256 amount) external view returns (uint256) {
        return amount * _feeRate / 10000;
    }

    function deposit(address provider, uint256 amount) external {
        _deposited[provider] = _deposited[provider] + amount;
        _totalPool = _totalPool + amount;
    }

    function withdraw(address provider, uint256 amount) external {
        require(_deposited[provider] >= amount, "Insufficient deposit");
        _deposited[provider] = _deposited[provider] - amount;
        _totalPool = _totalPool - amount;
    }

    function initiateLoan(address borrower, uint256 amount) external returns (uint256) {
        require(amount <= _totalPool, "Insufficient pool liquidity");
        uint256 loanId = _loanCount;
        _loanAmount[loanId] = amount;
        _loanBorrower[loanId] = borrower;
        _loanRepaid[loanId] = false;
        _totalPool = _totalPool - amount;
        _loanCount = _loanCount + 1;
        _activeLoanCount = _activeLoanCount + 1;
        return loanId;
    }

    function repayLoan(uint256 loanId, uint256 repayAmount) external {
        require(_loanRepaid[loanId] == false, "Loan already repaid");
        uint256 owed = _loanAmount[loanId];
        uint256 fee = owed * _feeRate / 10000;
        require(repayAmount >= owed + fee, "Insufficient repayment");
        _loanRepaid[loanId] = true;
        _totalPool = _totalPool + owed + fee;
        _activeLoanCount = _activeLoanCount - 1;
        _totalFeesEarned = _totalFeesEarned + fee;
    }

    function setFeeRate(uint256 newRate) external {
        require(msg.sender == _admin, "Only admin");
        _feeRate = newRate;
    }
}

contract FlashLoanTest is FlashLoan {
    constructor() FlashLoan(30) {}

    function initProvider(address provider) external {
        _deposited[provider] = 0;
    }

    function initLoan(uint256 loanId) external {
        _loanAmount[loanId] = 0;
        _loanBorrower[loanId] = address(0);
        _loanRepaid[loanId] = false;
    }
}
