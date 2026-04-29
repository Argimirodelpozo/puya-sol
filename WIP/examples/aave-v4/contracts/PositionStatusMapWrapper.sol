// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

import {PositionStatusMap} from './PositionStatusMap.sol';
import {ISpoke} from './ISpoke.sol';

contract PositionStatusMapWrapper {
    using PositionStatusMap for ISpoke.PositionStatus;

    ISpoke.PositionStatus internal _p;

    function BORROWING_MASK() external pure returns (uint256) {
        return PositionStatusMap.BORROWING_MASK;
    }

    function COLLATERAL_MASK() external pure returns (uint256) {
        return PositionStatusMap.COLLATERAL_MASK;
    }

    function setBorrowing(uint256 reserveId, bool borrowing) external {
        _p.setBorrowing(reserveId, borrowing);
    }

    function setUsingAsCollateral(uint256 reserveId, bool usingAsCollateral) external {
        _p.setUsingAsCollateral(reserveId, usingAsCollateral);
    }

    function isUsingAsCollateralOrBorrowing(uint256 reserveId) external view returns (bool) {
        return _p.isUsingAsCollateralOrBorrowing(reserveId);
    }

    function isBorrowing(uint256 reserveId) external view returns (bool) {
        return _p.isBorrowing(reserveId);
    }

    function isUsingAsCollateral(uint256 reserveId) external view returns (bool) {
        return _p.isUsingAsCollateral(reserveId);
    }

    function bucketId(uint256 reserveId) external pure returns (uint256) {
        return PositionStatusMap.bucketId(reserveId);
    }

    function isolateBorrowing(uint256 word) external pure returns (uint256) {
        return PositionStatusMap.isolateBorrowing(word);
    }

    function isolateCollateral(uint256 word) external pure returns (uint256) {
        return PositionStatusMap.isolateCollateral(word);
    }
}
