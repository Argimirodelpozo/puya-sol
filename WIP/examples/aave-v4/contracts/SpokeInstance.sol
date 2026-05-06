// SPDX-License-Identifier: UNLICENSED
// Copyright (c) 2025 Aave Labs
pragma solidity 0.8.28;

import {Spoke} from './Spoke.sol';

/// @title SpokeInstance
/// @author Aave Labs
/// @notice Implementation contract for the Spoke.
contract SpokeInstance is Spoke {
  uint64 public constant SPOKE_REVISION = 1;

  /// @dev Constructor.
  /// @dev During upgrade, must ensure that the new oracle is supporting existing assets on the spoke and the replaced oracle.
  /// @param oracle_ The address of the oracle.
  /// @param maxUserReservesLimit_ The maximum number of collateral and borrow reserves a user can have.
  constructor(address oracle_, uint16 maxUserReservesLimit_) Spoke(oracle_, maxUserReservesLimit_) {
    // EVM source called `_disableInitializers()` (OZ Initializable
    // safety lock for upgradeable contracts). Puya-sol emits a
    // sequence ending in an `assert(false)` (the inner `revert
    // InvalidInitialization()` branch fires unconditionally — likely
    // a translation gap around the OZ packed `_initialized`/
    // `_initializing` storage layout). We don't need the lock for
    // the AAVE deploy harness — `__ctor_pending` already gates
    // re-running __postInit — so we drop the call.
  }

  /// @notice Initializer.
  /// @dev The authority contract must implement the `AccessManaged` interface for access control.
  /// @param authority The address of the authority contract which manages permissions.
  // NOTE (AVM): the EVM source had `external override reinitializer(SPOKE_REVISION)`.
  // The OZ Initializable `reinitializer` modifier does packed-storage
  // bit math on `_initialized` (uint64) + `_initializing` (bool at
  // bit 64) that puya-sol mistranslates (getbit at index 64 on a
  // uint64 traps). Drop the modifier — re-init protection isn't
  // needed for AAVE's deploy harness; the user calls initialize once
  // and never again.
  function initialize(address authority) external override {
    emit SetSpokeImmutables(ORACLE, MAX_USER_RESERVES_LIMIT);

    require(authority != address(0), InvalidAddress());
    __AccessManaged_init(authority);
    if (_liquidationConfig.targetHealthFactor == 0) {
      _liquidationConfig.targetHealthFactor = HEALTH_FACTOR_LIQUIDATION_THRESHOLD;
      emit UpdateLiquidationConfig(_liquidationConfig);
    }
  }
}
