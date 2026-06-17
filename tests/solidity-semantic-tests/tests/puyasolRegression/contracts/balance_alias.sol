// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the `address(c).balance` temp-aliasing fix: two such reads in ONE
// expression must resolve to DISTINCT temps. The pre-fix `__app_balance_addr`
// was a fixed name, so the second app_params_get(AppAddress) clobbered the
// first and `sum` read the second child's balance twice (2*bBal) instead of
// aBal + bBal. Children are funded with different values so the balances differ
// (otherwise the bug would be invisible).
contract Child {
    uint256 public v;
    constructor() payable { v = msg.value; } // payable ctor → value-funding route
}

contract BalanceAlias {
    function probe()
        external
        payable
        returns (uint256 aBal, uint256 bBal, uint256 sum)
    {
        Child a = new Child{value: 200000}();
        Child b = new Child{value: 700000}();
        aBal = address(a).balance; // single read — unaffected by the bug
        bBal = address(b).balance; // single read — unaffected by the bug
        sum = address(a).balance + address(b).balance; // two reads in one expr — the bug site
    }
}
