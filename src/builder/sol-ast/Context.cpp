#include "builder/sol-ast/Context.h"
#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

bool Context::isInConstructor() const
{
	return function && function->inConstructor;
}

std::set<std::string>* Context::liveCalldataPointers() const
{
	return function ? &function->seededCalldataPointers : nullptr;
}

int64_t Context::callableId() const
{
	return function ? function->callableId : 0;
}

std::string Context::awstVarName(solidity::frontend::VariableDeclaration const& _vd) const
{
	// Modifier-lowering remap wins (same modifier applied twice → unique per-instance
	// local names, keyed by decl id).
	if (auto const* remap = bindings.paramRemaps.find(_vd.id()))
		return remap->name;

	// Params/returns keep their bare name (unique in the fn, ABI-facing); locals and
	// catch params mangle to name__<declId> so shadows can't collide in the flat AWST
	// frame. Pure function of the decl — solc ids are globally unique.
	if (_vd.isCallableOrCatchParameter() && !_vd.isTryCatchParameter())
		return _vd.name();
	return _vd.name() + "__" + std::to_string(_vd.id());
}

} // namespace puyasol::builder::sol_ast
