// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @dev Credit scoring system.
 * Admin registers borrowers with an initial score, adjusts scores,
 * and tracks payment history. Borrowers can be deactivated.
 */
abstract contract CreditScore {
    address private _admin;
    uint256 private _borrowerCount;

    mapping(uint256 => uint256) internal _borrowerScore;
    mapping(uint256 => uint256) internal _paymentCount;
    mapping(uint256 => bool) internal _borrowerActive;

    constructor() {
        _admin = msg.sender;
        _borrowerCount = 0;
    }

    function getAdmin() external view returns (address) {
        return _admin;
    }

    function getBorrowerCount() external view returns (uint256) {
        return _borrowerCount;
    }

    function getBorrowerScore(uint256 borrowerId) external view returns (uint256) {
        return _borrowerScore[borrowerId];
    }

    function getPaymentCount(uint256 borrowerId) external view returns (uint256) {
        return _paymentCount[borrowerId];
    }

    function isBorrowerActive(uint256 borrowerId) external view returns (bool) {
        return _borrowerActive[borrowerId];
    }

    function getAverageScore() external view returns (uint256) {
        require(_borrowerCount > 0, "No borrowers");
        uint256 totalScore = 0;
        for (uint256 i = 0; i < _borrowerCount; i++) {
            totalScore = totalScore + _borrowerScore[i];
        }
        return totalScore / _borrowerCount;
    }

    function registerBorrower(uint256 initialScore) external returns (uint256) {
        require(msg.sender == _admin, "Not admin");
        uint256 id = _borrowerCount;
        _borrowerScore[id] = initialScore;
        _paymentCount[id] = 0;
        _borrowerActive[id] = true;
        _borrowerCount = id + 1;
        return id;
    }

    function adjustScore(uint256 borrowerId, uint256 newScore) external {
        require(msg.sender == _admin, "Not admin");
        require(borrowerId < _borrowerCount, "Borrower does not exist");
        require(_borrowerActive[borrowerId], "Borrower is inactive");
        _borrowerScore[borrowerId] = newScore;
    }

    function recordPayment(uint256 borrowerId) external {
        require(borrowerId < _borrowerCount, "Borrower does not exist");
        require(_borrowerActive[borrowerId], "Borrower is inactive");
        _paymentCount[borrowerId] = _paymentCount[borrowerId] + 1;
    }

    function deactivateBorrower(uint256 borrowerId) external {
        require(msg.sender == _admin, "Not admin");
        require(borrowerId < _borrowerCount, "Borrower does not exist");
        require(_borrowerActive[borrowerId], "Borrower already inactive");
        _borrowerActive[borrowerId] = false;
    }
}

contract CreditScoreTest is CreditScore {
    constructor() CreditScore() {}

    function initBorrower(uint256 borrowerId) external {
        _borrowerScore[borrowerId] = 0;
        _paymentCount[borrowerId] = 0;
        _borrowerActive[borrowerId] = false;
    }
}
