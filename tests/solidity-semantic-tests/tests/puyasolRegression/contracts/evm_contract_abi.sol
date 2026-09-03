// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {ARC4} from "libs/AVM.sol";

contract EvmContractAbi {
    struct Record {
        uint16 id;
        string name;
        uint32[] values;
    }

    function ping() external pure returns (uint256) {
        return 77;
    }

    function scalars(uint16 x, bool b, bytes3 tag)
        external pure returns (uint16, bool, bytes3)
    {
        return (x + 1, !b, tag);
    }

    function signed(int16 x, int128 y)
        external pure returns (int16, int128)
    {
        return (x - 1, y + 2);
    }

    function nested(uint32[][][] memory values)
        external pure returns (uint32[][][] memory)
    {
        return values;
    }

    function record(Record memory item)
        external pure returns (Record memory)
    {
        item.id += 1;
        return item;
    }

    function codec(uint16 x, bytes memory data)
        external pure returns (bytes memory)
    {
        return abi.encode(x, data);
    }

    function arc4Codec(uint16 x, bytes memory data)
        external pure returns (uint16, bytes memory)
    {
        return abi.decode(
            ARC4.decode(ARC4.encode(abi.encode(x, data))),
            (uint16, bytes)
        );
    }

    function senderIdentity(address expected)
        external view returns (bool highLevel, bool assemblyLevel, address sender)
    {
        uint256 rawCaller;
        assembly {
            rawCaller := caller()
        }
        return (msg.sender == expected, rawCaller == uint160(expected), msg.sender);
    }

    fallback(bytes calldata input) external returns (bytes memory output) {
        return input;
    }
}

contract EvmTarget {
    function bump(uint16 x) external pure returns (uint16) {
        return x + 1;
    }
}

contract EvmCaller {
    function forward(address target, uint16 x) external returns (uint16) {
        return EvmTarget(target).bump(x);
    }
}

contract EvmConstructed {
    uint16 private value;

    constructor(uint16 initialValue) {
        value = initialValue;
    }

    function get() external view returns (uint16) {
        return value;
    }
}

contract EvmFactory {
    function makeAndRead(uint16 initialValue) external returns (uint16) {
        EvmConstructed child = new EvmConstructed(initialValue);
        return child.get();
    }
}
