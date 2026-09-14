/// @file SuperCallResolution.cpp
/// Emit concrete implementations requested by expression-based solc lookup.

#include "builder/contract/ContractBuilder.h"
#include "builder/lowering/calls/CallResolver.h"

namespace puyasol::builder
{

void ContractBuilder::emitSuperSubroutines(
	awst::Contract& contract, std::string const& contractName)
{
	// Registration deduplicates before enqueueing. Translating a body can
	// append more targets (including lower AST IDs); index traversal reaches
	// all of them without revisiting recursive edges or invalidating iterators.
	auto& pending = m_exprBuilder->pendingBaseImplementations;
	for (size_t i = 0; i < pending.size(); ++i)
	{
		auto const* function = pending[i];
		appendMethodWithModifierSubs(contract, buildFunction(
			*function, contractName,
			eb::CallResolver::baseImplementationName(*m_exprBuilder, *function),
			/*_asInternalCopy=*/true));
	}
	pending.clear();
}

} // namespace puyasol::builder
