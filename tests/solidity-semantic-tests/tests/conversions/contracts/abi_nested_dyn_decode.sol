// CUSTOM: abi.decode of nested-dynamic arrays (elements are themselves dynamic),
// a recursive EVM offset-table walk that rebuilds the ARC4 layout.
//
// uint256[][] / uint256[][][] are tested as full round-trips (abi.encode of
// uint-element nested arrays is supported). string[] / bytes[] are decoded from
// a caller-supplied EVM blob instead — abi.ENCODE of string[]/bytes[] is a
// separate, pre-existing puya-backend limitation (reinterpret-of-bytes to a
// dynamic ARC4 element is rejected), so the decode is exercised in isolation
// against a real eth_abi-encoded input.
contract C {
    // uint256[][] : outer dyn array of dyn arrays of 32-byte elements
    function rtU256_2d() public pure returns (uint256, uint256, uint256, uint256) {
        uint256[][] memory a = new uint256[][](2);
        a[0] = new uint256[](2); a[0][0] = 11; a[0][1] = 22;
        a[1] = new uint256[](1); a[1][0] = 33;
        uint256[][] memory b = abi.decode(abi.encode(a), (uint256[][]));
        return (b.length, b[0][0], b[0][1], b[1][0]);
    }

    // uint256[][][] : 3-level nesting, validates the recursive walk
    function rtU256_3d() public pure returns (uint256, uint256, uint256, uint256) {
        uint256[][][] memory a = new uint256[][][](2);
        a[0] = new uint256[][](1); a[0][0] = new uint256[](2); a[0][0][0] = 7; a[0][0][1] = 8;
        a[1] = new uint256[][](2);
        a[1][0] = new uint256[](1); a[1][0][0] = 9;
        a[1][1] = new uint256[](1); a[1][1][0] = 10;
        uint256[][][] memory b = abi.decode(abi.encode(a), (uint256[][][]));
        return (b.length, b[0][0][1], b[1].length, b[1][1][0]);
    }

    // empty inner array edge case
    function rtEmptyInner() public pure returns (uint256, uint256, uint256) {
        uint256[][] memory a = new uint256[][](2);
        a[0] = new uint256[](0);
        a[1] = new uint256[](3); a[1][0] = 1; a[1][1] = 2; a[1][2] = 3;
        uint256[][] memory b = abi.decode(abi.encode(a), (uint256[][]));
        return (b.length, b[0].length, b[1][2]);
    }

    // string[] decode from a supplied EVM blob (encode of string[] is blocked)
    function decStrArr(bytes calldata data) public pure returns (string memory, string memory, uint256) {
        string[] memory d = abi.decode(data, (string[]));
        return (d[0], d[1], d.length);
    }

    // bytes[] decode from a supplied EVM blob
    function decBytesArr(bytes calldata data) public pure returns (uint256, uint256, uint256, bytes1) {
        bytes[] memory d = abi.decode(data, (bytes[]));
        return (d.length, d[0].length, d[1].length, d[1][0]);
    }

    // abi.encode(string[]) / abi.encode(bytes[]) — decode then RE-ENCODE; the
    // output must be byte-identical to the input EVM blob. (The array is built
    // via decode, not literal element assignment, which is a separate
    // still-open codegen gap — see task #22.)
    function reencStrArr(bytes calldata data) public pure returns (bytes memory) {
        return abi.encode(abi.decode(data, (string[])));
    }
    function reencBytesArr(bytes calldata data) public pure returns (bytes memory) {
        return abi.encode(abi.decode(data, (bytes[])));
    }
}
