// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;
// Regression: bare `this` (contract-typed) lowered to the REAL application address while every
// other contract-typed value carries the FAKE app-id form bzero(24)++itob(appId) — so `this`
// flowing into an opaque contract-typed context (stored in another contract, passed through a
// library/interface param) made the eventual call-back target btoi(hash garbage). Fixed:
// `this` joins the fake-form convention; `address(this)`/`payable(this)` special-case to the
// real address (they are payable/balance-bearing).
interface ICb { function ping(uint64 x) external returns (uint64); }
contract Cee {
    ICb public target;
    function register(ICb t) external { target = t; }
    function pokeStored(uint64 x) external returns (uint64) { return target.ping(x); }
}
contract Cer is ICb {
    uint64 public last;
    function enrollAt(Cee c) external { c.register(this); }     // `this` crosses the boundary
    function ping(uint64 x) external returns (uint64) { last = x + 1; return last; }
    function realAddr() external view returns (address) { return address(this); }  // must stay REAL
}
