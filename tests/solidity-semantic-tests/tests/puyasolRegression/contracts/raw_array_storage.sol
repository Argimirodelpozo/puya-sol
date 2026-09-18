// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

contract RawArrayStorage {
    struct State { uint256 guard; uint256[] values; }
    uint256[] values;
    State state;
    uint8[] packed;
    uint256[][] nested;
    mapping(uint256 => uint256[]) mapped;

    function rootRoundtrip() external returns (uint256) {
        delete values;
        values.push(7);
        values.push(8);
        assembly { sstore(values.slot, 1) sstore(values.slot, 2) }
        return values[1];
    }

    function memberRoundtrip() external returns (uint256, uint256, uint256) {
        delete state;
        state.guard = 9;
        state.values.push(7);
        state.values.push(8);
        uint256[] storage member = state.values;
        assembly { sstore(member.slot, 1) }
        uint256 prefix = member[0];
        assembly { sstore(member.slot, 2) }
        return (state.guard, prefix, member[1]);
    }

    function hiddenTail() external returns (uint256 raw, uint256 restored) {
        delete values;
        values.push(7);
        assembly {
            mstore(0, values.slot)
            let tail := add(keccak256(0, 32), 1)
            sstore(tail, 29)
            raw := sload(tail)
            sstore(values.slot, 2)
        }
        restored = values[1];
    }

    function clearTail(bool whole) external returns (uint256) {
        delete values;
        values.push(7);
        values.push(8);
        if (whole) delete values;
        else values.pop();
        assembly { sstore(values.slot, 2) }
        return values[1];
    }

    function packedRoundtrip() external returns (uint256) {
        delete packed;
        packed.push(7);
        packed.push(8);
        packed.push(9);
        assembly { sstore(packed.slot, 1) sstore(packed.slot, 3) }
        return packed[2];
    }

    function nestedRoundtrip() external returns (uint256) {
        delete nested;
        nested.push().push(7);
        nested.push().push(18);
        assembly { sstore(nested.slot, 1) sstore(nested.slot, 2) }
        return nested[1][0];
    }

    function mappingRoundtrip() external returns (uint256) {
        delete mapped[3];
        uint256[] storage member = mapped[3];
        member.push(7);
        member.push(19);
        assembly { sstore(member.slot, 1) sstore(member.slot, 2) }
        return member[1];
    }

    function literalRoot() external returns (uint256) {
        delete values;
        values.push(7);
        values.push(8);
        assembly { sstore(0, 1) sstore(0, 2) }
        return values[1];
    }

    function runtimeRoot(uint256 slot) external returns (uint256) {
        delete values;
        values.push(7);
        values.push(8);
        assembly { sstore(slot, 1) sstore(slot, 2) }
        return values[1];
    }
}
