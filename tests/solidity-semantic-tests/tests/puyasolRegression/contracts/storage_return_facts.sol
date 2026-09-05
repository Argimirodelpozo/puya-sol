pragma solidity ^0.8.24;

contract StorageReturnFacts {
    struct Item { uint64 value; }
    Item[] private items;
    mapping(uint64 => Item) private records;

    function memoryOnly(uint64 index) internal view returns (Item storage result) {
        assembly { mstore(0, 0) }
        return items[index];
    }

    function storageRead(uint64 index) internal view returns (Item storage result) {
        assembly { pop(sload(0)) }
        return items[index];
    }

    function lookup(uint64 key) internal view returns (Item storage result) {
        assembly { mstore(0, 0) }
        result = records[key];
    }

    function run(bool readStorage) external returns (uint64, uint64) {
        if (items.length == 0) items.push(Item(7));
        records[9].value = 23;
        Item storage arrayValue = memoryOnly(0);
        uint64 value = arrayValue.value;
        if (readStorage) value = storageRead(0).value;
        Item storage mapValue = lookup(9);
        return (value, mapValue.value);
    }

    function slotRef(uint256 slot) internal pure returns (Item storage result) {
        assembly { result.slot := slot }
    }

    function forwardSlot(uint256 slot) internal pure returns (Item storage result) {
        return slotRef(slot);
    }

    function localSlot(uint256 slot) internal pure returns (Item storage) {
        Item storage result;
        assembly { result.slot := slot }
        return result;
    }

    function slots() external pure returns (uint256 first, uint256 second, uint256 third) {
        Item storage a = slotRef(123);
        Item storage b = forwardSlot(456);
        Item storage c = localSlot(789);
        assembly { first := a.slot second := b.slot third := c.slot }
    }
}
