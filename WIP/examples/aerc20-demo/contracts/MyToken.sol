// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "tokens/AERC20.sol";

contract MyToken is AERC20 {
    constructor() AERC20(1_000_000, 6, "My Token", "MTK") {}
}
