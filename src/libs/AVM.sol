// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/// AVM standard library — Solidity surface for Algorand-native primitives.
///
/// These libraries are recognised by the puya-sol compiler and replaced at
/// call-resolution time with the corresponding AVM intrinsic / inner-txn /
/// transaction-field opcodes. Bodies revert as a safety net so accidental
/// EVM use fails fast.
///
/// Layout:
///   library AVM     — ASA ops (create / opt-in / transfer / freeze / destroy
///                     + holding & params reads)
///   library Crypto  — Hash + signature verify (sha512_256, sha3_256,
///                     ed25519, falcon, vrf, ecdsa). Solidity's keccak256()
///                     and sha256() builtins map natively, no wrapper needed.
///   library Group   — Atomic-group txn access (size, index, gtxn fields)
///   library Txn     — Current-txn field reads (sender, fee, note, etc.)
///   library Global  — Global params (current app id/address, group id,
///                     opcode budget, latest timestamp)
library AVM {
    // ─── ASA: configuration ──────────────────────────────────────────────

    /// Create a new ASA owned and clawback-controlled by the contract.
    /// Returns the new ASA's id.
    function asaCreate(
        uint64 total,
        uint8 decimals,
        string memory name,
        string memory symbol
    ) internal returns (uint64) {
        total; decimals; name; symbol;
        revert("AVM.asaCreate: requires puya-sol");
    }

    /// Permanently destroy `assetId`. The contract must hold all units and
    /// be the asset's manager.
    function asaDestroy(uint64 assetId) internal {
        assetId;
        revert("AVM.asaDestroy: requires puya-sol");
    }

    /// Opt this contract into `assetId`. Required before receiving any units.
    function asaOptIn(uint64 assetId) internal {
        assetId;
        revert("AVM.asaOptIn: requires puya-sol");
    }

    /// Set `holder`'s freeze state for `assetId`.
    function asaFreeze(uint64 assetId, address holder, bool frozen) internal {
        assetId; holder; frozen;
        revert("AVM.asaFreeze: requires puya-sol");
    }

    // ─── ASA: transfers ──────────────────────────────────────────────────

    /// Clawback `amount` of `assetId` from `from` to `to`. Reverts if either
    /// party has not opted in. Use `from == address(this)` for plain sends.
    function asaTransfer(
        uint64 assetId,
        address from,
        address to,
        uint256 amount
    ) internal {
        assetId; from; to; amount;
        revert("AVM.asaTransfer: requires puya-sol");
    }

    // ─── ASA: reads ──────────────────────────────────────────────────────

    /// Read `holder`'s balance of `assetId`. Returns 0 if not opted in.
    function asaBalance(address holder, uint64 assetId)
        internal view returns (uint256)
    {
        holder; assetId;
        revert("AVM.asaBalance: requires puya-sol");
    }

    /// Read total supply of `assetId`.
    function asaTotalSupply(uint64 assetId) internal view returns (uint256) {
        assetId;
        revert("AVM.asaTotalSupply: requires puya-sol");
    }

    /// Read decimals of `assetId` (0..19).
    function asaDecimals(uint64 assetId) internal view returns (uint8) {
        assetId;
        revert("AVM.asaDecimals: requires puya-sol");
    }

    /// Read unit-name of `assetId` (Solidity `symbol()` analogue).
    function asaUnitName(uint64 assetId) internal view returns (string memory) {
        assetId;
        revert("AVM.asaUnitName: requires puya-sol");
    }

    /// Read display-name of `assetId` (Solidity `name()` analogue).
    function asaName(uint64 assetId) internal view returns (string memory) {
        assetId;
        revert("AVM.asaName: requires puya-sol");
    }
}

