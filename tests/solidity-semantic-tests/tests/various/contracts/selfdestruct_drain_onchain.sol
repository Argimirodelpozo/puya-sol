// CUSTOM (puya-sol on-chain verification): create2-free selfdestruct so
// the actual CloseRemainderTo-on-app-account sweep gets exercised on
// localnet (the upstream selfdestruct_* tests all xfail on create2 before
// reaching the mechanism). Confirms post-Cancun selfdestruct(beneficiary)
// drains the app account's balance to the beneficiary — or surfaces that
// the AVM rejects CloseRemainderTo on an application account.
contract C {
    receive() external payable {}

    function boom(address payable beneficiary) public {
        selfdestruct(beneficiary);
    }
}
