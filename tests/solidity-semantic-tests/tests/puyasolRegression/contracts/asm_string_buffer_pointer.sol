// Guard for the memory-pointer seam: a `new string(n)` / `new bytes(n)` buffer
// used in inline assembly as its Yul memory POINTER — the OZ Strings.toString /
// toHexString idiom (`add(buffer, k)` + `mstore8(ptr, ..)` + `return buffer`).
// Such a buffer is promoted to the blob-backed (pointer) model; an outside-asm
// value-use materialises [len word][data] back out of the memory blob.
//
// Covered here in an INTERNAL library function (the real OZ layout) and a public
// function, since the two build paths mark asm-aggregates separately.

library Str {
    bytes16 private constant HEX = "0123456789abcdef";

    function toString(uint256 value) internal pure returns (string memory) {
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

    function toHexString(uint256 value, uint256 byteLen) internal pure returns (string memory) {
        bytes memory buffer = new bytes(2 * byteLen + 2);
        buffer[0] = "0";
        buffer[1] = "x";
        uint256 pos = 2 * byteLen + 1;
        unchecked {
            for (uint256 i = 0; i < byteLen; i++) {
                buffer[pos--] = HEX[value & 0xf];
                value >>= 4;
                buffer[pos--] = HEX[value & 0xf];
                value >>= 4;
            }
        }
        require(value == 0, "hex length insufficient");
        return string(buffer);
    }
}

contract AsmStringBuffer {
    using Str for uint256;

    // internal/library build path
    function dec(uint256 v) public pure returns (string memory) {
        return v.toString();
    }

    function hex32(uint256 v) public pure returns (string memory) {
        return v.toHexString(32);
    }

    // public build path (same idiom, marked by the other buildBlock)
    function decInline(uint256 value) public pure returns (string memory) {
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
}
