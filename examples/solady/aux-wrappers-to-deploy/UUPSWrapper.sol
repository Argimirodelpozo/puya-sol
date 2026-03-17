// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/UUPSUpgradeable.sol";

contract UUPSWrapper is UUPSUpgradeable {
    function _authorizeUpgrade(address) internal override {}
}
