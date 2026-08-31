#pragma once

#include "awst/Node.h"

#include "builder/sol-types/SolcFwd.h"

#include <memory>
#include <string>

namespace puyasol::builder::proxies
{

/// UUPS (EIP-1822) lowering — proxy.md §3.
///
/// UUPS puts the upgrade machinery in the IMPLEMENTATION (OZ
/// UUPSUpgradeable): `__self`-based context checks distinguish
/// through-the-proxy from direct execution, and `upgradeToAndCall` writes
/// the 1967 implementation slot from the inside. On the AVM the
/// proxy/implementation pair is ONE updatable app, so:
///   onlyProxy / notDelegated       → constant-true (their check bodies
///                                    fold to no-ops; there is no
///                                    delegated-vs-direct distinction)
///   upgradeToAndCall / upgradeTo   → runtime trap: the upgrade is the
///                                    native UpdateApplication ceremony
///   _authorizeUpgrade(address)     → the user's permission hook becomes
///                                    the update gate: a synthesized
///                                    UpdateApplication-only ABI method
///                                    calls it (its modifiers included)
///                                    and emits ARC-28 Upgraded(address).
///
/// Recognition is the contained idiom kind: member functions of a base
/// contract NAMED "UUPSUpgradeable" (the OZ artifact), by function name.
enum class UupsFold
{
	None,
	/// _checkProxy / _checkNotDelegated: body → no-op (check passes).
	EmptyBody,
	/// upgradeTo(AndCall) / _upgradeToAndCallUUPS: body → runtime trap.
	Trap,
};

class UupsLowering
{
public:
	static constexpr char const* GATE_NAME = "__uups_update";

	/// Fold classification for a function about to be translated.
	static UupsFold classify(solidity::frontend::FunctionDefinition const& _func);

	/// The replacement body for a folded function.
	static std::shared_ptr<awst::Block> foldedBody(
		UupsFold _fold, awst::SourceLocation const& _loc);

	/// True when `_contract` linearizes over a base named "UUPSUpgradeable" —
	/// the concrete contract of a UUPS implementation.
	static bool isUupsImplementation(
		solidity::frontend::ContractDefinition const& _contract);

	/// The UpdateApplication gate: calls the translated `_authorizeUpgrade`
	/// method (`_authorizeMethod` — its inlined modifiers ARE the permission
	/// check) with this app's own address as the "new implementation", then
	/// emits Upgraded(address). UpdateApplication-only, never on create.
	static awst::ContractMethod updateGateMethod(
		std::string const& _cref,
		awst::ContractMethod const& _authorizeMethod,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::proxies
