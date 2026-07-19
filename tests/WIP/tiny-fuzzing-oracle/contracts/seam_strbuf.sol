// Targeted seeds for the NEW blob-backed bytes/string asm-pointer path
// (new bytes/string(n) whose pointer escapes into a Yul local). Boundary cases
// for emitBytesBlobAlloc (32 + ceil(len/32)*32) and the value-use materialisation:
// length 0/1/31/32/33 word boundaries, partial writes, backwards/forwards pointer
// walks, word-mstore through the escaped pointer, multiple live buffers.
contract G {
    // classic OZ toString (backwards walk, mstore8 through escaped ptr)
    function dec(uint256 value) public pure returns (string memory) {
        unchecked {
            if (value == 0) return "0";
            uint256 length = 0;
            { uint256 v = value; while (v != 0) { length++; v /= 10; } }
            string memory buffer = new string(length);
            uint256 ptr;
            assembly { ptr := add(buffer, add(32, length)) }
            while (value != 0) {
                ptr--;
                assembly { mstore8(ptr, add(48, mod(value, 10))) }
                value /= 10;
            }
            return buffer;
        }
    }

    // exact length, forwards walk, every byte written
    function fill(uint256 n, uint8 b) public pure returns (bytes memory) {
        require(n <= 96, "cap");
        bytes memory buf = new bytes(n);
        uint256 p;
        assembly { p := add(buf, 32) }
        for (uint256 i = 0; i < n; i++) {
            assembly { mstore8(p, b) }
            p += 1;
        }
        return buf;
    }

    // PARTIAL write: only the first k bytes touched, rest must stay zero
    function partWrite(uint256 n, uint256 k, uint8 b) public pure returns (bytes memory) {
        require(n <= 96 && k <= n, "cap");
        bytes memory buf = new bytes(n);
        uint256 p;
        assembly { p := add(buf, 32) }
        for (uint256 i = 0; i < k; i++) {
            assembly { mstore8(p, b) }
            p += 1;
        }
        return buf;
    }

    // full 32-byte WORD store through the escaped pointer (not mstore8)
    function word(uint256 n, uint256 w) public pure returns (bytes memory) {
        require(n >= 32 && n <= 96, "cap");
        bytes memory buf = new bytes(n);
        uint256 p;
        assembly { p := add(buf, 32) }
        assembly { mstore(p, w) }
        return buf;
    }

    // two live buffers at once — FMP must advance past the first
    function two(uint256 a, uint256 bn) public pure returns (bytes memory, bytes memory) {
        require(a <= 64 && bn <= 64, "cap");
        bytes memory x = new bytes(a);
        bytes memory y = new bytes(bn);
        uint256 px;
        uint256 py;
        assembly { px := add(x, 32) }
        assembly { py := add(y, 32) }
        if (a > 0) { assembly { mstore8(px, 0xAA) } }
        if (bn > 0) { assembly { mstore8(py, 0xBB) } }
        return (x, y);
    }

    // .length read after asm writes (value-use of a blob-backed buffer)
    function lenAfter(uint256 n) public pure returns (uint256) {
        require(n <= 96, "cap");
        bytes memory buf = new bytes(n);
        uint256 p;
        assembly { p := add(buf, 32) }
        if (n > 0) { assembly { mstore8(p, 0x41) } }
        return buf.length;
    }

    // buffer flows into an internal fn AFTER asm writes
    function viaHelper(uint256 n, uint8 b) public pure returns (bytes memory) {
        require(n > 0 && n <= 96, "cap");
        bytes memory buf = new bytes(n);
        uint256 p;
        assembly { p := add(buf, 32) }
        assembly { mstore8(p, b) }
        return _echo(buf);
    }

    function _echo(bytes memory x) internal pure returns (bytes memory) { return x; }

    // string() cast of a blob-backed bytes buffer
    function asStr(uint256 n, uint8 b) public pure returns (string memory) {
        require(n > 0 && n <= 64, "cap");
        bytes memory buf = new bytes(n);
        uint256 p;
        assembly { p := add(buf, 32) }
        assembly { mstore8(p, b) }
        return string(buf);
    }
}
