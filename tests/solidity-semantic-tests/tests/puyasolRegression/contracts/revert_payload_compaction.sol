// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

library RevertPayloadLibrary {
    function fail(string memory reason) internal pure {
        revert(reason);
    }
}

contract RevertPayloadCompaction {
    uint256 private evaluations;

    function literal(uint256 which) external pure {
        if (which == 0) revert("");
        if (which == 1) require(false, "short");
        if (which == 2) revert("1234567890123456789012345678901234");
        if (which == 3) require(false, "12345678901234567890123456789012345");
        if (which == 4) revert(unicode"café λ");
        revert("a\x00b");
    }

    function dynamicRequire(bool ok, string memory reason) external pure {
        require(ok, reason);
    }

    function dynamicRevert(string memory reason) external pure {
        revert(reason);
    }

    function libraryRevert(string memory reason) external pure {
        RevertPayloadLibrary.fail(reason);
    }

    function message() internal returns (string memory) {
        evaluations++;
        return "evaluated";
    }

    function passing() external returns (uint256) {
        require(true, message());
        return evaluations;
    }
}
