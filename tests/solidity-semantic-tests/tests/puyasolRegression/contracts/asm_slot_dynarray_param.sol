// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// OZ Checkpoints._unsafeAccess shape: asm .slot on a DYNAMIC-ARRAY storage
// param + keccak data-slot arithmetic + result.slot rebind.
contract Ckpt {
    struct Checkpoint208 { uint48 _key; uint208 _value; }
    Checkpoint208[] private _ckpts;

    function push(uint48 k, uint208 v) external {
        _ckpts.push(Checkpoint208(k, v));
    }

    function _unsafeAccess(Checkpoint208[] storage self, uint256 pos)
        private pure returns (Checkpoint208 storage result)
    {
        assembly {
            mstore(0x00, self.slot)
            result.slot := add(keccak256(0x00, 0x20), pos)
        }
    }

    function readUnsafe(uint256 pos) external view returns (uint48, uint208) {
        Checkpoint208 storage cp = _unsafeAccess(_ckpts, pos);
        return (cp._key, cp._value);
    }

    function len() external view returns (uint256) { return _ckpts.length; }
}
