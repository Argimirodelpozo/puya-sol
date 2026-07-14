// Directed probe for the COLD conversion paths (coverage_map: SolTypeConversion
// int<->bytesN at 42.6%). int<->bytesN<->int round-trips across widths, signed
// and unsigned. Address excluded (known by-design 20-vs-32 divergence).
contract C {
    // int -> bytesN (handleIntToBytes / handleBiguintToBytes) at each valid width
    function u2b32(uint256 x) public pure returns (bytes32) { return bytes32(x); }
    function u2b16(uint128 x) public pure returns (bytes16) { return bytes16(x); }
    function u2b8(uint64 x)  public pure returns (bytes8)  { return bytes8(x); }
    function u2b4(uint32 x)  public pure returns (bytes4)  { return bytes4(x); }
    function u2b1(uint8 x)   public pure returns (bytes1)  { return bytes1(x); }
    function u2b3(uint24 x)  public pure returns (bytes3)  { return bytes3(x); }
    function u2b20(uint160 x) public pure returns (bytes20){ return bytes20(x); }

    // signed int -> uint -> bytesN
    function i2b32(int256 x) public pure returns (bytes32) { return bytes32(uint256(x)); }
    function i2b8(int64 x)   public pure returns (bytes8)  { return bytes8(uint64(x)); }
    function i2b3(int24 x)   public pure returns (bytes3)  { return bytes3(uint24(x)); }

    // bytesN -> int (handleBytesToInt)
    function b2u32(bytes32 b) public pure returns (uint256) { return uint256(b); }
    function b2u8(bytes8 b)   public pure returns (uint64)  { return uint64(b); }
    function b2u4(bytes4 b)   public pure returns (uint32)  { return uint32(b); }
    function b2u3(bytes3 b)   public pure returns (uint24)  { return uint24(b); }
    function b2i8(bytes8 b)   public pure returns (int64)   { return int64(uint64(b)); }

    // round-trips (identity)
    function rt_u2b2u(uint64 x)  public pure returns (uint64)  { return uint64(bytes8(x)); }
    function rt_b2u2b(bytes8 b)  public pure returns (bytes8)  { return bytes8(uint64(b)); }
    function rt_u3(uint24 x)     public pure returns (uint24)  { return uint24(bytes3(x)); }

    // bytesN resize (truncate high / extend low with zeros)
    function bnarrow(bytes8 b) public pure returns (bytes4) { return bytes4(b); }
    function bwiden(bytes4 b)  public pure returns (bytes8) { return bytes8(b); }
    function bnarrow1(bytes32 b) public pure returns (bytes1) { return bytes1(b); }
}
