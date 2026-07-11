// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Isolation repro for the data-location divergences the battery flagged. Each fn returns 5
// (or 11) on a correct EVM; the AVM value pinpoints which specific pattern diverges.
contract ReproLoc {
    struct S { uint256 x; uint256 y; }
    S[] arr;
    S single;
    mapping(uint256 => S) m;

    // ---- storage mutation: which references write through? (all EVM=5) ----
    function direct() external returns (uint256) {            // no param, direct element write
        delete arr; arr.push(S(0, 0)); arr[0].x = 5; return arr[0].x;
    }
    function localStorageRef() external returns (uint256) {   // S storage s = arr[0]
        delete arr; arr.push(S(0, 0)); S storage s = arr[0]; s.x = 5; return arr[0].x;
    }
    function structVarParam() external returns (uint256) {    // pass a storage struct var
        single.x = 0; _bs(single); return single.x;
    }
    function arrayElemParam() external returns (uint256) {    // pass arr[0] (storage struct ref)
        delete arr; arr.push(S(0, 0)); _bs(arr[0]); return arr[0].x;
    }
    function mapValParam() external returns (uint256) {       // pass m[1] (storage struct ref)
        m[1].x = 0; _bs(m[1]); return m[1].x;
    }
    function arrayParam() external returns (uint256) {        // pass the whole storage array
        delete arr; arr.push(S(0, 0)); _ba(arr); return arr[0].x;
    }
    function _bs(S storage s) internal { s.x = 5; }
    function _ba(S[] storage a) internal { a[0].x = 5; }

    // ---- memory aliasing: array vs struct (both EVM=11) ----
    function memArrAlias() external pure returns (uint256) {
        uint256[] memory a = new uint256[](1); a[0] = 5;
        uint256[] memory b = a; b[0] = 11; return a[0];
    }
    function memStructAlias() external pure returns (uint256) {
        S memory a = S(5, 0); S memory b = a; b.x = 11; return a.x;
    }
}
