pragma solidity ^0.8.28;

// The test replaces these marker comments with zero, one or three parentheses.
contract ParenthesizedSlots {
    struct Cell { uint256 value; uint256[2] items; }

    function at(uint256 slot) internal pure returns (Cell storage cell) {
        assembly { cell.slot := slot }
    }

    function slotReferences(bool choose) external returns (uint256, uint256) {
        /*(*/at(256)/*)*/.value = 11;
        /*(*/at(512)/*)*/.value = 22;
        Cell storage cell = /*(*/choose ? at(256) : at(512)/*)*/;
        /*(*/cell/*)*/.value += 1;
        /*(*/ /*(*/at(256)/*)*/.items /*)*/[1] = 33;
        return (/*(*/cell/*)*/.value, /*(*/ /*(*/at(256)/*)*/.items /*)*/[1]);
    }
}
