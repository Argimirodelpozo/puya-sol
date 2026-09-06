// SPDX-License-Identifier: MIT
pragma solidity >=0.8.28;

library FunctionStore {
    function append(
        function(uint256) internal pure returns (uint256)[] storage items,
        function(uint256) internal pure returns (uint256) callback
    ) internal {
        items.push(callback);
    }
}

contract FunctionStorageShape {
    struct Dynamic {
        uint16 tag;
        function(uint256) internal pure returns (uint256)[] items;
    }
    struct Fixed {
        uint16 tag;
        function(uint256) internal pure returns (uint256) callback;
    }
    function(uint256) internal pure returns (uint256)[] flat;
    function(uint256) internal pure returns (uint256)[][] nested;
    function(uint256) internal pure returns (uint256)[][2] fixedDynamic;
    Dynamic dynamicStruct;
    Fixed fixedStruct;
    function(uint256) internal pure returns (uint256)[2] fixedArray;

    function twice(uint256 x) internal pure returns (uint256) { return x * 2; }
    function plusThree(uint256 x) internal pure returns (uint256) { return x + 3; }

    function appendInternal(
        function(uint256) internal pure returns (uint256)[] storage items
    ) internal { items.push(plusThree); }

    function appendStruct(Dynamic storage value) internal { value.items.push(plusThree); }

    function setup() external {
        delete flat;
        delete nested;
        delete fixedDynamic;
        delete dynamicStruct;
        flat.push(twice);
        appendInternal(flat);
        FunctionStore.append(flat, twice);
        nested.push();
        nested[0].push(plusThree);
        nested[0].push(twice);
        fixedDynamic[1].push(plusThree);
        dynamicStruct.tag = 7;
        appendStruct(dynamicStruct);
        fixedStruct.tag = 9;
        fixedStruct.callback = twice;
        fixedArray[0] = twice;
        fixedArray[1] = plusThree;
    }

    function valuesA(uint256 x) external view returns (uint256, uint256, uint256, uint256) {
        return (flat[0](x), flat[1](x), flat[2](x), nested[0][0](x));
    }

    function valuesB(uint256 x) external view returns (uint256, uint256, uint256, uint256) {
        return (nested[0][1](x), fixedDynamic[1][0](x), dynamicStruct.items[0](x),
            fixedStruct.callback(x) + fixedArray[1](x));
    }

    function lengths() external view returns (uint256, uint256, uint256, uint256, uint256, uint16, uint16) {
        return (flat.length, nested.length, fixedDynamic[0].length,
            fixedDynamic[1].length, dynamicStruct.items.length, dynamicStruct.tag, fixedStruct.tag);
    }

    function mutate() external {
        flat[0] = plusThree;
        flat.pop();
        nested[0][0] = twice;
        fixedDynamic[1][0] = twice;
        dynamicStruct.items[0] = twice;
    }

    function changed(uint256 x) external view returns (uint256, uint256, uint256, uint256, uint256) {
        return (flat.length, flat[0](x), nested[0][0](x),
            fixedDynamic[1][0](x), dynamicStruct.items[0](x));
    }
}
