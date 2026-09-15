pragma solidity ^0.8.20;

contract ContextOffsetBase {
    struct Item { uint256 x; }
    Item[] items;
    function touch(Item storage item) internal virtual { item.x = 1; }
    function relay(Item storage item) internal { touch((item)); }
    function check() external returns (uint256, uint256) {
        delete items;
        items.push(); items.push();
        relay(((items)[1]));
        return (items[0].x, items[1].x);
    }
}

contract ContextOffsetDerived is ContextOffsetBase {
    function touch(Item storage item) internal override { item.x = 9; }
    function checkBase() external returns (uint256, uint256) {
        delete items;
        items.push(); items.push();
        super.touch(((items)[1]));
        return (items[0].x, items[1].x);
    }
}

contract ContextMemoryTransfers {
    struct Item { uint256 x; }
    Item saved;
    function tupleWrite(Item memory p) internal pure {
        Item memory q;
        uint256 unused;
        ((q), unused) = ((p), 0);
        q.x = 9;
    }
    function tupleDeclare(Item memory p) internal pure {
        (Item memory q, uint256 unused) = (p, 0);
        q.x = 11;
    }
    function tupleHole(Item memory p) internal pure {
        Item memory q;
        (q,) = (p, 0);
        q.x = 13;
    }
    function assigned() external pure returns (uint256) {
        Item memory p = Item(1);
        tupleWrite(p);
        return p.x;
    }
    function declared() external pure returns (uint256) {
        Item memory p = Item(1);
        tupleDeclare(p);
        return p.x;
    }
    function hole() external pure returns (uint256) {
        Item memory p = Item(1);
        tupleHole(p);
        return p.x;
    }
    function swap() external pure returns (uint256, uint256) {
        Item memory a = Item(1);
        Item memory b = Item(2);
        Item memory saved = a;
        (a, b) = (b, a);
        b.x = 17;
        return (a.x, saved.x);
    }
    function branch(bool yes) external pure returns (uint256, uint256) {
        Item memory a = Item(1);
        Item memory b = Item(2);
        Item memory q;
        uint256 unused;
        (q, unused) = (yes ? a : b, 0);
        q.x = 19;
        return (a.x, b.x);
    }
    function narrow(uint8 a, int8 b) external pure returns (uint8 x, int8 y) {
        assembly { x := add(a, 257) y := sub(b, 257) }
    }
    function copyToStorage() external returns (uint256, uint256) {
        Item memory a = Item(1);
        Item memory b = a;
        a = Item(2);
        uint256 unused;
        (saved, unused) = (b, 0);
        b.x = 23;
        return (saved.x, b.x);
    }
}
