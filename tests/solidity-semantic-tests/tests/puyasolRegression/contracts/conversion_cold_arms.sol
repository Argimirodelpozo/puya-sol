// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract ConvHelper { function ping() external pure returns (uint256) { return 1; } }

contract ConvProbe2 {
    ConvHelper public h;
    constructor() { h = new ConvHelper(); }

    // address(0) literal fast path.
    function zeroAddr() public pure returns (address) {
        return address(0);
    }
    // contract-typed value (application carrier) -> address.
    function contractToAddr() public view returns (bool) {
        address a = address(h);
        return a != address(0);
    }
    // small-tier bytesN -> uintN (narrowing-mask arms).
    function smallBytesToInt(bytes1 a, bytes2 b, bytes8 c)
        public pure returns (uint8, uint16, uint64)
    {
        return (uint8(a), uint16(b), uint64(c));
    }
    // same-width uintN -> bytesN at the uint64-carrier tiers.
    function smallIntToBytes(uint8 a, uint16 b, uint64 c)
        public pure returns (bytes1, bytes2, bytes8)
    {
        return (bytes1(a), bytes2(b), bytes8(c));
    }
    // uint -> address (through uint160) at numeric-literal + huge values.
    function numToAddr() public pure returns (address, address) {
        return (
            address(uint160(0xDEAD)),
            address(uint160(type(uint160).max))
        );
    }
    // bytes12/bytes20 mid-tier conversions (biguint carrier).
    function midTiers(bytes12 a, bytes20 b) public pure returns (uint96, uint160) {
        return (uint96(a), uint160(b));
    }
}
