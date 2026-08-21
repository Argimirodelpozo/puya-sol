// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// Public/external library calls are DelegateCall in solc's
// type system but are lowered as reference-preserving internal subroutines by
// puya-sol. Their mutated memory parameters therefore need caller write-back.
library PublicMemoryLib {
    function fill(uint256[] memory values, uint256 x) public pure {
        values[0] = x;
        values[1] = x + 1;
    }

    function fillAndSum(uint256[] memory values, uint256 x)
        public pure returns (uint256)
    {
        values[0] = x;
        values[1] = x + 1;
        return values[0] + values[1];
    }

    function fillExternal(uint256[] memory values, uint256 x) external pure {
        values[0] = x;
        values[1] = x + 1;
    }
}

contract PublicLibraryMemoryCaller {
    using PublicMemoryLib for uint256[];

    function direct(uint256 x) external pure returns (uint256, uint256) {
        uint256[] memory values = new uint256[](2);
        PublicMemoryLib.fill(values, x);
        return (values[0], values[1]);
    }

    function attached(uint256 x) external pure returns (uint256, uint256) {
        uint256[] memory values = new uint256[](2);
        values.fill(x);
        return (values[0], values[1]);
    }

    function withReturn(uint256 x)
        external pure returns (uint256, uint256, uint256)
    {
        uint256[] memory values = new uint256[](2);
        uint256 sum = PublicMemoryLib.fillAndSum(values, x);
        return (sum, values[0], values[1]);
    }

    function externalVisibility(uint256 x)
        external pure returns (uint256, uint256)
    {
        uint256[] memory values = new uint256[](2);
        PublicMemoryLib.fillExternal(values, x);
        return (values[0], values[1]);
    }
}
