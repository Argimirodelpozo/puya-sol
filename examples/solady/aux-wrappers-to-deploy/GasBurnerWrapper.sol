// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/GasBurnerLib.sol";

contract GasBurnerWrapper {
    function burn(uint256 x) external returns (uint256) {
        GasBurnerLib.burn(x);
        return x;
    }
}
