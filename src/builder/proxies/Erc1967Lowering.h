#pragma once

#include "awst/Node.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace puyasol::builder::proxies
{

/// EIP-1967 proxy-slot lowering (see proxy.md §1).
///
/// The whole 1967 proxy edifice works around EVM code immutability; the AVM
/// has native app updates, so the proxy+implementation SYSTEM collapses to
/// one updatable application. This module recognizes the three magic slots
/// wherever inline assembly sloads/sstores them and lowers each to the
/// corresponding native fact:
///   admin slot          → a synthesized app global ("__erc1967_admin"),
///                         which also arms a bare UpdateApplication method
///                         gating native updates on that admin
///   implementation slot → this application's own identity on read
///                         (bytes24 ++ app id, the contract-value
///                         convention); a runtime trap on write (upgradeTo
///                         lowers to the native update ceremony, performed
///                         off-contract by the admin)
///   beacon slot         → runtime trap both directions (no AVM analogue;
///                         see proxy.md §4)
enum class Erc1967Slot
{
	None,
	Implementation,
	Admin,
	Beacon,
};

class Erc1967Lowering
{
public:
	static constexpr char const* ADMIN_KEY = "__erc1967_admin";

	/// Classify a slot expression: the three EIP-1967 constants (as
	/// compile-time IntegerConstants) or None.
	static Erc1967Slot classify(awst::Expression const* _slotExpr);

	/// classify() widened to VALUE positions: also matches a 32-byte
	/// BytesConstant holding a slot word (the form a Solidity-level
	/// `bytes32 constant` takes outside assembly).
	static Erc1967Slot classifyValue(awst::Expression const* _expr);

	/// "admin" / "implementation" / "beacon" for diagnostics.
	static char const* slotName(Erc1967Slot _slot);

	/// Warn (once per slot kind, via _warned) for every 1967 slot constant
	/// SURVIVING in a translated body. classify() consumes the constants in
	/// direct sload/sstore position, so a survivor means the slot value
	/// escaped into runtime data flow (function argument, memory store,
	/// arithmetic — e.g. OZ StorageSlot.getAddressSlot(SLOT).value) where
	/// storage accesses are NOT mapped to the native proxy model and split
	/// from the synthesized admin global / identity read / traps.
	static void warnEscapedSlotConstants(
		awst::ContractMethod const& _method, std::set<Erc1967Slot>& _warned);
	static void warnEscapedSlotConstants(
		awst::Statement const& _root, std::set<Erc1967Slot>& _warned);

	/// Admin-slot read: StateGet with a biguint-0 default — an unset admin
	/// reads as zero, exactly EVM's unset-slot semantics.
	static std::shared_ptr<awst::Expression> adminLoad(
		awst::SourceLocation const& _loc);

	/// Admin-slot write target assignment (value coerced to biguint by the
	/// caller). Statement is pushed into _out.
	static void adminStore(
		std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc,
		std::vector<std::shared_ptr<awst::Statement>>& _out);

	/// Implementation-slot read: this app's own identity,
	/// biguint(bytes24 ++ itob(CurrentApplicationID)).
	static std::shared_ptr<awst::Expression> implementationLoad(
		awst::SourceLocation const& _loc);

	/// Runtime trap statement for implementation/beacon writes and beacon
	/// reads (assert(false) with an explanatory message; unreachable sites
	/// are stripped by puya's DCE — the delegatecall precedent).
	static std::shared_ptr<awst::Statement> trapStatement(
		Erc1967Slot _slot, bool _isStore, awst::SourceLocation const& _loc);

	/// The synthesized admin global's declaration (appended to the
	/// contract's app_state when any admin-slot use was lowered).
	static awst::AppStorageDefinition adminStateDefinition(
		awst::SourceLocation const& _loc);

	/// The UpdateApplication method gating native updates on the 1967 admin:
	/// allowed ONLY for OnCompletion=UpdateApplication. The stored admin may
	/// be an ACCOUNT (raw 32 bytes → compared against Txn.Sender directly) or
	/// a CONTRACT identity (bytes24 ++ app id, this compiler's contract-value
	/// convention — the transparent-proxy ProxyAdmin topology): then the
	/// sender must be that application's ESCROW address, so an admin app
	/// driving an inner UpdateApplication authorizes. A zero (never-set)
	/// admin locks updates out entirely — fail closed.
	static awst::ContractMethod updateGateMethod(
		std::string const& _cref, awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::proxies
