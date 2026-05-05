// SPDX-License-Identifier: UNLICENSED
// Test fixture for AAVE V4 TreasurySpoke tests.
pragma solidity 0.8.28;

/// @title MockHub
/// @notice Minimal Hub mock used as the HUB constructor arg for
///         TreasurySpoke in tests. TreasurySpoke's getSuppliedAmount /
///         getSuppliedShares / getReserveSuppliedAssets /
///         getReserveSuppliedShares all forward to
///         HUB.getSpokeAddedAssets / HUB.getSpokeAddedShares — this
///         mock returns 0 from both so the spoke's queries succeed in
///         test setup. Real Hub is too large and stateful to deploy
///         in a unit test.
contract MockHub {
  function getSpokeAddedAssets(
    uint256 /*reserveId*/,
    address /*spoke*/
  ) external pure returns (uint256) {
    return 0;
  }

  function getSpokeAddedShares(
    uint256 /*reserveId*/,
    address /*spoke*/
  ) external pure returns (uint256) {
    return 0;
  }
}
