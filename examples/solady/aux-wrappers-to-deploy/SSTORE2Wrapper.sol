// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/SSTORE2.sol";

contract SSTORE2Wrapper {
    function write(bytes calldata data) external returns (address) {
        return SSTORE2.write(data);
    }

    function read(address pointer) external view returns (bytes memory) {
        return SSTORE2.read(pointer);
    }
}
