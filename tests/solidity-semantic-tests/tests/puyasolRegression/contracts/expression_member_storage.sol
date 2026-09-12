// SPDX-License-Identifier: MIT
pragma solidity ^0.8.34;

contract ExpressionMemberStorage {
    uint256[] data;
    uint256[3] fixedData;
    uint256 count;
    function refs() internal view returns (uint256[] storage, uint256) { return (data, 1); }
    function fixedRef() internal returns (uint256[3] storage) { ++count; return fixedData; }
    function lengths() external returns (uint256, uint256, uint256) {
        delete data;
        data.push(7);
        data.push(11);
        (uint256[] storage ref,) = refs();
        count = 0;
        uint256 size = fixedRef().length;
        return (ref.length, size, count);
    }
}
