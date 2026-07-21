// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards arc4 struct DEFAULT bool packing (fable-review-3 M14). puya's reader
// packs consecutive arc4.bool fields 8-per-byte; the default encoder gave each
// bool its own head byte, so a defaulted mapping value (>=2 leading bools + a
// dynamic field) had head offsets that disagreed with the reader. A
// read-modify-write of a never-written entry then spliced at the wrong spot.
contract Arc4BoolDefaultPacking {
    struct S {
        bool a;
        bool b;
        bool c;
        uint256[] arr; // dynamic field → the head carries a tail offset
    }

    mapping(uint256 => S) m;

    // Touch a never-written entry (its value is the default encoding), then
    // read a field: layout must agree with the reader.
    function readDefaults(uint256 k) external view returns (bool, bool, bool, uint256) {
        S storage s = m[k];
        return (s.a, s.b, s.c, s.arr.length);
    }

    // Read-modify-write starting from the default: set a bool and push to the
    // dynamic field, then read everything back.
    function modifyFromDefault(uint256 k) external returns (bool, bool, bool, uint256) {
        S storage s = m[k];
        s.b = true;
        s.arr.push(42);
        return (s.a, s.b, s.c, s.arr.length);
    }

    function arrAt(uint256 k, uint256 i) external view returns (uint256) {
        return m[k].arr[i];
    }
}
