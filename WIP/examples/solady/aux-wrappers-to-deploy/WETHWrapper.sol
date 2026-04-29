// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/tokens/WETH.sol";

// WETH is already a concrete contract, just re-export
contract WETHWrapper is WETH {}
