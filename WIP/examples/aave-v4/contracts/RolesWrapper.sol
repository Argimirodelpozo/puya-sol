// SPDX-License-Identifier: UNLICENSED
pragma solidity ^0.8.20;

import {Roles} from './Roles.sol';

contract RolesWrapper {
    function DEFAULT_ADMIN_ROLE() external pure returns (uint64) {
        return Roles.DEFAULT_ADMIN_ROLE;
    }
    function HUB_ADMIN_ROLE() external pure returns (uint64) {
        return Roles.HUB_ADMIN_ROLE;
    }
    function SPOKE_ADMIN_ROLE() external pure returns (uint64) {
        return Roles.SPOKE_ADMIN_ROLE;
    }
    function USER_POSITION_UPDATER_ROLE() external pure returns (uint64) {
        return Roles.USER_POSITION_UPDATER_ROLE;
    }
    function HUB_CONFIGURATOR_ROLE() external pure returns (uint64) {
        return Roles.HUB_CONFIGURATOR_ROLE;
    }
    function SPOKE_CONFIGURATOR_ROLE() external pure returns (uint64) {
        return Roles.SPOKE_CONFIGURATOR_ROLE;
    }
    function DEFICIT_ELIMINATOR_ROLE() external pure returns (uint64) {
        return Roles.DEFICIT_ELIMINATOR_ROLE;
    }
}
