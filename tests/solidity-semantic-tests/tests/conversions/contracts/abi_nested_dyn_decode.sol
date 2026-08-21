// CUSTOM: abi.decode of nested-dynamic arrays (elements are themselves dynamic),
// a recursive EVM offset-table walk that rebuilds the ARC4 layout.
//
// Round-trips cover uint256[][] / uint256[][][] and literal-built dynamic
// elements. Caller-supplied canonical blobs independently check the decoder.
contract C {
    struct S { uint256 a; string b; }  // dynamic struct (has a string field)

    // S[] : array of DYNAMIC structs — full literal-built round-trip
    // (construct via element assignment -> abi.encode -> abi.decode -> read).
    function rtStructArr() public pure returns (uint256, string memory, uint256, string memory, uint256) {
        S[] memory arr = new S[](2);
        arr[0] = S(42, "hi");
        arr[1] = S(7, "world!!");
        S[] memory d = abi.decode(abi.encode(arr), (S[]));
        return (d[0].a, d[0].b, d[1].a, d[1].b, d.length);
    }
    // S[] decode from a real EVM blob (oracle cross-check)
    function decStructArr(bytes calldata data) public pure returns (uint256, uint256, uint256) {
        S[] memory arr = abi.decode(data, (S[]));
        return (arr[0].a, arr[1].a, arr.length);
    }

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

    // string[] decode from a supplied EVM blob
    function decStrArr(bytes calldata data) public pure returns (string memory, string memory, uint256) {
        string[] memory d = abi.decode(data, (string[]));
        return (d[0], d[1], d.length);
    }

    // bytes[] decode from a supplied EVM blob
    function decBytesArr(bytes calldata data) public pure returns (uint256, uint256, uint256, bytes1) {
        bytes[] memory d = abi.decode(data, (bytes[]));
        return (d.length, d[0].length, d[1].length, d[1][0]);
    }

    // External EVM-ABI regressions. The Python test supplies canonical EVM
    // head/tail bytes independently of this compiler's encoder.
    function decEvmU8(bytes calldata data) public pure returns (uint256, uint8, uint8, uint8) {
        uint8[] memory d = abi.decode(data, (uint8[]));
        return (d.length, d[0], d[1], d[2]);
    }

    function decEvmMixed(bytes calldata data) public pure returns (uint256, uint16, uint256, uint16) {
        uint16[][2] memory d = abi.decode(data, (uint16[][2]));
        return (d[0].length, d[0][1], d[1].length, d[1][0]);
    }

    function decEvm3d(bytes calldata data) public pure returns (uint256, uint256, uint256, uint256) {
        uint256[][][] memory d = abi.decode(data, (uint256[][][]));
        return (d.length, d[0][0][1], d[1].length, d[1][1][0]);
    }

    // abi.encode(string[]) / abi.encode(bytes[]) — decode then RE-ENCODE; the
    // output must be byte-identical to the input EVM blob.
    function reencStrArr(bytes calldata data) public pure returns (bytes memory) {
        return abi.encode(abi.decode(data, (string[])));
    }
    function reencBytesArr(bytes calldata data) public pure returns (bytes memory) {
        return abi.encode(abi.decode(data, (bytes[])));
    }

    // FULL literal-built round-trip: construct via element assignment (the
    // element store, formerly a hard-error), abi.encode, abi.decode, read back.
    function rtStrArr() public pure returns (string memory, string memory, uint256) {
        string[] memory s = new string[](2);
        s[0] = "foo"; s[1] = "barbaz";
        string[] memory d = abi.decode(abi.encode(s), (string[]));
        return (d[0], d[1], d.length);
    }
    function rtBytesArr() public pure returns (uint256, uint256, bytes1) {
        bytes[] memory b = new bytes[](2);
        b[0] = hex"aabb"; b[1] = hex"ccddee";
        bytes[] memory d = abi.decode(abi.encode(b), (bytes[]));
        return (d[0].length, d[1].length, d[1][2]);
    }
}
