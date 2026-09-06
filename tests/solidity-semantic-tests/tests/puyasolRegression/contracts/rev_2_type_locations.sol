// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract TypeLocations {
    struct Value { uint16 scalar; uint16[2] items; }
    Value internal stored;

    function memoryValue(Value memory value) internal pure returns (Value memory) {
        return value;
    }
    function storageValue(Value storage value) internal {
        value.scalar += 1;
        value.items[0] += 2;
    }
    function run(Value calldata value) external returns (uint16, uint16, uint16, uint16) {
        Value memory local = memoryValue(value);
        stored = local;
        Value storage aliasValue = stored;
        storageValue(aliasValue);
        return (local.scalar, local.items[0], aliasValue.scalar, aliasValue.items[0]);
    }
}
