// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
contract PackedStruct {
    struct Packed { bool flag; uint8 a; uint16 b; uint32 c; address owner; bool flag2; }
    Packed ps;
    mapping(uint256 => Packed) m;
    function setAll(bool f, uint8 a, uint16 b, uint32 c, address o, bool f2) external {
        ps = Packed(f, a, b, c, o, f2);
    }
    function getAll() external view returns (bool,uint8,uint16,uint32,address,bool) {
        return (ps.flag, ps.a, ps.b, ps.c, ps.owner, ps.flag2);
    }
    function setFlag(bool f) external { ps.flag = f; }        // mutate one packed field; siblings must survive
    function setFlag2(bool f) external { ps.flag2 = f; }
    function bumpA(uint8 d) external { ps.a += d; }
    function setB(uint16 v) external { ps.b = v; }
    function setOwner(address o) external { ps.owner = o; }
    // in mapping
    function mSet(uint256 k, bool f, uint8 a, uint32 c) external { m[k] = Packed(f, a, 0, c, address(0), false); }
    function mSetFlag(uint256 k, bool f) external { m[k].flag = f; }
    function mGet(uint256 k) external view returns (bool,uint8,uint32) { return (m[k].flag, m[k].a, m[k].c); }
}
