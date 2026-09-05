// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

type SignedWord is int16;

contract SharedReturnPlan {
    int8 public small = -7;
    int72 public medium = -9;
    uint128 public wide = uint128(1) << 100;

    modifier pass() { _; }

    function pair(int8 x) public pure pass returns (int8 signedValue, uint128 wideValue) {
        return (x, uint128(1) << 100);
    }

    function internalPair(int8 x) external pure returns (int8, uint128) {
        return pair(x);
    }

    function externalPair(int8 x) external view returns (int8, uint128) {
        return this.pair(x);
    }

    function wrapped(SignedWord x) external pure returns (SignedWord) {
        return x;
    }

    function getterCalls() external view returns (int8, int72, uint128) {
        return (this.small(), this.medium(), this.wide());
    }
}
