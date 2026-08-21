// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// address.staticcall(data) now lowers like .call (inner ApplicationCall txn) instead of hard-erroring;
// a self-staticcall with abi.encodeWithSignature / abi.encodeCall resolves to a direct subroutine call,
// and the return is canonical EVM-encoded (round-trips through abi.decode). The EVM read-only guarantee is NOT
// enforced on AVM (documented warning) — that's the accepted divergence.
contract Base { function bar(uint256 x) external pure returns (uint256) { return x + 7; } }
contract C is Base {
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
    // encodeCall self-resolution: function-ref selector -> same-signature method on `this` (incl. inherited).
    function selfStaticCall(uint256 v) external view returns (uint256) {
        (bool ok, bytes memory r) = address(this).staticcall(abi.encodeCall(this.foo, (v)));
        require(ok && r.length == 32, "encodeCall failed");
        return abi.decode(r, (uint256));
    }
    function selfStaticInherited(uint256 v) external view returns (uint256) {
        (bool ok, bytes memory r) = address(this).staticcall(abi.encodeCall(Base.bar, (v)));   // inherited
        require(ok && r.length == 32, "inherited encodeCall failed");
        return abi.decode(r, (uint256));
    }
}
