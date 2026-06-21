// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// puyasolRegression — NOT an o.g. semantic test. Found by the generative fuzzer (Yul user functions).
// A Yul user-defined function is INLINE-EXPANDED (UserFunctionOps.cpp, non-recursive path ~line 116-191)
// by binding its params/returns to their BARE names (`x`, `y`) in m_locals + emitting `x = arg`. So two
// functions that share param/return names — or nested/repeated calls — CLOBBER the same runtime vars.
// Here sq(x)->y and cube(x)->y both use x/y, and cube calls sq, so `add(sq(a), cube(b))` collapses: the
// AVM computes 2*a^3 regardless of b (every call resolves to cube(a)) instead of a^2 + b^3.
// FIXED: each inline expansion gets UNIQUE local names (x -> __yul_<uid>_x) via a scoped rename map
// (m_yulInlineRenames, applied in resolveVarRef), and publishes the unique return temp in
// m_yulSubReturnTemps + returns it as the expression value so the caller reads the right var — mirrors
// what the SUBROUTINE path (~line 56-100) already does with __yulret_<id> temps. Saved/restored per frame.
contract YulUserFnVarClash {
    function uf(uint256 a, uint256 b) external pure returns (uint256 r) {
        assembly {
            function sq(x) -> y { y := mul(x, x) }
            function cube(x) -> y { y := mul(sq(x), x) }
            r := add(sq(a), cube(b))   // a^2 + b^3
        }
    }
}
