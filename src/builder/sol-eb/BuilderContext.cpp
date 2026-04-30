#include "builder/sol-eb/BuilderContext.h"

#include "builder/sol-ast/SolExpressionDispatch.h"
#include "builder/sol-eb/BinaryOpBuilder.h"
#include "builder/sol-eb/BuilderRegistry.h"

namespace puyasol::builder::eb
{

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