/// Hash + signature verification primitives.
library Crypto {
    /// SHA-512/256 hash. AVM-native; differs from EVM's keccak256.
    function sha512_256(bytes memory data) internal pure returns (bytes32) {
        data;
        revert("Crypto.sha512_256: requires puya-sol");
    }

    /// SHA-3-256 hash (Keccak with SHA-3 padding). AVM-native.
    function sha3_256(bytes memory data) internal pure returns (bytes32) {
        data;
        revert("Crypto.sha3_256: requires puya-sol");
    }

    /// Verify Ed25519 signature over the raw `data` bytes.
    /// `signature` is 64 bytes, `pubKey` is 32 bytes.
    function ed25519Verify(
        bytes memory data,
        bytes memory signature,
        bytes memory pubKey
    ) internal pure returns (bool) {
        data; signature; pubKey;
        revert("Crypto.ed25519Verify: requires puya-sol");
    }

    /// Verify Falcon signature over `data`. AVM v12+.
    /// `signature` is compressed-format Falcon-512, `pubKey` is 897 bytes.
    function falconVerify(
        bytes memory data,
        bytes memory signature,
        bytes memory pubKey
    ) internal pure returns (bool) {
        data; signature; pubKey;
        revert("Crypto.falconVerify: requires puya-sol");
    }

    /// Verify Algorand VRF proof. Returns (vrf_output, is_valid).
    /// ECVRF-ED25519-SHA512-Elligator2 (IETF draft-irtf-cfrg-vrf-03).
    function vrfVerify(
        bytes memory message,
        bytes memory proof,
        bytes memory pubKey
    ) internal pure returns (bytes memory, bool) {
        message; proof; pubKey;
        revert("Crypto.vrfVerify: requires puya-sol");
    }
}

/// Atomic-group transaction inspection.
library Group {
    /// Number of transactions in the current group (1 for stand-alone txn).
    function size() internal view returns (uint64) {
        revert("Group.size: requires puya-sol");
    }

    /// Index of THIS transaction within the group (0..size()-1).
    function index() internal view returns (uint64) {
        revert("Group.index: requires puya-sol");
    }

    /// Sender of group txn at `idx`.
    function txnSender(uint64 idx) internal view returns (address) {
        idx;
        revert("Group.txnSender: requires puya-sol");
    }

    /// Receiver of payment txn at `idx`. Reverts if not a payment.
    function txnReceiver(uint64 idx) internal view returns (address) {
        idx;
        revert("Group.txnReceiver: requires puya-sol");
    }

    /// Amount of payment / asset-transfer txn at `idx` (microAlgos / ASA units).
    function txnAmount(uint64 idx) internal view returns (uint64) {
        idx;
        revert("Group.txnAmount: requires puya-sol");
    }

    /// Asset receiver of axfer at `idx`.
    function txnAssetReceiver(uint64 idx) internal view returns (address) {
        idx;
        revert("Group.txnAssetReceiver: requires puya-sol");
    }

    /// Asset amount of axfer at `idx`.
    function txnAssetAmount(uint64 idx) internal view returns (uint64) {
        idx;
        revert("Group.txnAssetAmount: requires puya-sol");
    }

    /// Asset id (xferAsset) of axfer at `idx`.
    function txnAssetId(uint64 idx) internal view returns (uint64) {
        idx;
        revert("Group.txnAssetId: requires puya-sol");
    }

    /// Application id called by app-call txn at `idx`.
    function txnApplicationId(uint64 idx) internal view returns (uint64) {
        idx;
        revert("Group.txnApplicationId: requires puya-sol");
    }

    /// Fee (microAlgos) of txn at `idx`.
    function txnFee(uint64 idx) internal view returns (uint64) {
        idx;
        revert("Group.txnFee: requires puya-sol");
    }

    /// Type enum of txn at `idx` (0=unknown, 1=pay, 2=keyreg, 3=acfg, 4=axfer,
    /// 5=afrz, 6=appl).
    function txnType(uint64 idx) internal view returns (uint64) {
        idx;
        revert("Group.txnType: requires puya-sol");
    }
}

