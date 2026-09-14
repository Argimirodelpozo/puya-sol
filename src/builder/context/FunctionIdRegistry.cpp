#include "builder/context/FunctionIdRegistry.h"
#include "builder/context/ProgramAnalysis.h"
#include "builder/lowering/calls/FunctionPointerBuilder.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

void registerFunctionIds(ProgramAnalysis const& analysis, FunctionSymbolTable& symbols)
{
	symbols.clear();
	for (auto const& [id, function]: analysis.functionDeclarations)
	{
		if (!function->isImplemented() || function->isConstructor()) continue;
		auto const* owner = function->annotation().contract;
		bool const root = function->isFree() || (owner && owner->isLibrary());
		using solidity::frontend::Visibility;
		if (root || function->visibility() == Visibility::Internal
			|| function->visibility() == Visibility::Private)
			symbols.registerDeclaration(id, root);
	}
}

void presetDispatchCref(ProgramAnalysis const& analysis, eb::FunctionPointerRegistry& pointers)
{
	for (auto const* contract: analysis.contracts)
		if (!contract->isLibrary() && !contract->abstract() && !contract->isInterface())
		{
			pointers.currentCref = contract->fullyQualifiedName();
			return;
		}
}

} // namespace puyasol::builder
