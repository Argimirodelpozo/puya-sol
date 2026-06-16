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
	// Modifier-inliner remap wins: when the same modifier (with its own locals) is
	// applied multiple times in one function, each instance's locals get a unique
	// mangled name, registered by the inliner keyed on the decl id.
	if (auto const* remap = findParamRemap(_vd.id()))
		return remap->name;

	// Function input/return parameters keep their bare name — unique within the
	// function and surfaced in the ABI (and the named-return tuple). Everything
	// else function-scoped — local variables and catch-clause params — is mangled
	// `name__<declId>` so name shadowing across sibling/nested blocks can't
	// collide in the flat AWST frame. solc assigns globally-unique decl ids, so
	// this is a pure function of the decl: no per-block shadow map or parent-chain
	// name walk is needed (the old resolveVarName/lookupVarName + varNameToId).
	// References in inline assembly resolve through the same rule via
	// SolInlineAssembly's externalReferences → AssemblyBuilder external-var map.
	if (_vd.isCallableOrCatchParameter() && !_vd.isTryCatchParameter())
		return _vd.name();
	return _vd.name() + "__" + std::to_string(_vd.id());
}

} // namespace puyasol::builder::sol_ast
