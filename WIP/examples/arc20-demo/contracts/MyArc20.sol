// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "tokens/ARC20.sol";

/// @notice Concrete, deployable ARC-20 Smart ASA for the demo/tests. All behaviour
/// lives in the ARC20 base (WIP/tokens/ARC20.sol); this is the deploy target, mirroring
/// the aerc20-demo's `MyToken is AERC20` pattern.
contract MyArc20 is ARC20 {}
