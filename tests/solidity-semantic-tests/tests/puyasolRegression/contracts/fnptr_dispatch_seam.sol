// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the function-pointer dispatch seam (fable-review-3 H14):
// (F4) dispatchName collapsed signedness (int8/uint8 both "_u8") and every
//      non-int type to "_x" — distinct pointer signatures merged into ONE
//      dispatch group typed by whichever signature registered first;
// (F3) dispatch-subroutine DEFINITION types (address→biguint, enum→biguint,
//      multi-return→void TODO) disagreed with the call-site types
//      (account/uint64/WTuple) — multi-return results were silently dropped;
// (F2) external fn-ptr args used a drifted private encoder — negative
//      sub-256 signed args zero-extended to 32 bytes where the callee decodes
//      the declared width (revert), aggregates skipped ARC4.
contract FnPtrTarget {
    function g(int128 x) external pure returns (int128) {
        return x - 1;
    }
}

contract FnPtrSeam {
    enum E { A, B, C }

    function fu8a(uint8 x) internal pure returns (uint256) { return 100 + x; }
    function fu8b(uint8 x) internal pure returns (uint256) { return 200 + x; }
    function fi8a(int8 x) internal pure returns (uint256) { return x < 0 ? 1000 : 2000; }
    function fi8b(int8 x) internal pure returns (uint256) { return x < 0 ? 3000 : 4000; }
    function fb32(bytes32 b) internal pure returns (uint256) { return uint256(b) & 0xff; }
    function fstr(string memory s) internal pure returns (uint256) { return bytes(s).length; }
    function fa1(address a) internal pure returns (uint256) { return a == address(0) ? 7 : 8; }
    function fa2(address a) internal pure returns (uint256) { return a == address(0) ? 70 : 80; }
    function fe1(E e) internal pure returns (uint256) { return 500 + uint256(e); }
    function fe2(E e) internal pure returns (uint256) { return 600 + uint256(e); }
    function pairA() internal pure returns (uint256, uint256) { return (11, 22); }
    function pairB() internal pure returns (uint256, uint256) { return (33, 44); }

    // F4: uint8 vs int8 signatures must dispatch through SEPARATE groups.
    function pick8(bool c, bool useUnsigned, uint8 u, int8 i) external pure returns (uint256) {
        if (useUnsigned) {
            function(uint8) internal pure returns (uint256) p = c ? fu8a : fu8b;
            return p(u);
        }
        function(int8) internal pure returns (uint256) q = c ? fi8a : fi8b;
        return q(i);
    }

    // F4: bytes32 vs string signatures (both collapsed to "_x" pre-fix).
    function pickX(bool useB32) external pure returns (uint256) {
        if (useB32) {
            function(bytes32) internal pure returns (uint256) p = fb32;
            return p(bytes32(uint256(0x2a)));
        }
        function(string memory) internal pure returns (uint256) q = fstr;
        return q("hey");
    }

    // F3: address param (def was biguint vs call-site account).
    function pickAddr(bool c, address a) external pure returns (uint256) {
        function(address) internal pure returns (uint256) p = c ? fa1 : fa2;
        return p(a);
    }

    // F3: enum param (def was biguint vs call-site uint64).
    function pickEnum(bool c, E e) external pure returns (uint256) {
        function(E) internal pure returns (uint256) p = c ? fe1 : fe2;
        return p(e);
    }

    // F3: multi-return (def was void — results dropped).
    function pickPair(bool c) external pure returns (uint256 x, uint256 y) {
        function() internal pure returns (uint256, uint256) p = c ? pairA : pairB;
        (x, y) = p();
    }

    // F2: external fn-ptr with a signed narrow arg.
    function callExt(address target, int128 v) external returns (int128) {
        function(int128) external pure returns (int128) p = FnPtrTarget(target).g;
        return p(v);
    }
}
