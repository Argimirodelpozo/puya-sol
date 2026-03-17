// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/accounts/Timelock.sol";

// Timelock is already a concrete contract
contract TimelockWrapper is Timelock {}
