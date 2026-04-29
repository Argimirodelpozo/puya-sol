// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

import {SharesMath} from './SharesMath.sol';

contract SharesMathWrapper {
    function VIRTUAL_ASSETS() external pure returns (uint256) {
        return SharesMath.VIRTUAL_ASSETS;
    }

    function VIRTUAL_SHARES() external pure returns (uint256) {
        return SharesMath.VIRTUAL_SHARES;
    }

    function toSharesDown(
        uint256 assets,
        uint256 totalAssets,
        uint256 totalShares
    ) external pure returns (uint256) {
        return SharesMath.toSharesDown(assets, totalAssets, totalShares);
    }

    function toAssetsDown(
        uint256 shares,
        uint256 totalAssets,
        uint256 totalShares
    ) external pure returns (uint256) {
        return SharesMath.toAssetsDown(shares, totalAssets, totalShares);
    }

    function toSharesUp(
        uint256 assets,
        uint256 totalAssets,
        uint256 totalShares
    ) external pure returns (uint256) {
        return SharesMath.toSharesUp(assets, totalAssets, totalShares);
    }

    function toAssetsUp(
        uint256 shares,
        uint256 totalAssets,
        uint256 totalShares
    ) external pure returns (uint256) {
        return SharesMath.toAssetsUp(shares, totalAssets, totalShares);
    }
}
