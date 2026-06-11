// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// CUSTOM puya-sol test contract (NOT vendored from the upstream Solidity
// semantic suite) — guards the cross-contract selector/value-width bug. A call
// to a method with a non-uint64/256 integer parameter computed the WRONG ARC4
// method selector (and encoded the arg at the wrong width), so the call
// mis-routed/reverted. The callee names a param: <=64-bit -> "uint64",
// >64-bit -> "uintN" (sign dropped); a RETURN: signed -> "uint256", unsigned
// same as param. The caller must mirror that exactly.
contract Callee {
    function f(uint128 x) external pure returns (uint128) { return x + 1; }
    function g(int128 x) external pure returns (int128) { return x - 1; }
    function h(uint8 x) external pure returns (uint8) { return x + 1; }
    function k(uint256 x) external pure returns (uint256) { return x + 1; }
}
contract C {
    function callF(uint128 x) external returns (uint128) { return (new Callee()).f(x); }
    function callG(int128 x) external returns (int128) { return (new Callee()).g(x); }
    function callH(uint8 x) external returns (uint8) { return (new Callee()).h(x); }
    function callK(uint256 x) external returns (uint256) { return (new Callee()).k(x); }
}
