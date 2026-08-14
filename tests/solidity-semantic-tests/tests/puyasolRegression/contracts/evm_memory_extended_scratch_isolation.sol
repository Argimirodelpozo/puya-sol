// SPDX-License-Identifier: MIT
pragma solidity >=0.8.0;

contract C {
    function probe() external returns (uint256 transientValue, uint256 memoryValue) {
        assembly {
            tstore(0, 111)
            // Logical EVM-memory blob 5 used to be physical scratch slot 5,
            // overwriting the transient-storage blob at the same offset.
            mstore(20480, 222)
            transientValue := tload(0)
            memoryValue := mload(20480)
        }
    }
}
