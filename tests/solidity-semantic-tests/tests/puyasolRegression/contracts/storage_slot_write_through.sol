// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// CUSTOM regression fixture (NOT vendored). Guards the OZ StorageSlot idiom.
//
// Solidity forbids assigning to a storage pointer, so OZ's StorageSlot library
// is the sanctioned way to write THROUGH one:
//     function getStringSlot(string storage store)
//         internal pure returns (StringSlot storage r)
//     { assembly { r.slot := store.slot } }
//     StorageSlot.getStringSlot(store).value = v;      // means: store = v
// The whole function is a pointer cast, and the wrapper struct has one field,
// so `f(x).value` IS `x` as an LVALUE. puya-sol resolved the call to a bare
// biguint slot number instead, `.value` on it was an unsupported member access
// returning an empty BytesConstant, and the write was dropped — caught by the
// assignment-target-constant guard. Blocked 7 real contracts via OZ
// ShortStrings (kaito, degen, usde, sdai, ena, aero, velo).
//
// Verified against a live solc+EVM (fuzz_state.py, 96 sequenced calls, 0
// divergences on state + events + revert payloads).
library SS {
    struct StringSlot { string value; }
    struct BytesSlot { bytes value; }
    function getStringSlot(string storage store) internal pure returns (StringSlot storage r) {
        assembly { r.slot := store.slot }
    }
    function getBytesSlot(bytes storage store) internal pure returns (BytesSlot storage r) {
        assembly { r.slot := store.slot }
    }
}

// The REAL OZ shape: the function taking `string storage` is a LIBRARY
// function (ShortStrings is a library), not a contract method.
library Fallback {
    function setVia(string storage store, string calldata v) internal {
        SS.getStringSlot(store).value = v;
    }
}

contract SlotWrite {
    string private a;
    bytes  private c;

    // direct: argument is a state variable
    function setA(string calldata v) external { SS.getStringSlot(a).value = v; }
    function setC(bytes calldata v) external { SS.getBytesSlot(c).value = v; }

    // NOTE: the contract-method equivalent of setDViaLib is a COMPILE ERROR
    // by design — contract methods get no storage write-back, so the store
    // would be dropped. See the guard in SolExpressionDispatch.
    // through a `string storage` PARAM of a LIBRARY function (the OZ shape)
    string private d;
    function setDViaLib(string calldata v) external { Fallback.setVia(d, v); }
    function getD() external view returns (string memory) { return d; }

    // read back through the same alias
    function readA() external view returns (string memory) {
        return SS.getStringSlot(a).value;
    }
    function getA() external view returns (string memory) { return a; }
    function getC() external view returns (bytes memory) { return c; }
    function lenA() external view returns (uint256) { return bytes(a).length; }
}
