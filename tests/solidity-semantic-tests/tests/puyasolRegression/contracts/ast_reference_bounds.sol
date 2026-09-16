pragma solidity ^0.8.20;

contract AstReferenceBounds {
    struct S { uint256 value; }
    struct D { bytes value; }
    S[] private items;
    S[][] private rows;
    D[] private dynamicItems;
    constructor() {
        items.push(S(42));
        rows.push();
        rows[0].push(S(43));
        dynamicItems.push(D(hex"0000000000000000"));
    }
    function ignore(S storage) internal pure returns (uint256) { return 123; }
    function ignoreDynamic(D storage) internal pure returns (uint256) { return 124; }
    function element(uint256 i) external view returns (uint256) { return ignore((items[i])); }
    function nested(uint256 i, uint256 j) external view returns (uint256) { return ignore(rows[i][j]); }
    function dynamicElement(uint256 i) external view returns (uint256) { return ignoreDynamic(dynamicItems[i]); }
    function growIndex() internal returns (uint256) {
        items.push(S(9));
        return items.length - 1;
    }
    function growing() external returns (uint256) { return ignore(items[growIndex()]); }
    function calldataPointer(uint256[2][] calldata x, uint256 i) external pure returns (uint256 p) {
        assembly { x.offset := x.offset }
        uint256[2] calldata selected = ((x))[i];
        assembly { p := selected }
    }
    function calldataFixed(uint256[2][2] calldata x, uint256 i) external pure returns (uint256 p) {
        assembly { p := x }
        uint256[2] calldata selected = (x)[i];
        assembly { p := selected }
    }
    function calldataLocal(uint256[2][] calldata x, uint256 i) external pure returns (uint256 p) {
        uint256[2][] calldata local = x;
        assembly { local.offset := x.offset local.length := x.length }
        uint256[2] calldata selected = (local)[i];
        assembly { p := selected }
    }
}
