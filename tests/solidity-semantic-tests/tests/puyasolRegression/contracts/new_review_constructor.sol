// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

contract NewReviewSpawn {
    function answer() external pure returns (uint256) { return 42; }
}

library NewReviewCtorLib {
    function store(uint256 value) internal {
        assembly { sstore(100, value) }
    }
}

contract NewReviewCtorBase {
    NewReviewSpawn public child;

    constructor() {
        initialize();
    }

    function initialize() internal {
        deployChild();
        persistValue();
    }

    function deployChild() internal {
        child = new NewReviewSpawn();
    }

    function persistValue() internal {
        NewReviewCtorLib.store(91);
    }

    function rawLibraryValue() public view returns (uint256 value) {
        assembly { value := sload(100) }
    }
}

// B4/D13: base-constructor new/MemberAccess-assembly effects force post-init.
contract NewReviewConstructor is NewReviewCtorBase {
    function childAnswer() external returns (uint256) {
        return child.answer();
    }
}
