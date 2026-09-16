pragma solidity ^0.8.20;

contract Parentheses {
    struct Node { uint256 value; uint256[] items; }
    mapping(uint256 => Node) nodes;
    mapping(uint256 => mapping(uint256 => uint256)) nested;
    bytes data;

    function aliases() external returns (uint256, uint256) {
        nodes[1].value = 11;
        nodes[2].value = 22;
        Node storage p = nodes[1];
        (p) = (nodes[2]);
        ((p)).value = 33;
        return (nodes[1].value, nodes[2].value);
    }

    function nodeReference() internal view returns (Node storage) { return ((nodes[2])); }
    function namedReference() internal view returns (Node storage result) { (result) = ((nodes[2])); }
    function returnedReferences() external returns (uint256) {
        nodes[2].value = 1;
        (nodeReference()).value += 2;
        (namedReference()).value += 3;
        return nodes[2].value;
    }

    function indexedPaths() external returns (uint256, uint256, uint256) {
        ((nested[1]))[2] = 7;
        (((nodes[1])).items).push(9);
        uint256 length = ((nodes[1]).items).length;
        uint256 element = (((nodes[1]).items))[length - 1];
        delete (((nodes[1]).items))[length - 1];
        return (((nested[1]))[2], element, ((nodes[1]).items)[length - 1]);
    }

    function byteOps() external returns (uint256, uint256) {
        data = hex"010203";
        ((data)).push(0x04);
        delete ((data)[1]);
        ((data)).pop();
        return (((data)).length, uint8(((data))[1]));
    }

    function allocation() external pure returns (uint256, uint256) {
        bytes memory b = ((new bytes(3)));
        uint256 pointer;
        assembly { pointer := b }
        b[1] = 0x07;
        return (b.length, uint8((b)[1]) + (pointer == 0 ? 1 : 0));
    }

    function first(uint256[1] memory values) external pure returns (uint256) {
        return values[0];
    }

    function singleton() external returns (uint256) {
        (bool ok, bytes memory result) =
            ((address(this))).call(((abi.encodeCall)((this.first), (([uint256(7)])))));
        require(ok);
        return abi.decode(result, (uint256));
    }
}
