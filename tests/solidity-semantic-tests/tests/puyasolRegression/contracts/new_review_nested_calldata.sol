// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

// Synthetic calldata for dynamic-element arrays must use
// EVM head/tail offsets, not ARC-4's uint16 offset table.
contract NewReviewNestedCalldata {
    struct Bundle {
        uint256 tag;
        bytes data;
        uint256[] nums;
    }

    function inspectHeader(bytes[] calldata values)
        external pure
        returns (uint256 head, uint256 count, uint256 offset0, uint256 offset1,
                 uint256 size)
    {
        assembly {
            head := values.offset
            count := values.length
            let heads := add(values.offset, sub(values.length, values.length))
            offset0 := calldataload(heads)
            offset1 := calldataload(add(heads, 32))
            size := calldatasize()
        }
        values;
    }

    function inspectBytes(bytes[] calldata values)
        external pure
        returns (uint256 count, uint256 len0, bytes32 word0,
                 uint256 len1, bytes32 word1)
    {
        assembly {
            count := values.length
            let heads := add(values.offset, sub(values.length, values.length))
            let elem0 := add(heads, calldataload(heads))
            let elem1 := add(heads, calldataload(add(heads, 32)))
            len0 := calldataload(elem0)
            word0 := calldataload(add(elem0, 32))
            len1 := calldataload(elem1)
            word1 := calldataload(add(elem1, 32))
        }
    }

    function inspectNested(uint256[][] calldata values)
        external pure
        returns (uint256 count, uint256 len0, uint256 a, uint256 b,
                 uint256 len1, uint256 c)
    {
        assembly {
            count := values.length
            let heads := add(values.offset, sub(values.length, values.length))
            let elem0 := add(heads, calldataload(heads))
            let elem1 := add(heads, calldataload(add(heads, 32)))
            len0 := calldataload(elem0)
            a := calldataload(add(elem0, 32))
            b := calldataload(add(elem0, 64))
            len1 := calldataload(elem1)
            c := calldataload(add(elem1, 32))
        }
    }

    function inspectTriple(uint256[][][] calldata values)
        external pure
        returns (uint256 outerCount, uint256 middleCount, uint256 innerCount,
                 uint256 a, uint256 b)
    {
        assembly {
            outerCount := values.length
            let outerHeads := add(values.offset, sub(values.length, values.length))
            let middle := add(outerHeads, calldataload(outerHeads))
            middleCount := calldataload(middle)
            let middleHeads := add(middle, 32)
            let inner := add(middleHeads, calldataload(middleHeads))
            innerCount := calldataload(inner)
            a := calldataload(add(inner, 32))
            b := calldataload(add(inner, 64))
        }
    }

    function inspectFixedDynamic(uint256[][2] calldata values)
        external pure
        returns (uint256 head, uint256 count, uint256 offset0, uint256 offset1,
                 uint256 a, uint256 b)
    {
        assembly {
            // solc exposes neither `.offset` nor `.length` for a fixed calldata
            // array. It is still dynamically ABI-encoded because its elements
            // are dynamic, so recover the single parameter's tail from the ABI
            // head exactly as an external decoder does.
            let heads := add(4, calldataload(4))
            head := heads
            count := 2
            offset0 := calldataload(heads)
            offset1 := calldataload(add(heads, 32))
            let elem0 := add(heads, calldataload(heads))
            let elem1 := add(heads, calldataload(add(heads, 32)))
            a := calldataload(add(elem0, 32))
            b := calldataload(add(elem1, 32))
        }
    }

    function inspectStruct(Bundle calldata value)
        external pure
        returns (uint256 head, uint256 tag, uint256 dataLen,
                 uint256 numsLen, uint256 first)
    {
        assembly {
            // Struct calldata pointers likewise have no Yul `.offset` suffix.
            // This one-parameter function's struct tail is selected by word 4.
            let base := add(4, calldataload(4))
            head := base
            tag := calldataload(base)
            let dataTail := add(base, calldataload(add(base, 32)))
            dataLen := calldataload(dataTail)
            let numsTail := add(base, calldataload(add(base, 64)))
            numsLen := calldataload(numsTail)
            first := calldataload(add(numsTail, 32))
        }
    }
}
