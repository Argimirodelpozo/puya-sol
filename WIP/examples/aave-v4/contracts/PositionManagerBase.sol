// SPDX-License-Identifier: UNLICENSED
// Copyright (c) 2025 Aave Labs
pragma solidity 0.8.28;

import {IERC20} from './IERC20.sol';
import {IERC20Permit} from './IERC20Permit.sol';
import {Ownable2Step, Ownable} from './Ownable2Step.sol';
import {IntentConsumer} from './IntentConsumer.sol';
import {IMulticall, Multicall} from './Multicall.sol';
import {Rescuable} from './Rescuable.sol';
import {ISpoke} from './ISpoke.sol';
import {IPositionManagerBase} from './IPositionManagerBase.sol';

/// @title PositionManagerBase
/// @author Aave Labs
/// @notice Base implementation for position manager common functionalities.
/// @dev This base contract is not mandatory for position managers, it only implements optional convenience methods for position managers.
/// @dev Contract must be an active and approved user position manager in order to execute spoke actions on a user's behalf.
/// @dev The `_multicallEnabled()` function must be implemented to specify whether multicall is enabled.
abstract contract PositionManagerBase is
  IPositionManagerBase,
  Ownable2Step,
  IntentConsumer,
  Rescuable,
  Multicall
{
  /// @dev Map of registered spokes.
  mapping(address spoke => bool registered) internal _registeredSpokes;

  /// @notice Modifier that checks if the specified spoke is registered.
  modifier onlyRegisteredSpoke(address spoke) {
    require(_isSpokeRegistered(spoke), SpokeNotRegistered());
    _;
  }

  /// @dev Constructor.
  /// @param initialOwner_ The address of the initial owner.
  constructor(address initialOwner_) Ownable(initialOwner_) {}

  /// @inheritdoc IPositionManagerBase
  function registerSpoke(address spoke, bool registered) external onlyOwner {
    require(spoke != address(0), InvalidAddress());
    _registeredSpokes[spoke] = registered;
    emit SpokeRegistered(spoke, registered);
  }

  /// @inheritdoc IPositionManagerBase
  function setSelfAsUserPositionManagerWithSig(
    address spoke,
    address onBehalfOf,
    bool approve,
    uint256 nonce,
    uint256 deadline,
    bytes calldata signature
  ) external onlyRegisteredSpoke(spoke) {
    ISpoke.PositionManagerUpdate[] memory updates = new ISpoke.PositionManagerUpdate[](1);
    updates[0] = ISpoke.PositionManagerUpdate({positionManager: address(this), approve: approve});
    // PUYA-SOL PATCH (uros-splitter readiness, AVM port): try/catch is
    // unsupported on AVM (see WIP/examples/aave-v4/patches.md). Original:
    //   try ISpoke(spoke).setUserPositionManagersWithSig(...) {} catch {}
    // Replaced with a direct call. Behavior change: if the inner call
    // reverts, this txn now reverts too (instead of silently swallowing
    // the failure). Acceptable for this entrypoint because the caller
    // can retry with a different signature; the original swallow was
    // there as a frontrunning-tolerance hint, not a correctness
    // requirement.
    ISpoke(spoke).setUserPositionManagersWithSig(
      ISpoke.SetUserPositionManagers({
        onBehalfOf: onBehalfOf,
        updates: updates,
        nonce: nonce,
        deadline: deadline
      }),
      signature
    );
  }

  /// @inheritdoc IPositionManagerBase
  function permitReserveUnderlying(
    address spoke,
    uint256 reserveId,
    address onBehalfOf,
    uint256 value,
    uint256 deadline,
    uint8 permitV,
    bytes32 permitR,
    bytes32 permitS
  ) external onlyRegisteredSpoke(spoke) {
    address underlying = _getReserveUnderlying(spoke, reserveId);
    // PUYA-SOL PATCH (uros-splitter readiness, AVM port): try/catch is
    // unsupported on AVM (see WIP/examples/aave-v4/patches.md). Original:
    //   try IERC20Permit(underlying).permit({...}) {} catch {}
    // The OZ-recommended frontrunning-tolerant pattern was: swallow a
    // permit revert so a frontrunning attacker who pre-consumed the
    // permit nonce can't grief this entrypoint. AVM has no in-txn
    // recovery, so we forward the revert. Caller-side retry is the
    // recommended mitigation.
    IERC20Permit(underlying).permit({
      owner: onBehalfOf,
      spender: address(this),
      value: value,
      deadline: deadline,
      v: permitV,
      r: permitR,
      s: permitS
    });
  }

  /// @inheritdoc IPositionManagerBase
  function renouncePositionManagerRole(address spoke, address user) external onlyOwner {
    ISpoke(spoke).renouncePositionManagerRole(user);
  }

  /// @inheritdoc IMulticall
  function multicall(
    bytes[] calldata data
  ) public override(Multicall, IMulticall) returns (bytes[] memory) {
    require(_multicallEnabled(), UnsupportedAction());
    return super.multicall(data);
  }

  /// @inheritdoc IPositionManagerBase
  function isSpokeRegistered(address spoke) external view returns (bool) {
    return _isSpokeRegistered(spoke);
  }

  /// @dev Verifies the specified spoke is registered.
  function _isSpokeRegistered(address spoke) internal view returns (bool) {
    return _registeredSpokes[spoke];
  }

  /// @return The underlying asset for `reserveId` on the specified spoke.
  function _getReserveUnderlying(address spoke, uint256 reserveId) internal view returns (address) {
    return ISpoke(spoke).getReserve(reserveId).underlying;
  }

  /// @dev Flag to enable multicall usage. Needs to be set by the inheriting contracts.
  function _multicallEnabled() internal pure virtual returns (bool);

  function _rescueGuardian() internal view override returns (address) {
    return owner();
  }
}
