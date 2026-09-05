// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract TransientBase {
    uint128 transient internal baseValue;
    bool transient internal flag;
}

contract TransientSchema is TransientBase {
    uint128 transient public value;

    function run() external returns (uint128, uint128, bool) {
        baseValue = 11;
        value = 23;
        flag = true;
        return (baseValue, value, flag);
    }
}

contract MixedTransientSchema is TransientSchema {
    uint64 public persistent = 17;
}
