// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/Base58.sol";

contract Base58Wrapper {
    function encode(bytes calldata data) external pure returns (string memory) {
        return Base58.encode(data);
    }

    function decode(string calldata data) external pure returns (bytes memory) {
        return Base58.decode(data);
    }
}
