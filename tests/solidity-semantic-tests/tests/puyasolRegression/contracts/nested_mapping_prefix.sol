// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract NestedPrefix {
    struct Inner { mapping(uint256 => uint256) m; }
    struct Outer { Inner a; }
    struct Flat  { mapping(uint256 => uint256) m; }

    Outer st;
    Flat  st2;

    function readVia(mapping(uint256 => uint256) storage mm, uint256 k)
        internal view returns (uint256) { return mm[k]; }
    function writeVia(mapping(uint256 => uint256) storage mm, uint256 k, uint256 v)
        internal { mm[k] = v; }

    // depth-1 control: direct and param must agree
    function flatParam(uint256 k) external returns (uint256 direct, uint256 via) {
        st2.m[k] = 11;
        direct = st2.m[k];
        via = readVia(st2.m, k);
    }
    // depth-2: direct vs param
    function nestedParam(uint256 k) external returns (uint256 direct, uint256 via) {
        st.a.m[k] = 22;
        direct = st.a.m[k];
        via = readVia(st.a.m, k);
    }
    // depth-2 write via param, read direct
    function nestedWriteVia(uint256 k) external returns (uint256 direct) {
        writeVia(st.a.m, k, 33);
        direct = st.a.m[k];
    }
    // storage-local alias to the inner struct: direct-through-alias vs direct
    function aliasDirect(uint256 k) external returns (uint256 viaAlias, uint256 direct) {
        Inner storage p = st.a;
        st.a.m[k] = 44;
        viaAlias = p.m[k];
        direct = st.a.m[k];
    }
    // alias then param
    function aliasParam(uint256 k) external returns (uint256 via) {
        Inner storage p = st.a;
        st.a.m[k] = 55;
        via = readVia(p.m, k);
    }
}
