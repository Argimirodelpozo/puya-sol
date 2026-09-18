// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

contract RecursiveShapeSlotHandle {
    struct Leaf {
        uint256 value;
        bool enabled;
    }

    function data() internal pure returns (Leaf[2][2][2] storage result) {
        assembly { result.slot := 300 }
    }

    function write(uint256 i, uint256 j, uint256 k,
                   uint256 value, bool enabled) external {
        Leaf[2][2][2] storage values = data();
        values[i][j][k].value = value;
        values[i][j][k].enabled = enabled;
    }

    function read(uint256 i, uint256 j, uint256 k)
        external view returns (uint256, bool)
    {
        Leaf[2][2][2] storage values = data();
        return (values[i][j][k].value, values[i][j][k].enabled);
    }

    function copied(uint256 i)
        external view returns (uint256, bool, uint256, bool)
    {
        Leaf[2][2][2] storage values = data();
        Leaf[2][2] memory copy = values[i];
        return (copy[0][0].value, copy[0][0].enabled,
                copy[1][1].value, copy[1][1].enabled);
    }
}

contract RecursiveShapeAsmArrayRoot {
    struct Holder {
        uint16[] small;
        uint256[][] nested;
    }

    uint16[] private small;
    uint256[][] private nested;
    Holder private holder;

    function resizeRoots(uint256 smallLength, uint256 nestedLength) external {
        assembly {
            sstore(small.slot, smallLength)
            sstore(nested.slot, nestedLength)
        }
    }

    function resizeMembers(uint256 smallLength, uint256 nestedLength) external {
        uint16[] storage smallRef = holder.small;
        uint256[][] storage nestedRef = holder.nested;
        assembly {
            sstore(smallRef.slot, smallLength)
            sstore(nestedRef.slot, nestedLength)
        }
    }

    function lengths()
        external view returns (uint256, uint256, uint256, uint256,
                               uint256, uint256)
    {
        return (small.length, nested.length,
                nested.length == 0 ? 99 : nested[0].length,
                holder.small.length, holder.nested.length,
                holder.nested.length == 0 ? 99 : holder.nested[0].length);
    }
}
