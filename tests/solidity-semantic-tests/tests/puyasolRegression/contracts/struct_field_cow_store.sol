// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// One shared struct-field COW store (AssignmentHelper::buildStructFieldCowStore)
// backs `= v`, `op= v`, `++/--` and `delete` on struct fields. The per-site
// copies drifted: inc/dec skipped the lazy-root-box ensure (n[k][i].f++ on a
// FRESH mapping key died "no such box" where `= v` worked), and delete's copy
// kept StateGet inside the index target (puya: "unsupported assignment
// target" — `delete n[k][1].f` did not even compile).
contract StructFieldCowStore {
    struct S { uint64 f; uint64 g; }
    mapping(uint256 => S[3]) n;
    mapping(uint256 => S) m;

    function setNested(uint256 k) external returns (uint256) { n[k][1].f = 1; return n[k][1].f; }
    function incNested(uint256 k) external returns (uint256) { n[k][1].f++; return n[k][1].f; }
    function preNested(uint256 k) external returns (uint256) { return ++n[k][2].g; }
    function incFlat(uint256 k) external returns (uint256) { m[k].f++; return m[k].f; }
    function delNestedFresh(uint256 k) external returns (uint256) { delete n[k][1].f; return n[k][1].f; }
    function delNestedSet(uint256 k) external returns (uint256) {
        n[k][1].f = 9;
        n[k][1].g = 5;
        delete n[k][1].f;
        return n[k][1].f * 100 + n[k][1].g; // g survives: 5
    }
}
