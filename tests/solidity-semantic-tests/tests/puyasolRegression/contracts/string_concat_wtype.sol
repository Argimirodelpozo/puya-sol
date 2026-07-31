// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;
// CUSTOM regression fixture (NOT vendored). Guards the string.concat WType.
//
// `string.concat(...)` returns `string memory`, but it lowers to the `concat`
// INTRINSIC, whose result was labelled plain `bytes`. Identical bytes at
// runtime, different AWST WType — and puya type-checks assignment target vs
// value, so binding the result to a string-typed local made it reject the whole
// program with
//     assignment target type differs from expression value type
// The RETURN path had its own fixup, so only assign-to-a-local failed; that is
// why `direct` below compiled while `viaLocal` did not.
//
// Same family as the modifier-argument bytes32 bug (see
// modifier_arg_bytes32.sol): an intrinsic result labelled `bytes` bound to a
// declared target from a different corner of the byte-string family.
// Fixed at the SOURCE — SolBytesConcat labels its result with the call's
// declared type — so every consumer agrees, not just the ones with a fixup.
contract StringConcatWType {
    string public nm;

    constructor(string memory n) { nm = string.concat("x", n); }

    function viaLocal(string memory n) external pure returns (string memory) {
        string memory s = string.concat("x", n);      // the shape that failed
        return s;
    }
    function direct(string memory n) external pure returns (string memory) {
        return string.concat("a", n, "b");
    }
    function chained(string memory a, string memory b) external pure returns (string memory) {
        string memory s = string.concat(a, "-", b);
        return string.concat(s, s);
    }
    function lenOf(string memory n) external pure returns (uint256) {
        string memory s = string.concat("pre", n);
        return bytes(s).length;
    }
    // bytes.concat must KEEP its bytes label — the fix is string-only
    function bcat(bytes memory a) external pure returns (bytes memory) {
        return bytes.concat(a, hex"ff");
    }
    function toBytes(string memory n) external pure returns (bytes memory) {
        return bytes(string.concat("z", n));
    }
}
