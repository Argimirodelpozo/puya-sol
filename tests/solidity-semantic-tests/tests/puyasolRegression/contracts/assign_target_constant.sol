// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// CUSTOM regression fixture (NOT vendored). MUST FAIL TO COMPILE.
//
// Guards the general rule that an assignment whose LHS lowers to a CONSTANT is
// a write that goes nowhere. Such targets come from lvalue paths that give up
// and return a placeholder (SolExpressionDispatch's "unsupported member access"
// warning yields an empty BytesConstant / zero IntegerConstant). puya happens
// to reject a constant target — it is not in its Lvalue union — but other
// shapes of the same mistake would SILENTLY DROP THE STORE, so puya-sol
// hard-errors on it after serialization instead.
//
// The shape below is OZ Checkpoints._unsafeAccess, which writes through a
// storage reference whose slot is computed by ARITHMETIC in assembly:
//     assembly { mstore(0, self.slot); r.slot := add(keccak256(0, 0x20), pos) }
// That denotes a DIFFERENT storage location than any parameter, so it is NOT a
// storage-pointer alias (contrast storage_slot_write_through.sol, where
// `r.slot := store.slot` IS an identity alias and now compiles). Supporting it
// needs real storage-pointer arithmetic; until then it must stay LOUD.
library Ckpt {
    struct Item { uint224 value; }

    function unsafeAccess(Item[] storage self, uint256 pos)
        internal pure returns (Item storage r)
    {
        assembly {
            mstore(0, self.slot)
            r.slot := add(keccak256(0, 0x20), pos)
        }
    }
}

contract AssignTargetConstant {
    Ckpt.Item[] private items;

    function push(uint224 v) external { items.push(Ckpt.Item(v)); }

    // the write that would be silently dropped
    function overwriteLast(uint224 v) external {
        Ckpt.unsafeAccess(items, items.length - 1).value = v;
    }
}
