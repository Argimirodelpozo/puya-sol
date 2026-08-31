// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// T1: user-defined value types.
type Price is uint128;

library PriceMath {
    function add(Price a, Price b) internal pure returns (Price) {
        return Price.wrap(Price.unwrap(a) + Price.unwrap(b));
    }
}

contract UdvtProbe {
    using PriceMath for Price;
    Price public stored;

    function roundtrip(uint128 v) public pure returns (uint128) {
        Price p = Price.wrap(v);
        return Price.unwrap(p);
    }

    function addStore(uint128 a, uint128 b) public returns (uint128) {
        stored = Price.wrap(a).add(Price.wrap(b));
        return Price.unwrap(stored);
    }
}
