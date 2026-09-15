#pragma once

#include "awst/Node.h"
#include "builder/solc/ProxyFacts.h"

#include "builder/solc/SolcFwd.h"

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
/// Only explicitly registered declarations are folded; see ProxyFacts.

class UupsLowering
{
public:
	static constexpr char const* GATE_NAME = "__uups_update";

	/// The replacement body for a folded function.
	static std::shared_ptr<awst::Block> foldedBody(
		UupsFold _fold, awst::SourceLocation const& _loc);

	/// The UpdateApplication gate: calls the translated `_authorizeUpgrade`
	/// method (`_authorizeMethod` — its inlined modifiers ARE the permission
	/// check) with an unused zero address placeholder, then
	/// emits Upgraded(address). UpdateApplication-only, never on create.
	static awst::ContractMethod updateGateMethod(
		std::string const& _cref,
		awst::ContractMethod const& _authorizeMethod,
		awst::SourceLocation const& _loc);
};

} // namespace puyasol::builder::proxies
