// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// CUSTOM regression fixture (NOT vendored). Guards FAIL-LOUD on a dropped write.
//
// The OZ StorageSlot idiom `getStringSlot(store).value = v` writes THROUGH a
// storage-slot handle produced by assembly. puya-sol models that handle as a
// bare biguint slot number, so `.value` hit SolExpressionDispatch's
// "unsupported member access" path — a WARNING returning an empty BytesConstant.
// As an assignment TARGET that means the write silently goes nowhere; here it
// happened to reach puya, which rejected it with the unreadable
// "deserialization failed: 'BytesConstant'" (a constant is not in its Lvalue
// union), but other shapes would simply drop the store.
//
// Assigning INTO a constant is never meaningful, so puya-sol now hard-errors on
// it after serialization, naming the source location. This fixture must keep
// FAILING TO COMPILE until writes through a storage-slot handle are supported;
// if it ever compiles, check that the write actually lands before relaxing this.
library SS {
    struct StringSlot { string value; }
    function getStringSlot(string storage store) internal pure returns (StringSlot storage r) {
        assembly { r.slot := store.slot }
    }
}

contract SlotW {
    string private _fallback;
    function setIt(string calldata v) external { SS.getStringSlot(_fallback).value = v; }
    function get() external view returns (string memory) { return _fallback; }
}
