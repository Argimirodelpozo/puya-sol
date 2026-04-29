// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/BlockHashLib.sol";

contract BlockHashWrapper {
    function blockHash(uint256 blockNumber) external view returns (bytes32) {
        return BlockHashLib.blockHash(blockNumber);
    }
}
