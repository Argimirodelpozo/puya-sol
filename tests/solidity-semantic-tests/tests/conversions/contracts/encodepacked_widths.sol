// CUSTOM (puya-sol): abi.encodePacked must use EVM packed widths. Enum was
// the bug — packed as 8-byte native word instead of its 1-byte uint8
// encoding, corrupting keccak hashes and shifting following args. Address
// packs as the full 32-byte AVM account (EVM uses 20) — AVM-fundamental.
contract C {
    enum E { A, B, C }
    function pEnum(E e) public pure returns (bytes memory) { return abi.encodePacked(e); }
    function pMix(uint8 x, E e) public pure returns (bytes memory) { return abi.encodePacked(x, e); }
    function pI8neg() public pure returns (bytes memory) { return abi.encodePacked(int8(-3)); }
    function pI128neg() public pure returns (bytes memory) { return abi.encodePacked(int128(-3)); }
    function pAddr(address a) public pure returns (bytes memory) { return abi.encodePacked(a); }
}
