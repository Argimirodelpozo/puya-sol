/// @file Context.cpp
/// Out-of-line setter / mutation implementations for `Context`. Each
/// walks the parent chain from `this` to find the scope level that
/// owns the binding (BlockContext for block-scoped, FunctionContext
/// for function-scoped, TranslationContext for contract-scoped) and
/// writes there.

#include "builder/sol-ast/Context.h"

namespace puyasol::builder::sol_ast
{

namespace
{
BlockContext* nearestBlock(Context* _ctx)
{
	for (Context* c = _ctx; c; c = c->parent())
		if (auto* blk = dynamic_cast<BlockContext*>(c))
			return blk;
	return nullptr;
}

FunctionContext* nearestFunction(Context* _ctx)
{
	for (Context* c = _ctx; c; c = c->parent())
		if (auto* fn = dynamic_cast<FunctionContext*>(c))
			return fn;
	return nullptr;
}

TranslationContext* nearestTranslation(Context* _ctx)
{
	for (Context* c = _ctx; c; c = c->parent())
		if (auto* tr = dynamic_cast<TranslationContext*>(c))
			return tr;
	return nullptr;
}

std::unordered_map<int64_t, std::string> const kEmptySuperTargets;
}

void Context::setStorageAlias(int64_t _declId, std::shared_ptr<awst::Expression> _expr)
{
	if (auto* blk = nearestBlock(this))
		blk->storageAliases[_declId] = std::move(_expr);
}

void Context::setFuncPtrTarget(
	int64_t _declId,
	solidity::frontend::FunctionDefinition const* _target)
{
	if (auto* blk = nearestBlock(this))
		blk->funcPtrTargets[_declId] = _target;
}

void Context::eraseFuncPtrTarget(int64_t _declId)
{
	for (Context* c = this; c; c = c->parent())
	{
		if (auto* blk = dynamic_cast<BlockContext*>(c))
		{
			auto it = blk->funcPtrTargets.find(_declId);
			if (it != blk->funcPtrTargets.end())
			{
				blk->funcPtrTargets.erase(it);
				return;
			}
		}
	}
}

void Context::setConstantLocal(int64_t _declId, unsigned long long _value)
{
	if (auto* blk = nearestBlock(this))
		blk->constantLocals[_declId] = _value;
}

void Context::setSlotStorageRef(int64_t _declId, std::shared_ptr<awst::Expression> _expr)
{
	if (auto* blk = nearestBlock(this))
		blk->slotStorageRefs[_declId] = std::move(_expr);
}

void Context::setMappingKeyParam(int64_t _declId, std::string _name)
{
	if (auto* fn = nearestFunction(this))
		fn->mappingKeyParams[_declId] = std::move(_name);
}

void Context::setInConstructor(bool _flag)
{
	if (auto* fn = nearestFunction(this))
		fn->inConstructor = _flag;
}

void Context::setParamRemap(int64_t _declId, ParamRemap _remap)
{
	if (auto* tr = nearestTranslation(this))
		tr->paramRemaps[_declId] = std::move(_remap);
}

void Context::eraseParamRemap(int64_t _declId)
{
	if (auto* tr = nearestTranslation(this))
		tr->paramRemaps.erase(_declId);
}

void Context::setSuperTarget(int64_t _declId, std::string _name)
{
	if (auto* tr = nearestTranslation(this))
		tr->superTargetNames[_declId] = std::move(_name);
}

void Context::clearSuperTargets()
{
	if (auto* tr = nearestTranslation(this))
		tr->superTargetNames.clear();
}

std::unordered_map<int64_t, std::string> const& Context::allSuperTargets() const
{
	for (Context const* c = this; c; c = c->m_parent)
		if (auto const* tr = dynamic_cast<TranslationContext const*>(c))
			return tr->superTargetNames;
	return kEmptySuperTargets;
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
