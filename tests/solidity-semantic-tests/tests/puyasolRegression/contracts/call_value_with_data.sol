// SPDX-License-Identifier: MIT
pragma solidity ^0.8.0;

// CUSTOM regression fixture (NOT vendored / not an original Solidity semantic test).
// Guards the `.call{value: X}(data)` fix: the pre-fix dispatcher routed ANY
// .call with a {value:} clause to a bare-payment lowering, silently dropping
// the calldata — the payment was sent, deposit() never ran, ok == true. The
// fix submits [PaymentTxn, ApplicationCall] as one inner group, so the callee
// both executes and sees msg.value (gtxns Amount at GroupIndex-1).
contract Vault {
    uint256 public deposits;
    uint256 public got;

    function deposit() external payable {
        deposits += 1;
        got = msg.value;
    }
}

contract CallValueData {
    function run() external payable returns (bool ok, uint256 deposits, uint256 got) {
        Vault v = new Vault();
        // encodeCall (typed): canonical ARC4 selector. encodeWithSignature's
        // EVM-form string can't match a puya-sol router (EVM_DIVERGENCE #2).
        (ok, ) = address(v).call{value: 150000}(abi.encodeCall(Vault.deposit, ()));
        deposits = v.deposits();
        got = v.got();
    }
}
