#pragma once

#include "builder/solc/SolcFwd.h"

#include <cstdint>
#include <map>
#include <vector>

namespace puyasol::builder { class SourceMap; }

namespace puyasol::builder::proxies
{

enum class UupsFold { None, EmptyBody, Trap, TrapDelegate };
enum class Erc1967UtilsFold { None, ImplementationLoad, AdminLoad, AdminStore, TrapImplementation, TrapBeacon };

/// Explicit dependency registration, validated once against solc declarations.
/// Annotation policy grants adaptation; names/paths alone never establish it.
struct ProxyFacts
{
	std::map<int64_t, UupsFold> uupsFunctions;
	std::map<int64_t, Erc1967UtilsFold> utilsFunctions;
	/// Concrete contract id -> exact virtual authorization hook, including modifiers.
	std::map<int64_t, solidity::frontend::FunctionDefinition const*> authorizationHooks;

	static ProxyFacts analyze(
		std::vector<solidity::frontend::ContractDefinition const*> const& _contracts,
		SourceMap const& _sources);
};

} // namespace puyasol::builder::proxies