/// Current-transaction field reads.
library Txn {
    /// Transaction sender (caller).
    function sender() internal view returns (address) {
        revert("Txn.sender: requires puya-sol");
    }

    /// Fee paid by this txn (microAlgos).
    function fee() internal view returns (uint64) {
        revert("Txn.fee: requires puya-sol");
    }

    /// First-valid round.
    function firstValid() internal view returns (uint64) {
        revert("Txn.firstValid: requires puya-sol");
    }

    /// Last-valid round.
    function lastValid() internal view returns (uint64) {
        revert("Txn.lastValid: requires puya-sol");
    }

    /// 0-1024 byte free-form note.
    function note() internal view returns (bytes memory) {
        revert("Txn.note: requires puya-sol");
    }

    /// 32-byte lease value.
    function lease() internal view returns (bytes32) {
        revert("Txn.lease: requires puya-sol");
    }

    /// Type enum (see Group.txnType for values).
    function typeEnum() internal view returns (uint64) {
        revert("Txn.typeEnum: requires puya-sol");
    }

    /// Index within atomic group; 0 for stand-alone.
    function groupIndex() internal view returns (uint64) {
        revert("Txn.groupIndex: requires puya-sol");
    }

    /// 32-byte computed txn id.
    function txnId() internal view returns (bytes32) {
        revert("Txn.txnId: requires puya-sol");
    }

    /// Sender's new AuthAddr (rekey target), zero if no rekey.
    function rekeyTo() internal view returns (address) {
        revert("Txn.rekeyTo: requires puya-sol");
    }

    /// Application id invoked by this txn (0 in stateless / pay txns).
    function applicationId() internal view returns (uint64) {
        revert("Txn.applicationId: requires puya-sol");
    }

    /// OnCompletion enum (0=NoOp, 1=OptIn, 2=CloseOut, 3=ClearState,
    /// 4=UpdateApplication, 5=DeleteApplication).
    function onCompletion() internal view returns (uint64) {
        revert("Txn.onCompletion: requires puya-sol");
    }

    /// Number of app-call args.
    function numAppArgs() internal view returns (uint64) {
        revert("Txn.numAppArgs: requires puya-sol");
    }

    /// App-call arg `i` (0-indexed).
    function appArg(uint64 i) internal view returns (bytes memory) {
        i;
        revert("Txn.appArg: requires puya-sol");
    }
}

/// Global / protocol-level reads.
library Global {
    /// Current application id (`global CurrentApplicationID`).
    function currentApplicationId() internal view returns (uint64) {
        revert("Global.currentApplicationId: requires puya-sol");
    }

    /// Current application address (`global CurrentApplicationAddress`).
    function currentApplicationAddress() internal view returns (address) {
        revert("Global.currentApplicationAddress: requires puya-sol");
    }

    /// Address of the account that created this application (`global CreatorAddress`).
    /// Immutable for the life of the app; the canonical owner/admin for access control.
    function creatorAddress() internal view returns (address) {
        revert("Global.creatorAddress: requires puya-sol");
    }

    /// 32-byte group id of the current atomic group.
    function groupId() internal view returns (bytes32) {
        revert("Global.groupId: requires puya-sol");
    }

    /// Latest-block timestamp (Unix seconds). Same as Solidity's
    /// `block.timestamp` but exposed here for explicit-stdlib style.
    function latestTimestamp() internal view returns (uint64) {
        revert("Global.latestTimestamp: requires puya-sol");
    }

    /// Current round (Solidity `block.number` analogue).
    function round() internal view returns (uint64) {
        revert("Global.round: requires puya-sol");
    }

    /// Remaining opcode budget for this application call.
    function opcodeBudget() internal view returns (uint64) {
        revert("Global.opcodeBudget: requires puya-sol");
    }

    /// Application id of the caller (0 for top-level call from a non-app txn).
    function callerApplicationId() internal view returns (uint64) {
        revert("Global.callerApplicationId: requires puya-sol");
    }

    /// Minimum-balance requirement for `account` (microAlgos).
    function minBalance(address account) internal view returns (uint64) {
        account;
        revert("Global.minBalance: requires puya-sol");
    }

    /// Algo balance of `account` (microAlgos).
    function balance(address account) internal view returns (uint64) {
        account;
        revert("Global.balance: requires puya-sol");
    }
}

