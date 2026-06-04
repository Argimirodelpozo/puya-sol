// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {AVM, Global} from "libs/AVM.sol";

/// @title ARC20 — ARC-20 "Smart ASA" controlling app, compiled to the AVM by puya-sol.
///
/// Faithful port of algorandfoundation/arc20 (smart_contracts/smart_asa/contract.py).
/// The app creates and controls a single underlying ASA: the ASA's manager / reserve /
/// freeze / clawback are all the app account, so ONLY the app can move it. The ARC-20
/// *admin* roles (manager/reserve/freeze/clawback) are separate addresses stored in app
/// state and authorize the ABI methods.
///
/// Freeze is enforced at the APPLICATION level — `global_frozen` (whole asset) and
/// `account_frozen[addr]` (per holder) live in app state and are checked inside
/// `asset_transfer`; the underlying ASA is never natively frozen. This matches the
/// reference, where all transfers route through the app's clawback.
///
/// Built on libs/AVM.sol intrinsics (asaCreate / asaTransfer / asaBalance /
/// asaDestroy / asaOptIn) + Global.creatorAddress() for the create guard. No new
/// compiler intrinsics — the standard is almost entirely app-level state + guards.
///
/// NOTE: method names use snake_case to mirror the ARC-20 ABI. Reference/foreign types
/// (asset, account) are modelled as uint64 (asset id) / address; selector-exact ARC-4
/// reference-type conformance is tracked separately (see task 54d).
contract ARC20 {
    // ── ARC-20 config: an app-level mirror of the controlled ASA's parameters ──
    uint64  public  smartAsaId;       // the controlled ASA id (0 = not yet created)
    uint64  private cfgTotal;
    uint32  private cfgDecimals;
    bool    private cfgDefaultFrozen;
    string  private cfgUnitName;
    string  private cfgName;
    string  private cfgUrl;
    bytes   private cfgMetadataHash;
    address private managerAddr;      // ARC-20 admin roles (NOT the underlying ASA roles)
    address private reserveAddr;
    address private freezeAddr;
    address private clawbackAddr;

    // ── app-level freeze state ──
    bool    private globalFrozen;
    mapping(address => bool) private accountFrozen;

    /// ARC-20 `get_asset_config` return: the 11 config fields, in spec order.
    struct AssetConfig {
        uint64  total;
        uint32  decimals;
        bool    defaultFrozen;
        string  unitName;
        string  name;
        string  url;
        bytes   metadataHash;
        address managerAddr;
        address reserveAddr;
        address freezeAddr;
        address clawbackAddr;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 1. asset_create — create the controlled ASA + record config/roles.
    //    Creator-guarded and one-shot (smartAsaId must be 0).
    // ─────────────────────────────────────────────────────────────────────────
    function asset_create(
        uint64 total,
        uint32 decimals,
        bool defaultFrozen,
        string memory unitName,
        string memory name,
        string memory url,
        bytes memory metadataHash,
        address manager_addr,
        address reserve_addr,
        address freeze_addr,
        address clawback_addr
    ) external returns (uint64) {
        require(msg.sender == Global.creatorAddress(), "arc20: only creator");
        require(smartAsaId == 0, "arc20: already created");

        // Underlying ASA: the app is every role, so only the app can move units.
        smartAsaId = AVM.asaCreate(total, uint8(decimals), name, unitName);

        cfgTotal = total;
        cfgDecimals = decimals;
        cfgDefaultFrozen = defaultFrozen;
        cfgUnitName = unitName;
        cfgName = name;
        cfgUrl = url;
        cfgMetadataHash = metadataHash;
        managerAddr = manager_addr;
        reserveAddr = reserve_addr;
        freezeAddr = freeze_addr;
        clawbackAddr = clawback_addr;

        return smartAsaId;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Read-only getters (ARC-20 readonly methods 9–12).
    // ─────────────────────────────────────────────────────────────────────────

    /// 9. get_asset_config — the full stored config.
    function get_asset_config(uint64 asset) external view returns (AssetConfig memory) {
        require(asset == smartAsaId, "arc20: bad asset");
        return AssetConfig({
            total: cfgTotal,
            decimals: cfgDecimals,
            defaultFrozen: cfgDefaultFrozen,
            unitName: cfgUnitName,
            name: cfgName,
            url: cfgUrl,
            metadataHash: cfgMetadataHash,
            managerAddr: managerAddr,
            reserveAddr: reserveAddr,
            freezeAddr: freezeAddr,
            clawbackAddr: clawbackAddr
        });
    }

    /// 10. get_asset_is_frozen — whole-asset freeze flag.
    function get_asset_is_frozen(uint64 freeze_asset) external view returns (bool) {
        require(freeze_asset == smartAsaId, "arc20: bad asset");
        return globalFrozen;
    }

    /// 11. get_account_is_frozen — per-account freeze flag.
    function get_account_is_frozen(uint64 freeze_asset, address freeze_account)
        external view returns (bool)
    {
        require(freeze_asset == smartAsaId, "arc20: bad asset");
        return accountFrozen[freeze_account];
    }

    /// 12. get_circulating_supply — total minus the units the app/reserve still holds.
    function get_circulating_supply(uint64 asset) external view returns (uint64) {
        require(asset == smartAsaId, "arc20: bad asset");
        return uint64(uint256(cfgTotal) - AVM.asaBalance(address(this), smartAsaId));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Mutating methods 2–8 — implemented in tasks 54b/54c. Stubbed here so the
    // full 12-method ARC-20 ABI surface (and its selectors) is stable from the start.
    // ─────────────────────────────────────────────────────────────────────────

    /// 2. asset_opt_in — ABI-conformance entry point. Canonically a user opts into the
    /// Smart ASA (registering per-account local state) alongside a grouped opt-in axfer.
    /// This port keeps per-account data (account_frozen) in box state, so there is no
    /// user local state to register — holders opt into the underlying ASA directly with
    /// a client-side 0-axfer. Validates the asset; otherwise a no-op.
    function asset_opt_in(uint64 asset) external view {
        require(asset == smartAsaId, "arc20: bad asset");
    }

    /// 3. asset_config — reconfigure the Smart ASA (manager only). App-level state update
    /// with no inner acfg (the underlying ASA's params are fixed at create); the new total
    /// must still cover the already-circulating supply.
    function asset_config(
        uint64 config_asset,
        uint64 total,
        uint32 decimals,
        bool defaultFrozen,
        string memory unitName,
        string memory name,
        string memory url,
        bytes memory metadataHash,
        address manager_addr,
        address reserve_addr,
        address freeze_addr,
        address clawback_addr
    ) external {
        require(config_asset == smartAsaId, "arc20: bad asset");
        require(msg.sender == managerAddr, "arc20: not manager");
        require(
            total >= uint64(uint256(cfgTotal) - AVM.asaBalance(address(this), smartAsaId)),
            "arc20: total below circulating"
        );
        cfgTotal = total;
        cfgDecimals = decimals;
        cfgDefaultFrozen = defaultFrozen;
        cfgUnitName = unitName;
        cfgName = name;
        cfgUrl = url;
        cfgMetadataHash = metadataHash;
        managerAddr = manager_addr;
        reserveAddr = reserve_addr;
        freezeAddr = freeze_addr;
        clawbackAddr = clawback_addr;
    }

    /// 4. asset_transfer — the one mover. Every transfer is an app-mediated clawback
    /// (the app is the underlying ASA's clawback), branching on who the sender/receiver
    /// is, mirroring the reference:
    ///   * mint  (asset_sender == app):   reserve-authorized; receiver must be unfrozen.
    ///   * burn  (asset_receiver == app): reserve-authorized; sender must be unfrozen.
    ///   * clawback (caller == clawbackAddr): bypasses freeze (admin recovery).
    ///   * regular: caller must be the sender; asset + both parties must be unfrozen.
    function asset_transfer(
        uint64 xfer_asset,
        uint64 asset_amount,
        address asset_sender,
        address asset_receiver
    ) external {
        require(xfer_asset == smartAsaId, "arc20: bad asset");

        if (asset_sender == address(this)) {
            // mint: distribute from the app's reserve holding
            require(msg.sender == reserveAddr, "arc20: not reserve");
            require(!accountFrozen[asset_receiver], "arc20: receiver frozen");
        } else if (asset_receiver == address(this)) {
            // burn: pull back into the app's reserve holding
            require(msg.sender == reserveAddr, "arc20: not reserve");
            require(!accountFrozen[asset_sender], "arc20: sender frozen");
        } else if (msg.sender == clawbackAddr) {
            // clawback: admin recovery, intentionally bypasses freeze checks
        } else {
            // regular holder-initiated transfer
            require(msg.sender == asset_sender, "arc20: not sender");
            require(!globalFrozen, "arc20: asset frozen");
            require(!accountFrozen[asset_sender], "arc20: sender frozen");
            require(!accountFrozen[asset_receiver], "arc20: receiver frozen");
        }

        AVM.asaTransfer(smartAsaId, asset_sender, asset_receiver, asset_amount);
    }

    /// 5. asset_freeze — toggle the whole-asset freeze (freeze admin only).
    function asset_freeze(uint64 freeze_asset, bool asset_frozen) external {
        require(freeze_asset == smartAsaId, "arc20: bad asset");
        require(msg.sender == freezeAddr, "arc20: not freeze admin");
        globalFrozen = asset_frozen;
    }

    /// 6. account_freeze — toggle one account's freeze (freeze admin only).
    function account_freeze(uint64 freeze_asset, address freeze_account, bool asset_frozen)
        external
    {
        require(freeze_asset == smartAsaId, "arc20: bad asset");
        require(msg.sender == freezeAddr, "arc20: not freeze admin");
        accountFrozen[freeze_account] = asset_frozen;
    }

    /// 7. asset_close_out — return the caller's entire balance to `close_to` via clawback,
    /// closing their position. A frozen holder may only close out back to the app (reserve),
    /// mirroring the reference's "frozen unless closing to the creator".
    function asset_close_out(uint64 close_asset, address close_to) external {
        require(close_asset == smartAsaId, "arc20: bad asset");
        uint256 bal = AVM.asaBalance(msg.sender, smartAsaId);
        if (bal > 0) {
            require(
                !accountFrozen[msg.sender] || close_to == address(this),
                "arc20: sender frozen"
            );
            AVM.asaTransfer(smartAsaId, msg.sender, close_to, bal);
        }
    }

    /// 8. asset_destroy — destroy the underlying ASA (manager only). Requires the app to
    /// hold every unit (circulating == 0); AVM.asaDestroy enforces that on-chain.
    function asset_destroy(uint64 destroy_asset) external {
        require(destroy_asset == smartAsaId, "arc20: bad asset");
        require(msg.sender == managerAddr, "arc20: not manager");
        AVM.asaDestroy(smartAsaId);
        smartAsaId = 0;
    }
}
