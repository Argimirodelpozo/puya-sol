// SPDX-License-Identifier: MIT
pragma solidity ^0.8.26;

interface AuditNarrow {
    function choose(uint32 value) external pure returns (uint256);
}

// uint32/uint64 deliberately share a native ARC4 carrier; EVM identity must
// use the solc signature instead of that representation or declaration order.
contract AuditOverloads {
    function choose(uint64) external pure returns (uint256) { return 64; }
    function choose(uint32) external pure returns (uint256) { return 32; }
    function signatureCall() external returns (uint256) {
        (bool ok, bytes memory data) = address(this).call(
            abi.encodeWithSignature("choose(uint32)", uint32(7)));
        require(ok);
        return abi.decode(data, (uint256));
    }
    function selectorCall() external returns (uint256) {
        (bool ok, bytes memory data) = address(this).call(
            abi.encodeWithSelector(AuditNarrow.choose.selector, uint32(7)));
        require(ok);
        return abi.decode(data, (uint256));
    }
    function typedCall() external returns (uint256) {
        (bool ok, bytes memory data) = address(this).call(
            abi.encodeCall(AuditNarrow.choose, (uint32(7))));
        require(ok);
        return abi.decode(data, (uint256));
    }
    function missingOverload() external returns (bool) {
        (bool ok,) = address(this).call(abi.encodeWithSignature("choose(uint128)", uint128(7)));
        return ok;
    }
}
