// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {ARC4 as Codec, Bits} from "libs/AVM.sol";

contract Arc4StdlibValid {
    function bitLength(uint256 value) external pure returns (uint256) {
        return Bits.bitlen(value);
    }

    function encodePair() external pure returns (bytes memory) {
        uint16 small = 0x1234;
        int32 delta = -7;
        return Codec.encode(abi.encode(small, delta));
    }

    function transcode(bytes memory data)
        external pure returns (bytes memory)
    {
        (uint16 small, int32 delta) =
            abi.decode(Codec.decode(data), (uint16, int32));
        return Codec.encode(abi.encode(small, delta));
    }
}
