// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract RecursiveShapeStorage {
    struct Wide {
        uint256 a;
        uint256 b;
    }

    // 513 * 64 = 32,832 bytes: just over one AVM box, so this exercises
    // element-aligned multi-box sizing without bloating the post-init group.
    Wide[513] private values;

    function write(uint256 index, uint256 a, uint256 b) external {
        values[index] = Wide(a, b);
    }

    function writeFields(uint256 index, uint256 a, uint256 b) external {
        values[index].a = a;
        values[index].b = b;
        values[index].a += 1;
    }

    function read(uint256 index) external view returns (uint256, uint256) {
        Wide storage value = values[index];
        return (value.a, value.b);
    }
}

contract RecursiveShapeEvm {
    struct Leaf {
        uint256 value;
        bool enabled;
    }

    struct Holder {
        Leaf leaf;
        uint256 tail;
    }

    struct Complex {
        uint256[] items;
        Holder holder;
    }

    uint256[][][] private values;
    uint256[][2][2] private mixed;
    uint256[][2][] private mixedTree;
    uint256[2][2][] private fixedLeaf;
    bool[3][2][] private packedLeaf;
    Holder[2] private holders;
    Complex[] private complexes;
    Holder public holder;

    function replace(uint256[][][] calldata next) external {
        values = next;
    }

    function setHolder(uint256 value, bool enabled, uint256 tail) external {
        holder = Holder(Leaf(value, enabled), tail);
    }

    function replaceMixed(uint256[][2][2] calldata next) external {
        mixed = next;
    }

    function replaceFixedLeaf(uint256[2][2][] calldata next) external {
        fixedLeaf = next;
    }

    function replaceMixedTree(uint256[][2][] calldata next) external {
        mixedTree = next;
    }

    function replacePackedLeaf(bool[3][2][] calldata next) external {
        packedLeaf = next;
    }

    function replaceHolders(Holder[2] calldata next) external {
        holders = next;
    }

    function pushComplex(uint256[] calldata items, uint256 value,
                         bool enabled, uint256 tail) external {
        complexes.push(Complex(items, Holder(Leaf(value, enabled), tail)));
    }

    function replaceComplex(Complex[] calldata next) external {
        complexes = next;
    }

    function popComplex() external {
        complexes.pop();
    }

    function mixedSummary()
        external view returns (uint256, uint256, uint256, uint256)
    {
        uint256[][2][2] memory copy = mixed;
        return (copy[0][0][0], copy[0][1][0],
                copy[1][0][0], copy[1][1][1]);
    }

    function fixedLeafSummary()
        external view returns (uint256, uint256, uint256, uint256, uint256)
    {
        uint256[2][2][] memory copy = fixedLeaf;
        return (copy.length, copy[0][0][0], copy[0][1][1],
                copy[1][0][1], copy[1][1][0]);
    }

    function mixedTreeSummary()
        external view returns (uint256, uint256, uint256, uint256, uint256)
    {
        uint256[][2][] memory copy = mixedTree;
        return (copy.length, copy[0][0][0], copy[0][1][1],
                copy[1][0][1], copy[1][1][0]);
    }

    function packedLeafSummary()
        external view returns (uint256, bool, bool, bool, bool, bool, bool)
    {
        bool[3][2][] memory copy = packedLeaf;
        return (copy.length,
                copy[0][0][0], copy[0][0][1], copy[0][0][2],
                copy[0][1][0], copy[0][1][1], copy[0][1][2]);
    }

    function holderSummary()
        external view returns (uint256, bool, uint256, uint256, bool, uint256)
    {
        Holder[2] memory copy = holders;
        return (copy[0].leaf.value, copy[0].leaf.enabled, copy[0].tail,
                copy[1].leaf.value, copy[1].leaf.enabled, copy[1].tail);
    }

    function complexSummary()
        external view returns (uint256, uint256, uint256, bool, uint256)
    {
        Complex storage value = complexes[complexes.length - 1];
        return (complexes.length, value.items.length, value.holder.leaf.value,
                value.holder.leaf.enabled, value.holder.tail);
    }

    function clearAggregates() external {
        delete values;
        delete mixed;
        delete mixedTree;
        delete fixedLeaf;
        delete packedLeaf;
        delete holders;
        delete complexes;
        delete holder;
    }

    function clearedSummary()
        external view
        returns (uint256, uint256, uint256, uint256,
                 uint256, uint256, uint256, uint256)
    {
        return (values.length, mixed[0][0].length, mixed[1][1].length,
                mixedTree.length, fixedLeaf.length, packedLeaf.length,
                complexes.length, holder.leaf.value);
    }

    function summary()
        external view
        returns (uint256 outer, uint256 middle, uint256 inner,
                 uint256 a, uint256 b, uint256 tail)
    {
        uint256[][][] memory copy = values;
        return (copy.length, copy[0].length, copy[0][0].length,
                copy[0][0][0], copy[0][0][1], copy[1][0][0]);
    }
}

