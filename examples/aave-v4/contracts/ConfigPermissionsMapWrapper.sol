// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

import {ConfigPermissions, IConfigPositionManager} from './IConfigPositionManager.sol';
import {ConfigPermissionsMap} from './ConfigPermissionsMap.sol';

contract ConfigPermissionsMapWrapper {
    using ConfigPermissionsMap for ConfigPermissions;

    function setFullPermissions(bool status) external pure returns (uint8) {
        return ConfigPermissions.unwrap(ConfigPermissionsMap.setFullPermissions(status));
    }

    function setCanSetUsingAsCollateral(uint8 perms, bool status) external pure returns (uint8) {
        return ConfigPermissions.unwrap(
            ConfigPermissionsMap.setCanSetUsingAsCollateral(ConfigPermissions.wrap(perms), status)
        );
    }

    function setCanUpdateUserRiskPremium(uint8 perms, bool status) external pure returns (uint8) {
        return ConfigPermissions.unwrap(
            ConfigPermissionsMap.setCanUpdateUserRiskPremium(ConfigPermissions.wrap(perms), status)
        );
    }

    function setCanUpdateUserDynamicConfig(uint8 perms, bool status) external pure returns (uint8) {
        return ConfigPermissions.unwrap(
            ConfigPermissionsMap.setCanUpdateUserDynamicConfig(ConfigPermissions.wrap(perms), status)
        );
    }

    function canSetUsingAsCollateral(uint8 perms) external pure returns (bool) {
        return ConfigPermissionsMap.canSetUsingAsCollateral(ConfigPermissions.wrap(perms));
    }

    function canUpdateUserRiskPremium(uint8 perms) external pure returns (bool) {
        return ConfigPermissionsMap.canUpdateUserRiskPremium(ConfigPermissions.wrap(perms));
    }

    function canUpdateUserDynamicConfig(uint8 perms) external pure returns (bool) {
        return ConfigPermissionsMap.canUpdateUserDynamicConfig(ConfigPermissions.wrap(perms));
    }

    function eq(uint8 a, uint8 b) external pure returns (bool) {
        return ConfigPermissionsMap.eq(ConfigPermissions.wrap(a), ConfigPermissions.wrap(b));
    }

}
