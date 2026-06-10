// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// CUSTOM puya-sol test contract (NOT vendored from the upstream Solidity
// semantic suite) — added by us to guard a compiler fix.
// Recursion via Yul-function -> subroutine lowering. Exercises (a) a MULTI-return
// recursive fn and (b) a single-return ACCUMULATOR recursive fn — both of which
// the old return-var-name reuse clobbered.
contract RecursiveMultiReturn {
    function f(uint256 n) external pure returns (uint256 s, uint256 c) {
        assembly {
            function rec(x) -> sum, cnt {
                sum := x
                cnt := 1
                if gt(x, 0) {
                    let psum, pcnt := rec(sub(x, 1))
                    sum := add(sum, psum)
                    cnt := add(cnt, pcnt)
                }
            }
            let rsum, rcnt := rec(n)
            s := rsum
            c := rcnt
        }
    }
    function f2(uint256 n) external pure returns (uint256 r) {
        assembly {
            function sumrec(x) -> acc {
                acc := x
                if gt(x, 0) { acc := add(acc, sumrec(sub(x, 1))) }
            }
            r := sumrec(n)
        }
    }
}
