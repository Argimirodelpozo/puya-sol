// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Lending {
    address private admin;
    uint256 private interestRate;
    uint256 private totalDeposited;
    uint256 private totalBorrowed;
    uint256 private borrowerCount;

    mapping(address => uint256) internal _deposited;
    mapping(address => uint256) internal _borrowed;
    mapping(address => uint256) internal _borrowerIndex;

    constructor(uint256 interestRate_) {
        admin = msg.sender;
        interestRate = interestRate_;
    }

    function lend(address lender, uint256 amount) public {
        _deposited[lender] = _deposited[lender] + amount;
        totalDeposited = totalDeposited + amount;
    }

    function borrow(address borrower, uint256 amount) public {
        require(totalDeposited - totalBorrowed >= amount, "insufficient liquidity");
        if (_borrowed[borrower] == 0 && _borrowerIndex[borrower] == 0) {
            borrowerCount = borrowerCount + 1;
            _borrowerIndex[borrower] = borrowerCount;
        }
        _borrowed[borrower] = _borrowed[borrower] + amount;
        totalBorrowed = totalBorrowed + amount;
    }

    function repay(address borrower, uint256 amount) public {
        require(_borrowed[borrower] >= amount, "repay exceeds borrowed");
        _borrowed[borrower] = _borrowed[borrower] - amount;
        totalBorrowed = totalBorrowed - amount;
    }

    function calculateInterest(address borrower) public view returns (uint256) {
        return _borrowed[borrower] * interestRate / 10000;
    }

    function getDeposited(address account) public view returns (uint256) {
        return _deposited[account];
    }

    function getBorrowed(address account) public view returns (uint256) {
        return _borrowed[account];
    }

    function getAvailableLiquidity() public view returns (uint256) {
        return totalDeposited - totalBorrowed;
    }

    function getTotalDeposited() public view returns (uint256) {
        return totalDeposited;
    }

    function getTotalBorrowed() public view returns (uint256) {
        return totalBorrowed;
    }

    function getBorrowerCount() public view returns (uint256) {
        return borrowerCount;
    }

    function getInterestRate() public view returns (uint256) {
        return interestRate;
    }

    function setInterestRate(uint256 newRate) public {
        require(msg.sender == admin, "only admin");
        interestRate = newRate;
    }

    function getAdmin() public view returns (address) {
        return admin;
    }
}

contract LendingTest is Lending {
    constructor() Lending(500) {}

    function initAccount(address account) public {
        _deposited[account] = 0;
        _borrowed[account] = 0;
        _borrowerIndex[account] = 0;
    }
}
