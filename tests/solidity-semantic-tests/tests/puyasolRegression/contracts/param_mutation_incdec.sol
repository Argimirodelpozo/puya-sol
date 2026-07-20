// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards ParamMutationDetector coverage: a callee mutating a memory ref param
// ONLY via ++/--/delete (no plain assignment) was classified non-mutating, so
// the caller-side write-back was skipped and the mutation silently vanished.
contract ParamMutationIncDec {
    function inc(uint256[] memory a) internal pure {
        a[0]++;
    }

    function dec(uint256[] memory a) internal pure {
        --a[1];
    }

    function del(uint256[] memory a) internal pure {
        delete a[2];
    }

    function run() external pure returns (uint256, uint256, uint256) {
        uint256[] memory arr = new uint256[](3);
        arr[1] = 10;
        arr[2] = 77;
        inc(arr);   // arr[0]: 0 -> 1
        dec(arr);   // arr[1]: 10 -> 9
        del(arr);   // arr[2]: 77 -> 0
        return (arr[0], arr[1], arr[2]);
    }

    struct S {
        uint256 n;
    }

    function bump(S memory s) internal pure {
        s.n++;
    }

    function runStruct() external pure returns (uint256) {
        S memory s = S(41);
        bump(s);
        return s.n;
    }
}
