// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

import {SpokeUtils} from './SpokeUtils.sol';

contract SpokeUtilsWrapper {
    function toValue(uint256 amount, uint256 decimals, uint256 price)
        external
        pure
        returns (uint256)
    {
        return SpokeUtils.toValue(amount, decimals, price);
    }
}
