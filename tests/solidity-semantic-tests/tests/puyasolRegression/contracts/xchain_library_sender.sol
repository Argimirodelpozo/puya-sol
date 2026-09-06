// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

// Regression: under the xchain profile msg.sender is memoized into the
// contract-instance method __evm_sender. A LIBRARY (root subroutine) cannot
// invoke an instance method, so a library that reads msg.sender must inline
// the claim check instead. Permit2's PermitHash.hash() is this shape, reached
// through an EMPTY combining contract (Permit2 is SignatureTransfer, ...).
library Who {
    function me() internal view returns (address) { return msg.sender; }
    function tagged(uint256 x) internal view returns (bytes32) {
        return keccak256(abi.encode(x, msg.sender));
    }
}

abstract contract Base {
    using Who for uint256;
    function libSender() external view returns (address) { return Who.me(); }
    function libTag(uint256 x) external view returns (bytes32) { return x.tagged(); }
    function ownTag(uint256 x) external view returns (bytes32) {
        return keccak256(abi.encode(x, msg.sender));
    }
}

contract LibSender is Base {}
