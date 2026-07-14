// int -> enum conversion must range-check the FULL value. The check truncated a
// wide biguint input to uint64 FIRST, so an out-of-range value whose LOW 64 bits
// form a valid ordinal (int136 -2^135 -> low64 == 0) returned the wrong member
// instead of Panic(0x21). Found by the corpus-mutation fuzzer
// (internal_library_function_attached_to_enum uint256->int136).
contract C {
    enum E { A, B }   // 2 members; valid 0,1
    function fromI136(int136 x) public pure returns (uint8) { return uint8(E(x)); }
    function fromI200(int200 x) public pure returns (uint8) { return uint8(E(x)); }
    function fromU256(uint256 x) public pure returns (uint8) { return uint8(E(x)); }
    function fromI16(int16 x)  public pure returns (uint8) { return uint8(E(x)); }
}
