// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/DynamicBufferLib.sol";

contract DynamicBufferWrapper {
    using DynamicBufferLib for DynamicBufferLib.DynamicBuffer;

    function testAppend(bytes calldata a, bytes calldata b) external pure returns (bytes memory) {
        DynamicBufferLib.DynamicBuffer memory buffer;
        buffer.p(a);
        buffer.p(b);
        return buffer.data;
    }
}
