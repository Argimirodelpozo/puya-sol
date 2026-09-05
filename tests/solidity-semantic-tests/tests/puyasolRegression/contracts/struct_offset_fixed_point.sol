pragma solidity ^0.8.24;

contract StructOffsetFixedPoint {
    struct Item { uint64 value; }
    Item[] private items;

    // Deliberately leaf-first: propagation must not depend on walk order.
    function step0(Item storage item) internal { item.value = 7; }
    function step1(Item storage item) internal { step0(item); }
    function step2(Item storage item) internal { step1(item); }
    function step3(Item storage item) internal { step2(item); }
    function step4(Item storage item) internal { step3(item); }
    function step5(Item storage item) internal { step4(item); }
    function step6(Item storage item) internal { step5(item); }
    function step7(Item storage item) internal { step6(item); }
    function step8(Item storage item) internal { step7(item); }
    function step9(Item storage item) internal { step8(item); }
    function step10(Item storage item) internal { step9(item); }

    function cycleA(Item storage item, uint64 depth) internal {
        if (depth == 0) { step0(item); return; }
        cycleB(item, depth - 1);
    }
    function cycleB(Item storage item, uint64 depth) internal { cycleA(item, depth); }

    function run(bool cycle) external returns (uint64, uint64) {
        if (items.length == 0) {
            items.push(Item(0));
            items.push(Item(0));
        }
        items[0].value = 0;
        items[1].value = 0;
        if (cycle) cycleA(items[1], 3);
        else step10(items[1]);
        return (items[0].value, items[1].value);
    }
}
