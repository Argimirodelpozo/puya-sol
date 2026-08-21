// CUSTOM puya-sol regression: public assembly return() decodes arbitrary EVM
// ABI shapes through the shared recursive decoder.
contract GenericAssemblyAbiReturn {
    struct Result {
        uint8 tag;
        string text;
        uint16[] values;
    }

    function scalar() external pure returns (uint16) {
        assembly {
            mstore(0, 513)
            return(0, 0x20)
        }
    }

    function nested() external pure returns (uint16[][] memory) {
        assembly {
            let p := mload(0x40)
            // top-level offset, outer length, then two child offsets
            mstore(p, 0x20)
            mstore(add(p, 0x20), 2)
            mstore(add(p, 0x40), 0x40)
            mstore(add(p, 0x60), 0xa0)
            // [513, 65535]
            mstore(add(p, 0x80), 2)
            mstore(add(p, 0xa0), 513)
            mstore(add(p, 0xc0), 65535)
            // [7]
            mstore(add(p, 0xe0), 1)
            mstore(add(p, 0x100), 7)
            return(p, 0x120)
        }
    }

    function structure() external pure returns (Result memory) {
        assembly {
            let p := mload(0x40)
            // single dynamic result: offset to struct payload
            mstore(p, 0x20)
            // struct head: tag, text offset, values offset
            mstore(add(p, 0x20), 251)
            mstore(add(p, 0x40), 0x60)
            mstore(add(p, 0x60), 0xa0)
            // "hi"
            mstore(add(p, 0x80), 2)
            mstore(add(p, 0xa0), shl(240, 0x6869))
            // [2, 50000]
            mstore(add(p, 0xc0), 2)
            mstore(add(p, 0xe0), 2)
            mstore(add(p, 0x100), 50000)
            return(p, 0x120)
        }
    }

    function tupleResult() external pure returns (uint8, uint16[] memory, string memory) {
        assembly {
            let p := mload(0x40)
            // tuple head: tag, values offset, text offset
            mstore(p, 9)
            mstore(add(p, 0x20), 0x60)
            mstore(add(p, 0x40), 0xc0)
            // [513, 65535]
            mstore(add(p, 0x60), 2)
            mstore(add(p, 0x80), 513)
            mstore(add(p, 0xa0), 65535)
            // "ok"
            mstore(add(p, 0xc0), 2)
            mstore(add(p, 0xe0), shl(240, 0x6f6b))
            return(p, 0x100)
        }
    }
}
