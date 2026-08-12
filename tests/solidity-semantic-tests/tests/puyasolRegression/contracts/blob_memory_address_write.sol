// An ADDRESS written into a blob-backed memory array. The blob-memory write
// path pins its value temp as biguint, but `implicitNumericCast` only converts
// NUMERIC sources — an account passed through unchanged and puya rejected the
// store ("assignment target type differs"). Verbatim shape of Aave V3 Pool's
// getReservesList: elements copied from a mapping, then the trailing
// `mstore(arr, len)` length-word write that makes the array blob-backed.
contract C {
    mapping(uint256 => address) _list;
    uint256 _count;

    function seed(address a, address b) public {
        _list[0] = a;
        _list[1] = b;
        _count = 3;   // one slot more than we fill, so the shrink is observable
    }

    function copyOut() public view returns (address[] memory) {
        uint256 n = _count;
        uint256 dropped = 0;
        address[] memory out = new address[](n);
        for (uint256 i = 0; i < n; i++) {
            if (_list[i] != address(0)) {
                out[i - dropped] = _list[i];
            } else {
                dropped++;
            }
        }
        assembly { mstore(out, sub(n, dropped)) }
        return out;
    }
}

// Second guard: the element-offset bug is TYPE-INDEPENDENT (found via a
// uint256 twin while isolating the address failure above). A blob-backed
// dynamic array's pointer addresses its LENGTH word, so element i lives at
// +32+i*32; the writer omitted the header and every element landed one slot
// early — element 0 clobbered the length and the last element was never
// written. Values here are distinct so any shift is visible.
contract Shift {
    mapping(uint256 => uint256) _m;
    function seed() public { _m[0]=11; _m[1]=22; _m[2]=33; }
    // NOTE: a LOCAL (not a named return) — the blob-aggregate registry keys on
    // the local declaration, so `mstore` on a named return is a different
    // (unsupported) shape that fails to coerce.
    function blobCopy() public view returns (uint256[] memory) {
        uint256[] memory out = new uint256[](3);
        for (uint256 i = 0; i < 3; i++) { out[i] = _m[i]; }
        assembly { mstore(out, 3) }   // marks `out` blob-backed
        return out;
    }
    function plainCopy() public view returns (uint256[] memory) {
        uint256[] memory out = new uint256[](3);
        for (uint256 i = 0; i < 3; i++) { out[i] = _m[i]; }
        return out;
    }
}
