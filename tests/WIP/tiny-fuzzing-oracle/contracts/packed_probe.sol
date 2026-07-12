// Packed-slot codec differential probe: EVM packs slot0/slot1; puya-sol keeps
// per-var typed cells and assembles/splits the word in the asm dispatcher.
// All fn I/O is int/bool (no address/bytesN returns = no by-design divergence);
// the packed bytes8 is exercised through uint64 casts and the raw words.
contract PackedProbe {
    uint8 a;      // slot0 byte 0
    int32 b;      // slot0 bytes 1-4
    uint64 c;     // slot0 bytes 5-12
    bytes8 d;     // slot0 bytes 13-20
    bool e;       // slot0 byte 21
    int128 f;     // slot1 bytes 0-15
    uint96 g;     // slot1 bytes 16-27
    int16 h;      // slot1 bytes 28-29
    uint256 full; // slot2 (full-slot legacy path)
    int256 sfull; // slot3

    function setA(uint8 v) public { a = v; }
    function setB(int32 v) public { b = v; }
    function setC(uint64 v) public { c = v; }
    function setD(uint64 v) public { d = bytes8(v); }
    function setE(bool v) public { e = v; }
    function setF(int128 v) public { f = v; }
    function setG(uint96 v) public { g = v; }
    function setH(int16 v) public { h = v; }
    function setFull(uint256 v) public { full = v; }
    function setSFull(int256 v) public { sfull = v; }

    function getA() public view returns (uint8) { return a; }
    function getB() public view returns (int32) { return b; }
    function getC() public view returns (uint64) { return c; }
    function getD() public view returns (uint64) { return uint64(d); }
    function getE() public view returns (bool) { return e; }
    function getF() public view returns (int128) { return f; }
    function getG() public view returns (uint96) { return g; }
    function getH() public view returns (int16) { return h; }
    function getFull() public view returns (uint256) { return full; }
    function getSFull() public view returns (int256) { return sfull; }

    function word0() public view returns (uint256 w) { assembly { w := sload(0) } }
    function word1() public view returns (uint256 w) { assembly { w := sload(1) } }
    function word2() public view returns (uint256 w) { assembly { w := sload(2) } }
    function writeWord0(uint256 w) public { assembly { sstore(0, w) } }
    function writeWord1(uint256 w) public { assembly { sstore(1, w) } }
    function writeWord2(uint256 w) public { assembly { sstore(2, w) } }
    function readAt(uint256 s) public view returns (uint256 w) { assembly { w := sload(s) } }
    function writeAt(uint256 s, uint256 w) public { assembly { sstore(s, w) } }
}
