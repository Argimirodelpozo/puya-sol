pragma solidity >=0.8.0;

library VendoredHelpers {
    // This mirrors memview-sol's unused Bitcoin hash160 helper. The AVM has no
    // RIPEMD-160 precompile, but an unreachable library function must not make
    // a contract that never uses it fail compilation.
    function deadHash160(bytes memory input) internal view returns (bytes20 digest) {
        assembly {
            let ptr := mload(0x40)
            pop(staticcall(gas(), 2, add(input, 0x20), mload(input), ptr, 0x20))
            pop(staticcall(gas(), 3, ptr, 0x20, ptr, 0x20))
            digest := mload(add(ptr, 0x0c))
        }
    }

    function twice(uint256 value) internal pure returns (uint256) {
        return add(value, value);
    }

    // solc's contract graph can stop at a library entrypoint. The frontend's
    // closure must retain helpers referenced from that reachable entrypoint.
    function add(uint256 a, uint256 b) private pure returns (uint256) {
        return a + b;
    }
}

contract C {
    function f(uint256 value) external pure returns (uint256) {
        return VendoredHelpers.twice(value);
    }
}
