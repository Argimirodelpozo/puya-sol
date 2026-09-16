pragma solidity ^0.8.20;

contract AstTupleStorage {
    uint256[2] private scalar;
    uint256 private cursor;
    function next() internal returns (uint256) { return cursor++; }
    function parenthesizedDestinations() external returns (uint256, uint256, uint256) {
        cursor = 0;
        ((scalar[next()]), (scalar[next()])) = (7, 9);
        return (scalar[0], scalar[1], cursor);
    }
    struct S { uint256[] values; bytes data; }
    struct Packed { address account; uint16 tag; }
    S private x;
    S private y;
    Packed private px;
    Packed private py;

    function dynamicMembers() external returns (uint256, uint256, bytes memory, bytes memory) {
        delete x; delete y;
        x.values.push(11); y.values.push(22);
        x.data = hex"010203"; y.data = hex"aabb";
        (x, y) = (y, x);
        return (x.values[0], y.values[0], x.data, y.data);
    }

    function packedMembers() external returns (bool) {
        px = Packed(msg.sender, 11);
        py = Packed(address(this), 22);
        (px, py) = (py, px);
        return px.account == msg.sender && py.account == msg.sender && px.tag == 11 && py.tag == 11;
    }
}
