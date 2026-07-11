// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// ABI encode/decode round-trips of nested/dynamic types (built from scalar fuzz inputs).
contract AbiNested {
    struct Inner { uint64 x; int32 y; }
    struct Outer { Inner inner; uint256 z; bool flag; }
    function structRT(uint256 x, uint256 y, uint256 z, uint256 f) external pure returns (uint256) {
        Outer memory o = Outer(Inner(uint64(x), int32(int256(y))), z, f % 2 == 1);
        Outer memory d = abi.decode(abi.encode(o), (Outer));
        return uint256(d.inner.x) + uint256(int256(d.inner.y)) + d.z + (d.flag ? 1000 : 0);
    }
    function dynArrRT(uint256 n, uint256 seed) external pure returns (uint256) {
        uint256[] memory a = new uint256[](n % 10);
        for (uint256 k=0;k<a.length;k++) a[k] = seed + k * 7;
        uint256[] memory d = abi.decode(abi.encode(a), (uint256[]));
        uint256 s; for (uint256 k=0;k<d.length;k++) s += d[k]; return s;
    }
    function nestedArrRT(uint256 n, uint256 m) external pure returns (uint256) {
        uint256[][] memory a = new uint256[][](n % 4);
        for (uint256 i=0;i<a.length;i++){ a[i]=new uint256[](m % 4); for(uint256 j=0;j<a[i].length;j++) a[i][j]=i*10+j; }
        uint256[][] memory d = abi.decode(abi.encode(a), (uint256[][]));
        uint256 s; for(uint256 i=0;i<d.length;i++) for(uint256 j=0;j<d[i].length;j++) s+=d[i][j]; return s;
    }
    function structArrRT(uint256 n, uint256 seed) external pure returns (uint256) {
        Inner[] memory a = new Inner[](n % 6);
        for(uint256 k=0;k<a.length;k++) a[k]=Inner(uint64(seed+k), int32(int256(k)));
        Inner[] memory d = abi.decode(abi.encode(a), (Inner[]));
        uint256 s; for(uint256 k=0;k<d.length;k++) s += uint256(d[k].x) + uint256(int256(d[k].y)); return s;
    }
    function packedVsStd(uint128 a, uint128 b) external pure returns (uint256) {
        return abi.encodePacked(a, b).length * 1000 + abi.encode(a, b).length;
    }
}
