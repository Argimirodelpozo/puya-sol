// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/Base64.sol";

contract Base64Wrapper {
    function encode(bytes calldata data) external pure returns (string memory) {
        return Base64.encode(data);
    }

    function decode(string calldata data) external pure returns (bytes memory) {
        return Base64.decode(data);
    }
}
