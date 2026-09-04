// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

library ModifiedLeaf {
    struct Cell {
        uint256 value;
    }

    modifier same(Cell memory cell, uint256 amount) {
        require(amount != 0);
        cell.value += 1;
        _;
        cell.value += 2;
    }

    function mutate(Cell memory cell, uint256 amount)
        internal
        pure
        same(cell, amount)
        returns (uint256)
    {
        return cell.value;
    }
}

function throughFree(ModifiedLeaf.Cell memory cell, uint256 amount)
    pure
    returns (uint256)
{
    return ModifiedLeaf.mutate(cell, amount);
}

library LibraryCaller {
    function throughLibrary(ModifiedLeaf.Cell memory cell, uint256 amount)
        internal
        pure
        returns (uint256)
    {
        return ModifiedLeaf.mutate(cell, amount);
    }
}

contract ModifierHost {
    // A host modifier with the same name must not replace the library's.
    modifier same(ModifiedLeaf.Cell memory, uint256) {
        revert("host modifier selected");
        _;
    }

    function freePath(uint256 start, uint256 amount)
        external
        pure
        returns (uint256 result, uint256 observed)
    {
        ModifiedLeaf.Cell memory cell = ModifiedLeaf.Cell(start);
        result = throughFree(cell, amount);
        observed = cell.value;
    }

    function libraryPath(uint256 start, uint256 amount)
        external
        pure
        returns (uint256 result, uint256 observed)
    {
        ModifiedLeaf.Cell memory cell = ModifiedLeaf.Cell(start);
        result = LibraryCaller.throughLibrary(cell, amount);
        observed = cell.value;
    }
}
