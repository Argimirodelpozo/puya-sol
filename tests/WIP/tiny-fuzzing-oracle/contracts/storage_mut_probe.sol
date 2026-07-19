// Cold probe: storage inc/dec/delete/compound-assign across packed sub-word,
// signed, mapping, struct-field, array-elem targets (SolUnaryOperation +
// SolAssignment storage-mutation cold lines; historically bug-rich).
contract C {
    uint8 pu8; int8 pi8; uint16 pu16; int16 pi16; uint64 u64; int64 i64;  // packed
    uint128 u128; int128 i128;
    mapping(uint256 => int64) msi;
    mapping(uint256 => uint8) mu8;
    uint32[] arr;
    struct S { uint8 a; int16 b; uint64 c; }
    S s;

    constructor() { arr.push(0); arr.push(0); arr.push(0); }

    // inc/dec on packed sub-word (wrap at boundaries when unchecked)
    function incU8() public { unchecked { pu8++; } }
    function decU8() public { unchecked { pu8--; } }
    function incI8() public { unchecked { pi8++; } }
    function decI8() public { unchecked { pi8--; } }
    function setU8(uint8 v) public { pu8 = v; }
    function setI8(int8 v) public { pi8 = v; }
    function getU8() public view returns (uint8) { return pu8; }
    function getI8() public view returns (int8) { return pi8; }

    // compound assign on packed signed
    function addI16(int16 v) public { pi16 += v; }
    function mulI16(int16 v) public { unchecked { pi16 *= v; } }
    function getI16() public view returns (int16) { return pi16; }

    // mapping value inc/dec/compound (signed)
    function mIncr(uint256 k) public { msi[k]++; }
    function mAdd(uint256 k, int64 v) public { msi[k] += v; }
    function mDel(uint256 k) public { delete msi[k]; }
    function mGet(uint256 k) public view returns (int64) { return msi[k]; }
    function mu8Incr(uint256 k) public { unchecked { mu8[k]++; } }
    function mu8Get(uint256 k) public view returns (uint8) { return mu8[k]; }

    // struct field inc/dec/compound + delete
    function sIncA() public { unchecked { s.a++; } }
    function sAddB(int16 v) public { s.b += v; }
    function sMulC(uint64 v) public { unchecked { s.c *= v; } }
    function sDel() public { delete s; }
    function sGet() public view returns (uint8, int16, uint64) { return (s.a, s.b, s.c); }

    // array elem compound + inc
    function aAdd(uint256 i, uint32 v) public { arr[i] += v; }
    function aIncr(uint256 i) public { unchecked { arr[i]++; } }
    function aDel(uint256 i) public { delete arr[i]; }
    function aGet(uint256 i) public view returns (uint32) { return arr[i]; }
}
