/// @file Context.cpp
/// All decl-id-keyed mutators are inline in the header now (direct
/// hashmap ops on the shared ScopeState). Only the lexical-scope walks
/// that need to find the right typed ancestor remain here.

#include "builder/sol-ast/Context.h"

namespace puyasol::builder::sol_ast
{

namespace
{
FunctionContext* nearestFunction(Context* _ctx)
{
	for (Context* c = _ctx; c; c = c->parent())
		if (auto* fn = dynamic_cast<FunctionContext*>(c))
			return fn;
	return nullptr;
}

}

void Context::setInConstructor(bool _flag)
{
	if (auto* fn = nearestFunction(this))
		fn->inConstructor = _flag;
}

std::string Context::awstVarName(solidity::frontend::VariableDeclaration const& _vd) const
{
	// Modifier-inliner remap wins (same modifier applied twice → unique per-instance
	// local names, keyed by decl id).
	if (auto const* remap = findParamRemap(_vd.id()))
		return remap->name;

	// Params/returns keep their bare name (unique in the fn, ABI-facing); locals and
	// catch params mangle to name__<declId> so shadows can't collide in the flat AWST
	// frame. Pure function of the decl — solc ids are globally unique.
	if (_vd.isCallableOrCatchParameter() && !_vd.isTryCatchParameter())
		return _vd.name();
	return _vd.name() + "__" + std::to_string(_vd.id());
}

} // namespace puyasol::builder::sol_ast
