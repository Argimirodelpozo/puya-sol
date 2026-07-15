// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Guard: the "does this function use inline assembly" gates scanned only the
// function body's TOP-LEVEL statements, so asm nested in `unchecked {}` (or a
// plain block) flipped them: the ARC4 param remap ran, the asm read `i` as
// arc4.uint256, and its switch compared that against biguint constants — a
// never-equal match, silently taking the default branch (wrong slot 0).
contract C {
    mapping(uint => uint) private m0;
    mapping(uint => uint) private m1;
    mapping(uint => uint) private m2;

    // the fuzzer's shape (slot_access_via_mapping_pointer, unchecked-wrap)
    function viaUnchecked(uint i) public returns (uint slot) { unchecked {
        mapping(uint => uint) storage m0Ptr = m0;
        mapping(uint => uint) storage m1Ptr = m1;
        mapping(uint => uint) storage m2Ptr = m2;
        assembly {
            switch i
            case 1 { slot := m1Ptr.slot }
            case 2 { slot := m2Ptr.slot }
            default { slot := m0Ptr.slot }
        }
    } }

    // plain nested block, decls at top level
    function viaNestedBlock(uint i) public returns (uint slot) {
        mapping(uint => uint) storage m0Ptr = m0;
        mapping(uint => uint) storage m1Ptr = m1;
        {
            assembly {
                switch i
                case 1 { slot := m1Ptr.slot }
                default { slot := m0Ptr.slot }
            }
        }
    }
}