/// @title Bits
/// @dev Bit-twiddling backed by native AVM opcodes. Unlike the stub libraries
/// above (which puya-sol intercepts at call-resolution), this is REAL Solidity:
/// the Yul `clz` builtin lowers to the AVM `bitlen` opcode inside puya-sol, so
/// no compiler intercept is needed. `clz` is an Osaka EVM builtin, so callers
/// must compile with `--evm-version osaka`.
///
/// The EVM has no bit-length / count-leading-zeros opcode (pre-Osaka), so EVM
/// libraries hand-roll most/least-significant-bit with a 256-bit binary search
/// + a de Bruijn byte table — which lowers to ~1000 lines of byte-array ops on
/// the AVM. `bitlen` is one opcode. See EVMfun.md.
library Bits {
    /// Bit length of x: index of the highest set bit + 1, or 0 when x == 0.
    /// msb(x) = bitlen(x) - 1; lsb(x) = bitlen(x & -x) - 1.
    function bitlen(uint256 x) internal pure returns (uint256 r) {
        // clz(x) == 256 - bitlen(x); puya-sol emits AVM `bitlen` for clz.
        assembly ("memory-safe") {
            r := sub(256, clz(x))
        }
    }
}

/// @title Scratch
/// @dev AVM scratch space: 256 slots per transaction, ephemeral (cleared after
/// the txn), and a LATER txn in the same atomic group can READ (not write) an
/// EARLIER txn's slots via `gload`. This is the AVM analogue of EVM transient
/// storage (TSTORE), but scoped to the group — perfect for flash-accounting
/// deltas that must net to zero across a group of top-level calls and then
/// vanish. There is NO Yul/EVM equivalent (EVM "scratch" is memory 0x00-0x40,
/// unrelated), so these are stub bodies that puya-sol intercepts by library name
/// (AsaIntrinsics dispatchScratch): store->`stores`, loadSelf->`loads`,
/// load->`gloadss`.
///
/// NOTE: this minimal API is uint64-valued (slots default to 0). uint128/uint256
/// deltas need a biguint-bytes variant with explicit zero-init — future work.
library Scratch {
    /// Store `value` into THIS txn's scratch slot `slot`.
    function store(uint64 slot, uint64 value) internal {
        slot; value;
        revert("Scratch.store: requires puya-sol");
    }

    /// Read THIS txn's scratch slot `slot` (0 if never written).
    function loadSelf(uint64 slot) internal view returns (uint64) {
        slot;
        revert("Scratch.loadSelf: requires puya-sol");
    }

    /// Read scratch slot `slot` of group txn `groupIndex` (must be < this txn's
    /// group index — you can only read earlier txns). 0 if that txn never wrote it.
    function load(uint64 groupIndex, uint64 slot) internal view returns (uint64) {
        groupIndex; slot;
        revert("Scratch.load: requires puya-sol");
    }

    // ── bytes-valued variants ──
    // Scratch slots hold either a uint64 or a bytes value. The bytes variants let
    // a slot carry an arbitrary blob (e.g. a list of (currency, delta) entries for
    // multi-currency flash accounting). A slot never written returns empty bytes.

    /// Store a bytes blob into THIS txn's scratch slot `slot`.
    function storeBytes(uint64 slot, bytes memory value) internal {
        slot; value;
        revert("Scratch.storeBytes: requires puya-sol");
    }

    /// Read THIS txn's scratch slot `slot` as bytes (empty if never written).
    function loadBytesSelf(uint64 slot) internal view returns (bytes memory) {
        slot;
        revert("Scratch.loadBytesSelf: requires puya-sol");
    }

    /// Read group txn `groupIndex`'s scratch slot `slot` as bytes (must be an
    /// earlier txn; empty if it never wrote the slot).
    function loadBytes(uint64 groupIndex, uint64 slot) internal view returns (bytes memory) {
        groupIndex; slot;
        revert("Scratch.loadBytes: requires puya-sol");
    }
}
