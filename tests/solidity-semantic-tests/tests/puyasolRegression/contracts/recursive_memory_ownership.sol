// SPDX-License-Identifier: MIT
pragma solidity ^0.8.28;

contract RecursiveOwnershipAudit {
    struct Node { uint16 value; Node[] children; }
    function aliases(uint8 count) external pure returns (uint16, uint16, uint256, uint16, uint16) {
        Node[] memory children = new Node[](count);
        Node memory parent = Node(9, children);
        uint256 emptyLength = parent.children[0].children.length;
        parent.children[0].value = 7;
        uint16 throughOriginal = children[0].value;
        children[0].value = 11;
        uint16 throughParent = parent.children[0].value;
        children[0].children = children;
        parent.children[0].children[0].value = 13;
        return (throughOriginal, throughParent, emptyLength, children[0].value, parent.value);
    }
    function assignmentBounds(uint8 index) external pure returns (uint256) {
        Node[] memory children = new Node[](1);
        Node memory parent = Node(9, children);
        parent.children[index].children = children;
        return 7;
    }
}