contract RecursiveShapeType {
    struct Node {
        uint256 value;
        Node[][] children;
    }

    Node private root;

    function setRoot(uint256 value) external {
        root.value = value;
    }

    function getRoot() external view returns (uint256) {
        return root.value;
    }
}

contract RecursiveShapeMapping {
    struct Bucket {
        mapping(uint256 => uint256) values;
        uint256 marker;
    }

    Bucket[][] public rows;

    function seed() external {
        if (rows.length == 0) {
            rows.push();
            rows[0].push();
            rows[0].push();
        }
    }

    function write(uint256 outer, uint256 inner, uint256 key, uint256 value)
        external
    {
        rows[outer][inner].values[key] = value;
    }

    function setMarker(uint256 outer, uint256 inner, uint256 value) external {
        rows[outer][inner].marker = value;
    }

    function read(uint256 outer, uint256 inner, uint256 key)
        external view returns (uint256)
    {
        return rows[outer][inner].values[key];
    }
}

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

contract RecursiveShapeBoxRef {
    struct Leaf { uint256 value; }
    struct Inner { uint256 value; bool enabled; }
    struct Holder { Inner inner; }

    Leaf[][] private values;
    Holder[][] private holders;
    uint256[] private scalarValues;
    uint256[][2] private mixedValues;

    function seed() external {
        if (values.length == 0) {
            values.push();
            values[0].push(Leaf(0));
        }
        if (holders.length == 0) {
            holders.push();
            holders[0].push(Holder(Inner(0, false)));
        }
        if (scalarValues.length == 0) scalarValues.push(1);
        if (mixedValues[0].length == 0) mixedValues[0].push(2);
        if (mixedValues[1].length == 0) mixedValues[1].push(3);
    }

    function bump(Leaf[][] storage ref) internal {
        ref[0][0].value = 707;
    }

    function setLeaf(Leaf storage ref) internal {
        ref.value = 808;
    }

    function setHolder(Holder storage ref) internal {
        ref.inner.value = 909;
        ref.inner.value += 1;
        ref.inner.enabled = true;
    }

    function setScalars(uint256[] storage ref) internal returns (uint256) {
        ref[0] = 515;
        ref[0] += 5;
        ref.push(616);
        return ref.length;
    }

    function setMixed(uint256[][2] storage ref) internal {
        ref[0][0] = 717;
        ref[1][0] = 818;
    }

    function run() external returns (uint256) {
        bump(values);
        return values[0][0].value;
    }

    function runElementRef() external returns (uint256) {
        setLeaf(values[0][0]);
        return values[0][0].value;
    }

    function runNestedRef() external returns (uint256, bool) {
        setHolder(holders[0][0]);
        return (holders[0][0].inner.value, holders[0][0].inner.enabled);
    }

    function runScalarRef() external returns (uint256, uint256, uint256) {
        uint256 length = setScalars(scalarValues);
        return (length, scalarValues[0], scalarValues[1]);
    }

    function runMixedRef() external returns (uint256, uint256) {
        setMixed(mixedValues);
        return (mixedValues[0][0], mixedValues[1][0]);
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
