// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Coverage lane for calls/SolTypeConversion.cpp: the explicit-conversion
// shapes (enum casts both ways, int<->bytesN at every width tier, address
// and contract casts, narrowing masks) exercised with value assertions.
contract ExplicitConversionMatrix {
    enum Mode { A, B, C }

    function enumRoundTrip(uint8 raw) public pure returns (uint8) {
        Mode m = Mode(raw);          // uint -> enum (range-checked)
        return uint8(m);             // enum -> uint
    }

    function intToBytesTiers()
        public pure
        returns (bytes1, bytes4, bytes8, bytes16, bytes32)
    {
        // int -> bytesN across the uint64/biguint carrier boundary.
        return (
            bytes1(uint8(0xAB)),
            bytes4(uint32(0xDEADBEEF)),
            bytes8(uint64(0x1122334455667788)),
            bytes16(uint128(2 ** 100 + 7)),
            bytes32(uint256(2 ** 200 + 9))
        );
    }

    function bytesToIntTiers(bytes4 a, bytes32 b)
        public pure returns (uint32, uint256)
    {
        return (uint32(a), uint256(b));
    }

    function narrowingMasks(uint256 wide)
        public pure returns (uint8, uint32, uint64, uint128)
    {
        // Explicit narrowing keeps only the low bits — the masking arm.
        return (uint8(wide), uint32(wide), uint64(wide), uint128(wide));
    }

    function signedNarrowWiden(int256 v) public pure returns (int8, int256) {
        int8 narrow = int8(v);
        return (narrow, int256(narrow));   // widen sign-extends back
    }

    function addressCasts(address a)
        public view returns (uint160, address, address)
    {
        uint160 asInt = uint160(a);        // address -> uint160
        address back = address(asInt);     // uint160 -> address
        address self = address(this);      // contract -> address
        return (asInt, back, self);
    }

    function contractCast() public view returns (ExplicitConversionMatrix) {
        // address -> contract type
        return ExplicitConversionMatrix(payable(address(this)));
    }

    function bytesNResize(bytes4 v) public pure returns (bytes2, bytes8) {
        return (bytes2(v), bytes8(v));     // truncate keeps HIGH bytes; widen pads LOW
    }
}
