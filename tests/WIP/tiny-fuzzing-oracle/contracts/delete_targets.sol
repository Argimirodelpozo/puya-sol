// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract G {
    uint128[] arr;
    mapping(uint256 => uint64) m;
    struct S { uint64 a; int32 b; uint8 c; } S st;
    uint256 scalar;
    function pushArr(uint128 v) external { arr.push(v); }
    function setArr(uint256 i, uint128 v) external { if (i<arr.length) arr[i]=v; }
    function delArrElem(uint256 i) external { if (i<arr.length) delete arr[i]; }
    function delArr() external { delete arr; }
    function getArr(uint256 i) external view returns (uint128) { return i<arr.length?arr[i]:0; }
    function lenArr() external view returns (uint256) { return arr.length; }
    function setM(uint256 k, uint64 v) external { m[k]=v; }
    function delM(uint256 k) external { delete m[k]; }
    function getM(uint256 k) external view returns (uint64) { return m[k]; }
    function setSt(uint64 a, int32 b, uint8 c) external { st=S(a,b,c); }
    function delSt() external { delete st; }
    function delStField() external { delete st.b; }
    function getStA() external view returns (uint64){return st.a;}
    function getStB() external view returns (int32){return st.b;}
    function getStC() external view returns (uint8){return st.c;}
    function setScalar(uint256 v) external { scalar=v; }
    function delScalar() external { delete scalar; }
    function getScalar() external view returns (uint256){return scalar;}
}
