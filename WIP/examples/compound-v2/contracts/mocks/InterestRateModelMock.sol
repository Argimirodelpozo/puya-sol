// SPDX-License-Identifier: BSD-3-Clause
pragma solidity ^0.8.10;

/// @title Mock InterestRateModel for CToken testing.
/// Returns configurable borrow/supply rates.
contract InterestRateModelMock {
    function isInterestRateModel() external pure returns (bool) {
        return true;
    }
    uint public borrowRatePerBlock;
    uint public supplyRatePerBlock;

    function setBorrowRate(uint _rate) external {
        borrowRatePerBlock = _rate;
    }

    function setSupplyRate(uint _rate) external {
        supplyRatePerBlock = _rate;
    }

    function getBorrowRate(uint, uint, uint) external view returns (uint) {
        return borrowRatePerBlock;
    }

    function getSupplyRate(uint, uint, uint, uint) external view returns (uint) {
        return supplyRatePerBlock;
    }
}
