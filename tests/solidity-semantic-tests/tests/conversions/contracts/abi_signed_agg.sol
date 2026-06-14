// CUSTOM (puya-sol): abi.encode of a SIGNED sub-256 (int128) value inside an
// aggregate must SIGN-extend a negative value to the 32-byte word, not
// zero-pad. Two sites: struct fields (toPackedBytes) and array elements
// (encodeDynArrayPadSmallElems).
contract C {
    struct P { uint256 a; int128 b; address c; bool d; }
    function encS(P memory p) public pure returns (bytes memory) { return abi.encode(p); }
    function encArr(int128[] memory a) public pure returns (bytes memory) { return abi.encode(a); }
}
