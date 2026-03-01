// SPDX-License-Identifier: MIT
// Payment splitting pattern — share-based distribution with owner-managed payees
// Demonstrates: owner access control, multiple mappings, share-based math, require
pragma solidity ^0.8.20;

abstract contract PaymentSplitter {
    address private owner;
    uint256 private totalShares;
    uint256 private totalDeposited;
    uint256 private totalReleased;
    uint256 private payeeCount;

    mapping(address => uint256) internal _shares;
    mapping(address => uint256) internal _released;
    mapping(address => uint256) internal _payeeIndex; // 1-based, 0 = not a payee

    error NotOwner();
    error ZeroShares();
    error AlreadyPayee();
    error NoPaymentDue();

    event PayeeAdded(address indexed account, uint256 shares);
    event PaymentDeposited(uint256 amount);
    event PaymentReleased(address indexed to, uint256 amount);

    constructor() {
        owner = msg.sender;
    }

    function addPayee(address payee, uint256 shares_) external {
        if (msg.sender != owner) revert NotOwner();
        if (shares_ == 0) revert ZeroShares();
        if (_payeeIndex[payee] != 0) revert AlreadyPayee();
        _shares[payee] = shares_;
        payeeCount += 1;
        _payeeIndex[payee] = payeeCount;
        totalShares += shares_;
        emit PayeeAdded(payee, shares_);
    }

    function deposit(uint256 amount) external {
        totalDeposited += amount;
        emit PaymentDeposited(amount);
    }

    function release(address payee) external returns (uint256) {
        uint256 shares_ = _shares[payee];
        uint256 totalOwed = totalDeposited * shares_ / totalShares;
        uint256 alreadyReleased = _released[payee];
        uint256 owed = totalOwed - alreadyReleased;
        if (owed == 0) revert NoPaymentDue();
        _released[payee] = alreadyReleased + owed;
        totalReleased += owed;
        emit PaymentReleased(payee, owed);
        return owed;
    }

    function releasable(address payee) external view returns (uint256) {
        uint256 shares_ = _shares[payee];
        if (shares_ == 0) return 0;
        uint256 totalOwed = totalDeposited * shares_ / totalShares;
        uint256 alreadyReleased = _released[payee];
        uint256 owed = totalOwed - alreadyReleased;
        return owed;
    }

    function getShares(address payee) external view returns (uint256) {
        return _shares[payee];
    }

    function getReleased(address payee) external view returns (uint256) {
        return _released[payee];
    }

    function getTotalShares() external view returns (uint256) {
        return totalShares;
    }

    function getTotalDeposited() external view returns (uint256) {
        return totalDeposited;
    }

    function getTotalReleased() external view returns (uint256) {
        return totalReleased;
    }

    function getPayeeCount() external view returns (uint256) {
        return payeeCount;
    }

    function isPayee(address addr) external view returns (bool) {
        return _payeeIndex[addr] != 0;
    }

    function getOwner() external view returns (address) {
        return owner;
    }
}

// Test wrapper — default constructor (no args)
contract PaymentSplitterTest is PaymentSplitter {
    constructor() PaymentSplitter() {}

    function initPayee(address payee) external {
        _shares[payee] = 0;
        _released[payee] = 0;
        _payeeIndex[payee] = 0;
    }
}
