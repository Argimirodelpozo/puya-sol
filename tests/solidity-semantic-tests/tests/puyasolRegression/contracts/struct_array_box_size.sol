// Fixed array of STRUCTS: the box must be sized elemSize(struct) * N. The sizing
// switch defaulted to 32 B/element for struct elements (only uintN / bytesN /
// nested-static-array had cases), under-allocating the box for any struct whose
// ARC-4 encoding isn't exactly 32 B — element access then overran it.
// Found by the corpus-mutation fuzzer (memory_structs_read_write, uint16->int160).
contract C {
    struct Wide { uint8 x; int160 y; uint256 z; uint8[2] a; }   // 1+20+32+2 = 55 B
    struct Small { uint8 x; uint16 y; uint256 z; uint8[2] a; }  // 1+2+32+2  = 37 B
    Wide[5] wide;
    Small[5] small;

    function setWide(uint256 i, uint8 x, int160 y, uint256 z, uint8 a) public {
        wide[i].x = x; wide[i].y = y; wide[i].z = z; wide[i].a[1] = a;
    }
    function getWide(uint256 i) public view returns (uint8, int160, uint256, uint8) {
        Wide memory w = wide[i];                                  // storage -> memory copy
        return (w.x, w.y, w.z, w.a[1]);
    }
    function setSmall(uint256 i, uint256 z) public { small[i].z = z; }
    function getSmall(uint256 i) public view returns (uint256) { return small[i].z; }
}
