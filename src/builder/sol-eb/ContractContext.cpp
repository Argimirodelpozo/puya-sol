#include "builder/sol-eb/ContractContext.h"

#include <cassert>

#include "builder/sol-ast/Context.h"
#include "builder/sol-ast/SolExpressionDispatch.h"
#include "builder/sol-eb/BinaryOpBuilder.h"
#include "builder/sol-eb/BuilderRegistry.h"

namespace puyasol::builder::eb
{

// All scope-bound state accessor implementations have moved onto
// `sol_ast::Context` itself (see Context.cpp). Bridges previously
// defined here delegated through `currentScope`; with every visitor +
// builder now reading via `m_scope` and every helper writing via the
// nearest enclosing `TranslationContext` / `FunctionContext` /
// `BlockContext`, those bridges had no callers and were deleted.

ContractContext::ContractContext(
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
	// Wire callbacks. Each captures `this` — ContractContext is non-movable
	// and non-copyable, so the captured pointer remains stable.
	buildExpr = [this](solidity::frontend::Expression const& _expr) {
		return this->build(_expr);
	};
	buildBinaryOp = [this](solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> _left,
		std::shared_ptr<awst::Expression> _right,
		awst::WType const* _resultType,
		awst::SourceLocation const& _loc) {
		assert(currentScope && "buildBinaryOp called with no current scope");
		return eb::buildBinaryOp(*this, *currentScope, _op,
			std::move(_left), std::move(_right), _resultType, _loc);
	};
	builderForInstance = [this](solidity::frontend::Type const* _solType, std::shared_ptr<awst::Expression> _expr) {
		return registry->tryBuildInstance(*this, _solType, std::move(_expr));
	};
}

ContractContext::~ContractContext() = default;

std::shared_ptr<awst::Expression> ContractContext::build(
	solidity::frontend::Expression const& _expr)
{
	return sol_ast::buildExpression(*this, _expr);
}

std::vector<std::shared_ptr<awst::Statement>> ContractContext::takePending()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.swap(pendingStatements);
	return result;
}

std::vector<std::shared_ptr<awst::Statement>> ContractContext::takePrePending()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.swap(prePendingStatements);
	return result;
}

void ContractContext::appendPendingTo(
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	for (auto& p: takePrePending())
		_out.push_back(std::move(p));
	for (auto& p: takePending())
		_out.push_back(std::move(p));
}

} // namespace puyasol::builder::eb
