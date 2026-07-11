// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
contract Cee {
    function litu(uint256 a) external pure returns (uint64, uint128) { return (uint64(a), uint128(a)); }  // literal tuple
    function lits(int256 a) external pure returns (int64, int128) { return (int64(a), int128(a)); }        // signed literal tuple
    function mk() internal pure returns (uint64, uint128) { return (1, 2); }
    function opaque() external pure returns (uint64, uint128) { return mk(); }                             // opaque spill
    function mks(int256 a) internal pure returns (int64, uint128) { return (int64(a), uint128(uint256(a))); }
    function opaques(int256 a) external pure returns (int64, uint128) { return mks(a); }                   // signed opaque spill
    function ternu(uint256 a) external pure returns (uint64, uint128) { return a % 2 == 0 ? (uint64(1),uint128(2)) : (uint64(3),uint128(4)); } // unsigned ternary
    function terns(int256 a) external pure returns (int64, int128) { return a > 0 ? (int64(1),int128(2)) : (int64(-1),int128(-2)); }           // SIGNED ternary (old Pass4 didn't handle!)
    function namedt(uint256 a) external pure returns (uint64 x, uint128 y) { x = uint64(a); y = uint128(a); } // named tuple, implicit return
}
contract Cer {
    Cee c;
    constructor() { c = new Cee(); }
    function glitu(uint256 a) external returns (uint256) { (uint64 x,uint128 y)=c.litu(a); return uint256(x)+uint256(y); }
    function glits(int256 a) external returns (int256) { (int64 x,int128 y)=c.lits(a); return int256(x)+int256(y); }
    function gopaque() external returns (uint256) { (uint64 x,uint128 y)=c.opaque(); return uint256(x)+uint256(y); }
    function gopaques(int256 a) external returns (int256) { (int64 x,uint128 y)=c.opaques(a); return int256(x)+int256(uint256(y)); }
    function gternu(uint256 a) external returns (uint256) { (uint64 x,uint128 y)=c.ternu(a); return uint256(x)+uint256(y); }
    function gterns(int256 a) external returns (int256) { (int64 x,int128 y)=c.terns(a); return int256(x)+int256(y); }
    function gnamedt(uint256 a) external returns (uint256) { (uint64 x,uint128 y)=c.namedt(a); return uint256(x)+uint256(y); }
}
