// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/LibBytes.sol";

contract LibBytesWrapper {
    function load(bytes calldata data, uint256 offset) external pure returns (bytes32) {
        return LibBytes.load(data, offset);
    }
}
