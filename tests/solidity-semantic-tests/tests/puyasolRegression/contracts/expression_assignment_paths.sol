// SPDX-License-Identifier: MIT
pragma solidity ^0.8.34;

contract ExpressionAssignmentPaths {
    struct Item { uint256 a; uint256 b; }
    struct Root { uint256[2] values; uint256 other; }
    uint256[1100] data;
    Item[600] items;
    Root root;
    uint256 count;
    function index() internal returns (uint256) { ++count; return 0; }
    function rhs() internal returns (uint256) { ++count; return count; }
    function paged(bool compound) external returns (uint256, uint256) {
        data[0] = 10;
        count = 0;
        if (compound) data[index()] += rhs();
        else data[index()] = rhs();
        return (data[0], count);
    }
    function mutateItem() internal returns (uint256) { items[0].b = 9; return 7; }
    function nestedPaged() external returns (uint256, uint256, uint256) {
        items[0].a = 5;
        items[0].b = 0;
        count = 0;
        items[index()].a += mutateItem();
        return (items[0].a, items[0].b, count);
    }
    function rootIndex() internal returns (uint256) { ++root.other; return 0; }
    function rootValue() internal view returns (uint256) { return root.other; }
    function boxed() external returns (uint256, uint256) {
        root.other = 1;
        root.values[rootIndex()] = rootValue();
        return (root.values[0], root.other);
    }
    function tupleStores() external returns (uint256, uint256, uint256, uint256, uint256) {
        count = 0;
        (items[index()].a, items[index()].b) = (11, 22);
        root.other = 1;
        (root.values[rootIndex()], root.other) = (7, 10);
        return (items[0].a, items[0].b, count, root.values[0], root.other);
    }
    function pagedIncrement() external returns (uint256, uint256, uint256) {
        items[0].a = 7;
        count = 0;
        uint256 before = items[index()].a++;
        return (before, items[0].a, count);
    }
}
