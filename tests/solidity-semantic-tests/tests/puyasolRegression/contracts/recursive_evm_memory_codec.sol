// CUSTOM puya-sol regression: recursive ARC4 <-> EVM memory conversion.
// Every parameter/local below is deliberately mentioned by inline assembly so
// --evm-memory-layout spills it to scratch-backed EVM memory. Returning the
// whole value forces the inverse materialisation path.
contract RecursiveEvmMemoryCodec {
    struct Item {
        uint8 tag;
        uint16[] values;
        bytes raw;
    }

    function echoU8(uint8[] memory value) external pure returns (uint8[] memory) {
        assembly { pop(value) }
        return value;
    }

    function echoFixed(uint8[3] memory value) external pure returns (uint8[3] memory) {
        assembly { pop(value) }
        return value;
    }

    function echoNested(uint16[][] memory value) external pure returns (uint16[][] memory) {
        assembly { pop(value) }
        return value;
    }

    function echo3d(uint8[][][] memory value) external pure returns (uint8[][][] memory) {
        assembly { pop(value) }
        return value;
    }

    function editNested(uint16[][] memory value)
        external pure returns (uint16, uint256)
    {
        assembly { pop(value) }
        value[0][1] = 60000;
        return (value[0][1], value[1].length);
    }

    function edit3d(uint8[][][] memory value) external pure returns (uint8) {
        assembly { pop(value) }
        value[1][0][0] = 250;
        return value[1][0][0];
    }

    function editFixed(uint8[3] memory value) external pure returns (uint8) {
        assembly { pop(value) }
        value[1] = 200;
        return value[1];
    }

    function buildItem(uint8 tag, uint16[] memory values, bytes memory raw)
        external pure returns (Item memory)
    {
        Item memory item = Item(tag, values, raw);
        assembly { pop(item) }
        return item;
    }

    function editItem(Item memory item) external pure returns (uint8, uint16, bytes1) {
        assembly { pop(item) }
        item.tag = 251;
        item.values[1] = 50000;
        item.raw[0] = 0x7a;
        return (item.tag, item.values[1], item.raw[0]);
    }
}
