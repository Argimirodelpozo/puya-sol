// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/JSONParserLib.sol";

contract JSONParserWrapper {
    using JSONParserLib for string;

    function parse(string calldata json) external pure returns (string memory) {
        JSONParserLib.Item memory item = json.parse();
        return item.value();
    }
}
