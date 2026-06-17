// SPDX-License-Identifier: MIT
pragma abicoder v2;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// A library function returning a storage ref to a struct-with-mapping is modeled
// as a biguint slot handle (puya can't hold the mapping-bearing struct value).
// `.slot` on the bound local must read that handle; pre-fix it coerce-errored
// ("cannot coerce non-scalar type 'Items' to biguint in assembly arithmetic").
library Lib {
    struct Items {
        mapping(uint => uint) a;
    }

    function get() public returns (Items storage x) {
        assembly { x.slot := 123 }
    }
}

contract C {
    function f() public returns (uint256 slot) {
        Lib.Items storage ptr = Lib.get();
        assembly { slot := ptr.slot }
    }
}
