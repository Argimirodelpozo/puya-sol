#include "builder/sol-eb/BuilderContext.h"

#include "builder/sol-ast/Context.h"
#include "builder/sol-ast/SolExpressionDispatch.h"
#include "builder/sol-eb/BinaryOpBuilder.h"
#include "builder/sol-eb/BuilderRegistry.h"

namespace puyasol::builder::eb
{

bool BuilderContext::isUnchecked() const
{
	return currentScope && currentScope->isUnchecked();
}

std::shared_ptr<awst::Expression> BuilderContext::findStorageAlias(int64_t _declId) const
{
	return currentScope ? currentScope->findStorageAlias(_declId) : nullptr;
}

void BuilderContext::setStorageAlias(
	int64_t _declId,
	std::shared_ptr<awst::Expression> _expr
)
{
	for (auto* ctx = currentScope; ctx; ctx = ctx->parent())
	{
		if (auto* blk = dynamic_cast<sol_ast::BlockContext*>(ctx))
		{
			blk->storageAliases[_declId] = std::move(_expr);
			return;
		}
	}
}

namespace
{
sol_ast::BlockContext* nearestBlock(sol_ast::Context* _scope)
{
	for (auto* ctx = _scope; ctx; ctx = ctx->parent())
		if (auto* blk = dynamic_cast<sol_ast::BlockContext*>(ctx))
			return blk;
	return nullptr;
}
} // namespace

std::string BuilderContext::resolveVarName(std::string const& _name, int64_t _declId)
{
	// Honour explicit remaps (used by modifier inliner when the same
	// modifier — with its own local vars — is applied multiple times).
	auto remapIt = paramRemaps.find(_declId);
	if (remapIt != paramRemaps.end())
		return remapIt->second.name;

	auto* blk = nearestBlock(currentScope);
	int64_t existing = currentScope ? currentScope->lookupVarId(_name) : 0;
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

std::string BuilderContext::lookupVarName(std::string const& _name, int64_t _declId) const
{
	std::string unique = _name + "__" + std::to_string(_declId);
	if (currentScope && currentScope->lookupVarId(unique) == _declId)
		return unique;
	return _name;
}

solidity::frontend::FunctionDefinition const* BuilderContext::findFuncPtrTarget(
	int64_t _declId
) const
{
	return currentScope ? currentScope->findFuncPtrTarget(_declId) : nullptr;
}

void BuilderContext::setFuncPtrTarget(
	int64_t _declId,
	solidity::frontend::FunctionDefinition const* _target
)
{
	if (auto* blk = nearestBlock(currentScope))
		blk->funcPtrTargets[_declId] = _target;
}

void BuilderContext::eraseFuncPtrTarget(int64_t _declId)
{
	for (auto* ctx = currentScope; ctx; ctx = ctx->parent())
	{
		if (auto* blk = dynamic_cast<sol_ast::BlockContext*>(ctx))
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

BuilderContext::BuilderContext(
	TypeMapper& _typeMapper,
	StorageMapper& _storageMapper,
	std::string const& _sourceFile,
	std::string const& _contractName,
	std::unordered_map<std::string, std::string> const& _libraryFunctionIds,
	std::unordered_set<std::string> const& _overloadedNames,
	std::unordered_map<int64_t, std::string> const& _freeFunctionById
)
	: typeMapper(_typeMapper),
	  storageMapper(_storageMapper),
	  sourceFile(_sourceFile),
	  contractName(_contractName),
	  libraryFunctionIds(_libraryFunctionIds),
	  overloadedNames(_overloadedNames),
	  freeFunctionById(_freeFunctionById),
	  registry(std::make_unique<BuilderRegistry>())
{
	// Wire callbacks. Each captures `this` — BuilderContext is non-movable
	// and non-copyable, so the captured pointer remains stable.
	buildExpr = [this](solidity::frontend::Expression const& _expr) {
		return this->build(_expr);
	};
	buildBinaryOp = [this](solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> _left,
		std::shared_ptr<awst::Expression> _right,
		awst::WType const* _resultType,
		awst::SourceLocation const& _loc) {
		return eb::buildBinaryOp(*this, _op, std::move(_left), std::move(_right), _resultType, _loc);
	};
	builderForInstance = [this](solidity::frontend::Type const* _solType, std::shared_ptr<awst::Expression> _expr) {
		return registry->tryBuildInstance(*this, _solType, std::move(_expr));
	};
}

BuilderContext::~BuilderContext() = default;

std::shared_ptr<awst::Expression> BuilderContext::build(
	solidity::frontend::Expression const& _expr)
{
	return sol_ast::buildExpression(*this, _expr);
}

std::vector<std::shared_ptr<awst::Statement>> BuilderContext::takePending()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.swap(pendingStatements);
	return result;
}

std::vector<std::shared_ptr<awst::Statement>> BuilderContext::takePrePending()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.swap(prePendingStatements);
	return result;
}

} // namespace puyasol::builder::eb
