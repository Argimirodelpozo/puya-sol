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

BlockContext* nearestBlock(Context* _ctx)
{
	for (Context* c = _ctx; c; c = c->parent())
		if (auto* blk = dynamic_cast<BlockContext*>(c))
			return blk;
	return nullptr;
}
}

void Context::setInConstructor(bool _flag)
{
	if (auto* fn = nearestFunction(this))
		fn->inConstructor = _flag;
}

std::string Context::resolveVarName(std::string const& _name, int64_t _declId)
{
	// Honour explicit remaps (used by modifier inliner when the same
	// modifier — with its own local vars — is applied multiple times).
	if (auto const* remap = findParamRemap(_declId))
		return remap->name;

	auto* blk = nearestBlock(this);
	int64_t existing = lookupVarId(_name);
	if (existing != 0 && existing != _declId)
	{
		// Name is shadowed — use unique name
		std::string unique = _name + "__" + std::to_string(_declId);
		if (blk) blk->varNameToId[unique] = _declId;
		return unique;
	}
	if (blk) blk->varNameToId[_name] = _declId;
	return _name;
}

std::string Context::lookupVarName(std::string const& _name, int64_t _declId) const
{
	std::string unique = _name + "__" + std::to_string(_declId);
	if (lookupVarId(unique) == _declId)
		return unique;
	return _name;
}

} // namespace puyasol::builder::sol_ast
