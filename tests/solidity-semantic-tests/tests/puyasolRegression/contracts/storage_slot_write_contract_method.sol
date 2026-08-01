// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// CUSTOM fixture. MUST FAIL TO COMPILE — see
// test_storage_slot_write_through_contract_method_fails_loud.
//
// Same StorageSlot write-through as storage_slot_write_through.sol, but the
// helper taking `string storage` is a CONTRACT METHOD rather than a library
// function. Only library/free functions get the storage-param write-back
// augmentation, so this store would reach a local copy and vanish. It must stay
// a loud error until contract methods gain write-back.
library SS {
    struct StringSlot { string value; }
    function getStringSlot(string storage store) internal pure returns (StringSlot storage r) {
        assembly { r.slot := store.slot }
    }
}
contract SlotWriteContractMethod {
    string private b;
    function _viaParam(string storage store, string calldata v) internal {
        SS.getStringSlot(store).value = v;
    }
    function setB(string calldata v) external { _viaParam(b, v); }
    function getB() external view returns (string memory) { return b; }
}
