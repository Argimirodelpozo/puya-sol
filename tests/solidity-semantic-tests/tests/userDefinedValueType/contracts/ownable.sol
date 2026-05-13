// ============================================================================
// THIS TEST MODIFIED FROM UPSTREAM SOLIDITY
// See tail-of-file comments for AVM adaptation notes.
// ============================================================================
// Implementation of OpenZepplin's
// https://github.com/OpenZeppelin/openzeppelin-contracts/blob/master/contracts/access/Ownable.sol
// using user defined value types.

contract Ownable {
    type Owner is address;
    Owner public owner = Owner.wrap(msg.sender);
    error OnlyOwner();
    modifier onlyOwner() {
        if (Owner.unwrap(owner) != msg.sender)
            revert OnlyOwner();

        _;
    }
    event OwnershipTransferred(Owner indexed previousOwner, Owner indexed newOwner);
    function setOwner(Owner newOwner) onlyOwner external {
        emit OwnershipTransferred({previousOwner: owner, newOwner: newOwner});
        owner = newOwner;
    }
    function renounceOwnership() onlyOwner external {
        owner = Owner.wrap(address(0));
    }
}
// ----
// # MODIFIED FOR ALGORAND #
// # On Algorand, msg.sender is the localnet default account (32-byte Algorand address),   #
// # not the EVM hardcoded 0x1212...12. The constructor sets owner = msg.sender, so the    #
// # deployer IS the owner. However, setOwner(0x1212...) transfers ownership to an address #
// # the caller doesn't control, so subsequent onlyOwner calls fail.                       #
// # We test: renounce (caller is still initial owner), owner()->0, then setOwner fails.   #
// #                                                                                       #
// # Original assertions:                                                                  #
// #   owner() -> 0x1212121212121212121212121212120000000012                                #
// #   setOwner(address): 0x1212...12 ->                                                   #
// #   renounceOwnership() ->                                                               #
// #   owner() -> 0                                                                        #
// #   setOwner(address): 0x1212...12 -> FAILURE, hex"5fc483c5"                            #
// renounceOwnership() ->
// owner() -> 0
// setOwner(address): 0x1212121212121212121212121212120000000012 -> FAILURE, hex"5fc483c5"
