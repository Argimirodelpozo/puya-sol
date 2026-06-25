// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// address.staticcall(data) now lowers like .call (inner ApplicationCall txn) instead of hard-erroring;
// a self-staticcall with abi.encodeWithSignature resolves to a direct subroutine call. The EVM read-only
// guarantee is NOT enforced on AVM (documented warning) — that's the accepted divergence.
contract C {
    function foo(uint256 x) public pure returns (uint256) { return x + 100; }
    function selfStatic(uint256 v) external view returns (uint256) {
        (bool ok, bytes memory r) = address(this).staticcall(abi.encodeWithSignature("foo(uint256)", v));
        require(ok && r.length == 32, "staticcall failed");
        return abi.decode(r, (uint256));
    }
    function selfCall(uint256 v) external returns (uint256) {
        (bool ok, bytes memory r) = address(this).call(abi.encodeWithSignature("foo(uint256)", v));
        require(ok && r.length == 32, "call failed");
        return abi.decode(r, (uint256));
    }
}
