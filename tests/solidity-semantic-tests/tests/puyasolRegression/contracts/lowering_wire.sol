pragma solidity ^0.8.20;

contract WireSink {
    function pack(uint8 a, uint24 b, int16 c, bool d, bytes3 e, string memory f, uint8[] memory g)
        external pure returns (bytes32)
    {
        return keccak256(abi.encode(a, b, c, d, e, f, g));
    }
}

library SelfRoute {
    function callSelf(bytes memory data) internal returns (bytes memory) {
        (bool ok, bytes memory result) = ((address(this))).call((data));
        require(ok);
        return result;
    }
}

contract WireProbe {
    function typed(address other, bool pointer) external returns (bytes32) {
        uint8[] memory values = new uint8[](2);
        values[0] = 7;
        values[1] = 255;
        if (pointer) {
            function(uint8, uint24, int16, bool, bytes3, string memory, uint8[] memory)
                external pure returns (bytes32) target = WireSink(other).pack;
            return target(255, 0xabcdef, -7, true, hex"010203", "abc", values);
        }
        return WireSink(other).pack(255, 0xabcdef, -7, true, hex"010203", "abc", values);
    }

    function raw(address other, bool selector) external returns (bytes32) {
        uint8[] memory values = new uint8[](2);
        values[0] = 7;
        values[1] = 255;
        bytes memory data = selector
            ? abi.encodeWithSelector(WireSink.pack.selector, uint8(255), uint24(0xabcdef),
                int16(-7), true, bytes3(hex"010203"), "abc", values)
            : abi.encodeCall(WireSink.pack, (255, 0xabcdef, -7, true, hex"010203", "abc", values));
        (bool ok, bytes memory result) = other.call(data);
        require(ok);
        return abi.decode(result, (bytes32));
    }

    function ping(uint256 value) external pure returns (uint256) { return value + 1; }
    function librarySelf() external returns (uint256) {
        return abi.decode(SelfRoute.callSelf(abi.encodeCall(this.ping, (41))), (uint256));
    }
}
