// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// NOT an o.g. semantic test. Guards multiple-`_;` modifiers (the body runs once per placeholder).
// The body inliner splices the placeholder body per `_;`; before the fix it shared the same AWST
// nodes across the copies, so a checked-arithmetic body aliased its overflow-assert temps and
// miscompiled (AVM stuck / reverts flipped vs EVM). Fixed by deep-cloning the spliced body per `_;`.
contract G {
    uint256 acc;
    uint256 ctr;

    modifier twice() { _; _; }                 // body runs 2x
    modifier both() { ctr++; _; ctr++; }       // stacked outer

    // checked add, run twice -> acc += 2*v (and reverts iff EITHER add overflows)
    function addTwice(uint256 v) external twice { acc = acc + v; }
    // stacked: both(twice(body)) -> ctr += 2, acc += 2*v
    function addStacked(uint256 v) external both twice { acc = acc + v; }
    // value-returning under twice -> body returns twice, last wins
    function ret(uint256 v) external twice returns (uint256) { return v; }

    function getAcc() external view returns (uint256) { return acc; }
    function getCtr() external view returns (uint256) { return ctr; }
}
