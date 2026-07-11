// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// D2 characterization matrix: every return-shape class ReturnRewriter's 6 passes
// discriminate. The per-method wire return types dumped from awst.json are the
// ORACLE for the compute-wire-type-once redesign: identical oracle = safe.
contract M {
    uint256 s;
    modifier m() { s += 1; _; }
    // singles, unsigned (pass 2 widths + pass 5 masks)
    function ru8(uint256 a) public pure returns (uint8) { return uint8(a); }
    function ru32(uint256 a) public pure returns (uint32) { return uint32(a); }
    function ru64(uint256 a) public pure returns (uint64) { return uint64(a); }
    function ru128(uint256 a) public pure returns (uint128) { return uint128(a); }
    function ru256(uint256 a) public pure returns (uint256) { return a; }
    // singles, signed (pass 4 single)
    function ri8(int256 a) public pure returns (int8) { return int8(a); }
    function ri64(int256 a) public pure returns (int64) { return int64(a); }
    function ri128(int256 a) public pure returns (int128) { return int128(a); }
    function ri256(int256 a) public pure returns (int256) { return a; }
    // non-int singles
    function rb(uint256 a) public pure returns (bool) { return a % 2 == 0; }
    function ra(uint256 a) public pure returns (address) { return address(uint160(a)); }
    function rb4(uint256 a) public pure returns (bytes4) { return bytes4(uint32(a)); }
    function rdyn(uint256 a) public pure returns (bytes memory) { bytes memory b = new bytes(2); b[0] = bytes1(uint8(a)); return b; }
    function rstr() public pure returns (string memory) { return "x"; }
    function rarr(uint256 a) public pure returns (uint32[] memory) { uint32[] memory r = new uint32[](2); r[0] = uint32(a); return r; }  // pass 1
    // tuples (pass 3 static; pass 4 signed-containing)
    function t_uu(uint256 a) public pure returns (uint64, uint128) { return (uint64(a), uint128(a)); }
    function t_su(int256 a) public pure returns (int64, uint128) { return (int64(a), uint128(uint256(a))); }
    function t_bb(uint256 a) public pure returns (bytes4, uint128) { return (bytes4(uint32(a)), uint128(a)); }
    function t_bools(uint256 a) public pure returns (bool, bool, uint256) { return (a % 2 == 0, a % 3 == 0, a); }
    function t_dyn(uint256 a) public pure returns (uint64, bytes memory) { bytes memory b = new bytes(1); b[0] = bytes1(uint8(a)); return (uint64(a), b); }
    function t_dyn_big(uint256 a) public pure returns (uint128, bytes memory) { bytes memory b = new bytes(1); b[0] = bytes1(uint8(a)); return (uint128(a), b); }  // residual (a): biguint elem in a DYNAMIC tuple
    function t_dyn_sig(int256 a) public pure returns (int128, bytes memory) { bytes memory b = new bytes(1); b[0] = bytes1(uint8(uint256(a))); return (int128(a), b); }  // signed elem in a dynamic tuple
    function md_dyn(uint256 a) public m returns (uint128, bytes memory) { bytes memory b = new bytes(1); b[0] = bytes1(uint8(a)); return (uint128(a), b); }  // residual (a)+(b): modifier + dynamic + biguint
    // opaque + ternary return values (pass 3/4 non-literal branches)
    function mk() internal pure returns (uint64, uint128) { return (1, 2); }
    function t_opaque() public pure returns (uint64, uint128) { return mk(); }
    function t_tern(uint256 a) public pure returns (uint64, uint128) { return a % 2 == 0 ? (uint64(1), uint128(2)) : (uint64(3), uint128(4)); }
    function s_tern(int256 a) public pure returns (int128) { return a > 0 ? int128(1) : int128(-1); }
    // modifier'd (chain path; passes 2/3 skip) + named returns
    function md_u128(uint256 a) public m returns (uint128 r) { r = uint128(a); }
    function md_i64(int256 a) public m returns (int64 r) { r = int64(a); }
    function md_tuple(uint256 a) public m returns (uint64 x, uint128 y) { x = uint64(a); y = uint128(a); }
    // asm-bodied biguint return (pass 2 wrap-mod)
    function asm_u256(uint256 a) public pure returns (uint256 r) { assembly { r := add(a, 1) } }
    function asm_u128(uint256 a) public pure returns (uint128 r) { assembly { r := add(a, 1) } }
}
