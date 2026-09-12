pragma solidity ^0.8.20;

library ExactAlias {
    struct StringSlot { string value; }
    function named(uint64 before_, string storage store, uint64 after_) internal pure
        returns (StringSlot storage result) {
        assembly { result.slot := store.slot }
    }
    function bound(string storage store, uint64 before_, uint64 after_) internal pure
        returns (StringSlot storage result) {
        assembly { result.slot := store.slot }
    }
}

contract AliasBase {
    function choose(string storage first, string storage second) internal pure virtual
        returns (ExactAlias.StringSlot storage result) {
        assembly { result.slot := first.slot }
    }
}

contract StorageAliasFacts is AliasBase {
    using ExactAlias for string;
    string private text;
    string[1] private texts;
    string private other;
    uint64 private counter;

    function step(uint64 digit) internal returns (uint64) {
        counter = counter * 10 + digit;
        return digit;
    }
    function pick(uint64 digit) internal returns (uint256) { step(digit); return 0; }
    function read(string storage store) internal view returns (uint64) {
        return uint64(bytes(store.bound(0, 0).value).length);
    }
    function choose(string storage first, string storage second) internal pure override
        returns (ExactAlias.StringSlot storage result) {
        assembly { result.slot := second.slot }
    }

    function named() external returns (uint64, uint64) {
        counter = 0;
        ExactAlias.named({after_: step(2), store: text, before_: step(1)}).value = "hello";
        return (counter, read(text));
    }
    function bound() external returns (uint64, uint64) {
        counter = 0;
        texts[pick(3)].bound({after_: step(2), before_: step(1)}).value = "hello";
        return (counter, uint64(bytes(texts[0]).length));
    }
    function virtualTarget() external returns (uint64, uint64) {
        text = "one";
        other = "two";
        choose(text, other).value = "chosen";
        return (uint64(bytes(text).length), uint64(bytes(other).length));
    }
}
