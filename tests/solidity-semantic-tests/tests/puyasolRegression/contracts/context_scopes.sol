// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

function initialValue(uint256 value) pure returns (uint256) {
    return value * 7;
}

contract ContextBase {
    uint256 public initialized = initialValue(6);
    uint256 public base;

    constructor(uint256 seed) { base = seed + 1; }
}

contract ContextScopes is ContextBase {
    uint8 private total;

    constructor(uint256 seed) ContextBase(initialValue(seed)) {}

    function reset() external { total = 0; }

    modifier twice(uint8 value) {
        unchecked { value++; }
        { _; }
        if (value == 0) _;
    }

    function repeated(uint8 x) external twice(255) twice(255) returns (uint8) {
        total += x;
        return total;
    }

    function nested(uint8 x) external pure returns (uint8) {
        unchecked {
            { if (x > 0) { x++; } }
            for (uint8 i = 0; i < 2; ++i) {
                if (i == 0) continue;
                x++;
            }
            uint8 n = 0;
            do {
                ++n;
                if (n == 1) continue;
                x++;
            } while (n < 2);
            while (x < 3) {
                ++x;
                if (x == 2) break;
            }
        }
        return x + 1;
    }

    function checkedAfter(uint8 x) external pure returns (uint8) {
        unchecked { if (x < 255) ++x; }
        return x + 1;
    }

    function checkedHelper(uint8 x) internal pure returns (uint8) { return x + 1; }

    function checkedCall(uint8 x) external pure returns (uint8) {
        unchecked { return checkedHelper(x); }
    }

    function shifted(bytes calldata data) external pure returns (bytes memory) {
        assembly {
            data.offset := add(data.offset, 1)
            data.length := sub(data.length, 1)
        }
        {
            assembly {
                data.offset := add(data.offset, 1)
                data.length := sub(data.length, 1)
            }
        }
        return data;
    }

    function original(bytes calldata data) external pure returns (bytes memory) {
        return data;
    }
}
