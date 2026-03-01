// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

abstract contract Dividend {
    address private admin;
    uint256 private totalShares;
    uint256 private totalDividends;
    uint256 private totalClaimed;
    uint256 private shareholderCount;
    uint256 private dividendRound;

    mapping(address => uint256) internal _shares;
    mapping(address => uint256) internal _claimed;
    mapping(address => uint256) internal _shareholderIndex;

    constructor() {
        admin = msg.sender;
    }

    function addShareHolder(address holder, uint256 shares) public {
        require(msg.sender == admin, "not admin");
        if (_shareholderIndex[holder] == 0) {
            shareholderCount += 1;
            _shareholderIndex[holder] = shareholderCount;
        }
        totalShares += shares;
        _shares[holder] += shares;
    }

    function distributeDividend(uint256 amount) public {
        require(msg.sender == admin, "not admin");
        totalDividends += amount;
        dividendRound += 1;
    }

    function claimable(address holder) public view returns (uint256) {
        if (totalShares == 0) {
            return 0;
        }
        uint256 owed = totalDividends * _shares[holder] / totalShares;
        if (owed <= _claimed[holder]) {
            return 0;
        }
        return owed - _claimed[holder];
    }

    function claim(address holder) public returns (uint256) {
        uint256 amount = claimable(holder);
        require(amount > 0, "nothing to claim");
        _claimed[holder] += amount;
        totalClaimed += amount;
        return amount;
    }

    function getShares(address holder) public view returns (uint256) {
        return _shares[holder];
    }

    function getClaimed(address holder) public view returns (uint256) {
        return _claimed[holder];
    }

    function getTotalShares() public view returns (uint256) {
        return totalShares;
    }

    function getTotalDividends() public view returns (uint256) {
        return totalDividends;
    }

    function getTotalClaimed() public view returns (uint256) {
        return totalClaimed;
    }

    function getShareholderCount() public view returns (uint256) {
        return shareholderCount;
    }

    function getDividendRound() public view returns (uint256) {
        return dividendRound;
    }

    function getAdmin() public view returns (address) {
        return admin;
    }
}

contract DividendTest is Dividend {
    constructor() Dividend() {}

    function initHolder(address holder) public {
        _shares[holder] = 0;
        _claimed[holder] = 0;
        _shareholderIndex[holder] = 0;
    }
}
