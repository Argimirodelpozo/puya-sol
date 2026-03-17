// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/LibRLP.sol";

contract LibRLPWrapper {
    function computeAddress(address deployer, uint256 nonce) external pure returns (address) {
        return LibRLP.computeAddress(deployer, nonce);
    }
}
