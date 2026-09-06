pragma solidity >=0.8.20;

contract SlotBindings {
    struct Item { uint256 value; }

    function selected(uint256 slot) internal pure returns (Item storage result) {
        assembly { result.slot := slot }
    }

    function run() public pure returns (bool) {
        Item storage pointer = selected(7);
        uint256 observed;
        {
            Item storage pointer;
            assembly { pointer.slot := 9 observed := pointer.slot }
            require(observed == 9);
        }
        assembly { observed := pointer.slot }
        require(observed == 7);

        Item storage first;
        Item storage second;
        uint256 a;
        uint256 b;
        assembly {
            function pair() -> x, y { x := 123 y := 456 }
            first.slot, second.slot := pair()
            a := first.slot
            b := second.slot
        }
        return a == 123 && b == 456;
    }
}
