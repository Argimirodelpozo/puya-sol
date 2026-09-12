// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

library PlacementLibrary {
    struct Small { uint8 x; uint8 y; uint8 z; }
    struct MappedSmall { uint8 x; }
    function bump(Small storage value) internal returns (uint256) {
        return ++value.x;
    }
    function bumpMapped(MappedSmall storage value) internal returns (uint256) {
        return ++value.x;
    }
}

function bumpFree(PlacementLibrary.Small storage value) returns (uint256) {
    return ++value.x;
}

contract StoragePlacementReferenceFacts {
    using PlacementLibrary for PlacementLibrary.Small;
    PlacementLibrary.Small public value;

    function run() external returns (uint256, uint256, uint256) {
        value.x = 7;
        uint256 libraryResult = value.bump();
        uint256 freeResult = bumpFree(value);
        return (libraryResult, freeResult, value.x);
    }
}

contract StoragePlacementMappedReferenceFacts {
    using PlacementLibrary for PlacementLibrary.MappedSmall;
    PlacementLibrary.MappedSmall public value;
    mapping(uint256 => PlacementLibrary.MappedSmall) public entries;

    function run() external returns (uint256, uint256, uint256, uint256) {
        value.x = 7;
        entries[1].x = 17;
        uint256 rootResult = value.bumpMapped();
        uint256 mappedResult = entries[1].bumpMapped();
        return (rootResult, mappedResult, value.x, entries[1].x);
    }
}
