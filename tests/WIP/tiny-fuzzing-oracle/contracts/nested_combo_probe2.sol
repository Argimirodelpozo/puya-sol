// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Probe 2: depth-3 nesting, dyn-array fields inside mapped structs, whole-aggregate
// copies at depth, nested-struct ABI returns/params, memory round-trips, delete-at-depth.
contract G {
    struct L0 { int32 a; uint64 b; }
    struct L1 { L0 inner; int64[] xs; bool flag; }
    struct L2 { uint128 tag; L1 mid; }

    L2 s;
    mapping(uint256 => L2) m;
    L2[] arr;

    function setDeep(uint256 k, int32 a, uint64 b, bool f, uint128 t) external {
        m[k].mid.inner = L0(a, b);
        m[k].mid.flag = f;
        m[k].tag = t;
    }
    function pushX(uint256 k, int64 v) external { m[k].mid.xs.push(v); }
    function getX(uint256 k, uint256 i) external view returns (int64) {
        return i < m[k].mid.xs.length ? m[k].mid.xs[i] : int64(-1);
    }
    function lenX(uint256 k) external view returns (uint256) { return m[k].mid.xs.length; }
    function getDeepA(uint256 k) external view returns (int32) { return m[k].mid.inner.a; }
    function bumpDeepA(uint256 k, int32 d) external { unchecked { m[k].mid.inner.a += d; } }

    // whole-aggregate copies at depth
    function storeToS(uint256 k) external { s = m[k]; }               // storage <- storage(map)
    function sToMap(uint256 k) external { m[k] = s; }                 // map <- storage
    function memRoundTrip(uint256 k, int32 na) external {             // memory round-trip
        L2 memory tmp = m[k];
        tmp.mid.inner.a = na;
        m[k] = tmp;
    }
    function copyInner(uint256 kFrom, uint256 kTo) external { m[kTo].mid.inner = m[kFrom].mid.inner; }
    function delMid(uint256 k) external { delete m[k].mid; }
    function delAll(uint256 k) external { delete m[k]; }

    // nested struct through the ABI (external return + param)
    function getWhole(uint256 k) external view returns (L2 memory) { return m[k]; }
    function putWhole(uint256 k, L2 memory v) external { m[k] = v; }

    // array of nested structs
    function pushArr(uint128 t, int32 a) external { arr.push(L2(t, L1(L0(a, 5), new int64[](0), true))); }
    function arrDeepA(uint256 i) external view returns (int32) { return i < arr.length ? arr[i].mid.inner.a : int32(-1); }
    function sGetA() external view returns (int32) { return s.mid.inner.a; }
    function sGetTag() external view returns (uint128) { return s.tag; }
}
