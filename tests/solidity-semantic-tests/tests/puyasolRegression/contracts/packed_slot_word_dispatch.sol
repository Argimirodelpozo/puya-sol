contract C {
    uint8 a;      // slot0 byte 0
    int32 b;      // slot0 bytes 1-4
    uint64 c;     // slot0 bytes 5-12
    bytes8 d;     // slot0 bytes 13-20
    bool e;       // slot0 byte 21
    uint256 big;  // slot1 (full, legacy path)
    function readWord() public view returns (uint256 w) { assembly { w := sload(0) } }
    function writeWord(uint256 w) public { assembly { sstore(0, w) } }
    function set(uint8 x, int32 y, uint64 z, bytes8 u, bool v) public { a=x; b=y; c=z; d=u; e=v; }
    function get() public view returns (uint8, int32, uint64, bytes8, bool) { return (a, b, c, d, e); }
}
