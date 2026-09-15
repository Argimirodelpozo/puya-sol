// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

/// Memory-model cost benchmark: every input is a runtime value so nothing folds.
contract ScratchMemoryBench {
    struct P { uint256 x; uint64 y; bool b; }

    uint256[] stored;

    function sumParam(uint256[] memory a) external pure returns (uint256 s) {
        for (uint256 i = 0; i < a.length; i++) s += a[i];
    }

    function fillAndSum(uint256 n) external pure returns (uint256 s) {
        uint256[] memory a = new uint256[](n);
        for (uint256 i = 0; i < a.length; i++) a[i] = i * 2;
        for (uint256 i = 0; i < a.length; i++) s += a[i];
    }

    function bump(P memory p) internal pure {
        p.x += 1;
        p.y += 2;
        p.b = !p.b;
    }

    function structUpdate(uint256 x, uint64 y, uint256 rounds) external pure returns (uint256) {
        P memory p = P(x, y, false);
        for (uint256 i = 0; i < rounds; i++) bump(p);
        return p.x + p.y + (p.b ? 1 : 0);
    }

    function sort(uint256[] memory a) external pure returns (uint256[] memory) {
        for (uint256 i = 0; i < a.length; i++)
            for (uint256 j = 0; j + 1 < a.length - i; j++)
                if (a[j] > a[j + 1]) (a[j], a[j + 1]) = (a[j + 1], a[j]);
        return a;
    }

    function nestedRows(uint256 n) external pure returns (uint256 s) {
        uint256[][] memory m = new uint256[][](n);
        for (uint256 i = 0; i < n; i++) {
            m[i] = new uint256[](2);
            m[i][1] = i + 1;
        }
        for (uint256 i = 0; i < n; i++) s += m[i][1];
    }

    function storageRoundTrip(uint256 n) external returns (uint256 s) {
        for (uint256 i = 0; i < n; i++) stored.push(i);
        uint256[] memory m = stored;
        for (uint256 i = 0; i < m.length; i++) m[i] += 1;
        stored = m;
        for (uint256 i = 0; i < stored.length; i++) s += stored[i];
    }
}
