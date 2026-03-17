// SPDX-License-Identifier: MIT
pragma solidity ^0.8.4;

import "../contracts/utils/LibZip.sol";

contract LibZipWrapper {
    function flzCompress(bytes calldata data) external pure returns (bytes memory) {
        return LibZip.flzCompress(data);
    }

    function flzDecompress(bytes calldata data) external pure returns (bytes memory) {
        return LibZip.flzDecompress(data);
    }

    function cdCompress(bytes calldata data) external pure returns (bytes memory) {
        return LibZip.cdCompress(data);
    }

    function cdDecompress(bytes calldata data) external pure returns (bytes memory) {
        return LibZip.cdDecompress(data);
    }
}
