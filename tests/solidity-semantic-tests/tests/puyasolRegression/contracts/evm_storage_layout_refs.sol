// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// OZ StorageSlot shape: storage-ref params through LIBRARY functions,
// write-through via asm `r.slot := store.slot`.
library SS {
    struct StringSlot { string value; }
    struct Uint256Slot { uint256 value; }
    function getStringSlot(string storage store) internal pure returns (StringSlot storage r) {
        assembly { r.slot := store.slot }
    }
    function getUint256Slot(bytes32 slot) internal pure returns (Uint256Slot storage r) {
        assembly { r.slot := slot }
    }
}

// OZ Checkpoints shape: dynamic-array storage param + unsafe keccak access.
library Checkpoints {
    struct Checkpoint { uint32 key; uint224 value; }
    function push(Checkpoint[] storage self, uint32 key, uint224 value) internal {
        self.push(Checkpoint(key, value));
    }
    function latest(Checkpoint[] storage self) internal view returns (uint224) {
        uint256 pos = self.length;
        return pos == 0 ? 0 : _unsafeAccess(self, pos - 1).value;
    }
    function _unsafeAccess(Checkpoint[] storage self, uint256 pos)
        internal pure returns (Checkpoint storage result)
    {
        assembly {
            mstore(0x00, self.slot)
            result.slot := add(keccak256(0x00, 0x20), pos)
        }
    }
}

library Lib {
    function bump(uint256[] storage arr, uint256 i, uint256 by) internal {
        arr[i] += by;
    }
    function total(uint256[] storage arr) internal view returns (uint256 t) {
        for (uint256 i = 0; i < arr.length; i++) t += arr[i];
    }
    function setVia(string storage store, string calldata v) internal {
        SS.getStringSlot(store).value = v;
    }
}

contract EvmRefs {
    using Checkpoints for Checkpoints.Checkpoint[];

    string internal a;                       // slot 0
    uint256[] internal nums;                 // slot 1
    Checkpoints.Checkpoint[] internal ckpts; // slot 2
    struct Pos { uint128 x; uint128 y; }
    mapping(uint256 => Pos) internal poss;   // slot 3
    uint256 internal tail;                   // slot 4

    // ── StorageSlot write-through ──
    function setA(string calldata v) external { SS.getStringSlot(a).value = v; }
    function getA() external view returns (string memory) { return a; }
    function readA() external view returns (string memory) { return SS.getStringSlot(a).value; }
    function lenA() external view returns (uint256) { return bytes(a).length; }
    function setDViaLib(string calldata v) external { Lib.setVia(a, v); }
    function setTailViaSlot(uint256 v) external {
        SS.getUint256Slot(bytes32(uint256(4))).value = v;
    }
    function getTail() external view returns (uint256) { return tail; }

    // ── array storage params through a library ──
    function pushNum(uint256 v) external { nums.push(v); }
    function bumpNum(uint256 i, uint256 by) external { Lib.bump(nums, i, by); }
    function numAt(uint256 i) external view returns (uint256) { return nums[i]; }
    function totalNums() external view returns (uint256) { return Lib.total(nums); }

    // ── Checkpoints (using-for receiver) ──
    function ck(uint32 key, uint224 value) external { ckpts.push(key, value); }
    function ckLatest() external view returns (uint224) { return ckpts.latest(); }
    function ckLen() external view returns (uint256) { return ckpts.length; }

    // ── storage locals: bind, use, rebind ──
    function viaLocal(uint256 k, uint128 x, uint128 y) external {
        Pos storage p = poss[k];
        p.x = x;
        p.y = y;
    }
    function getPos(uint256 k) external view returns (uint128, uint128) {
        Pos storage p = poss[k];
        return (p.x, p.y);
    }
    function rebind(uint256 k1, uint256 k2, uint128 v) external {
        Pos storage p = poss[k1];
        p.x = v;
        p = poss[k2];
        p.x = v + 1;
    }
}
