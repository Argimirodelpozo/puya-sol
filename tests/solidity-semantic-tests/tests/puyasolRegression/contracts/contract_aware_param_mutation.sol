// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Parameter-mutation summaries must use the concrete contract's virtual target,
// preserve super's lexical lookup, and reach a least fixed point for recursion.
contract MutationBase {
    struct S { uint256 x; }

    function hook(S memory) internal pure virtual {}

    function route(S memory s) internal pure {
        hook(s);
    }

    function runVirtual() external pure returns (uint256) {
        S memory s = S(5);
        route(s);
        return s.x;
    }
}

contract MutationDerived is MutationBase {
    function hook(S memory s) internal pure override {
        s.x = 77;
    }
}

contract SuperMiddle is MutationBase {
    function callSuper(S memory s) internal pure {
        super.hook(s);
    }
}

contract SuperLeaf is SuperMiddle {
    function hook(S memory s) internal pure override {
        s.x = 99;
    }

    function runSuper() external pure returns (uint256) {
        S memory s = S(5);
        callSuper(s);
        return s.x; // lexical super target is MutationBase.hook: read-only
    }
}

contract RecursiveMutation {
    struct S { uint256 x; }

    function readA(S memory a, S memory b, uint256 n)
        internal pure returns (uint256)
    {
        if (n == 0) return a.x + b.x;
        return readB(a, b, n - 1);
    }

    function readB(S memory a, S memory b, uint256 n)
        internal pure returns (uint256)
    {
        if (n == 0) return a.x + b.x;
        return readA(a, b, n - 1);
    }

    function mutateA(S memory s, uint256 n) internal pure {
        if (n == 0) {
            s.x = 51;
            return;
        }
        mutateB(s, n - 1);
    }

    function mutateB(S memory s, uint256 n) internal pure {
        mutateA(s, n);
    }

    function runReadOnlyCycle() external pure returns (uint256) {
        S memory s = S(4);
        // The same mutable value is valid in two read-only positions. The old
        // recursion fallback marked both mutated and made puya reject this alias.
        return readA(s, s, 2);
    }

    function runMutatingCycle() external pure returns (uint256) {
        S memory s = S(1);
        mutateA(s, 2);
        return s.x;
    }
}
