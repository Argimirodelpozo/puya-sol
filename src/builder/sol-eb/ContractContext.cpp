#include "builder/sol-eb/ContractContext.h"

#include <cassert>

#include "awst/NameGen.h"
#include "builder/sol-ast/Context.h"
#include "builder/sol-ast/SolExpressionDispatch.h"
#include "builder/sol-eb/BinaryOpBuilder.h"
#include "builder/sol-eb/BuilderRegistry.h"

namespace puyasol::builder::eb
{

// Scope-bound accessor bridges moved to sol_ast::Context (Context.cpp);
// no callers remained here after visitors/builders switched to m_scope directly.

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
	// Capture `this` in callbacks — ContractContext is non-movable/non-copyable.
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

std::shared_ptr<awst::Expression> ContractContext::emitSequencedOperand(
	OperandDeltas&& _d,
	std::shared_ptr<awst::Expression> _value,
	bool _pin,
	awst::SourceLocation const& _loc)
{
	for (auto& s: _d.pre)
		prePendingStatements.push_back(std::move(s));
	bool isConstant = !_value
		|| dynamic_cast<awst::IntegerConstant const*>(_value.get())
		|| dynamic_cast<awst::BoolConstant const*>(_value.get())
		|| dynamic_cast<awst::BytesConstant const*>(_value.get())
		|| dynamic_cast<awst::StringConstant const*>(_value.get())
		|| dynamic_cast<awst::VoidConstant const*>(_value.get());
	if (_pin && !isConstant && _value->wtype
		&& _value->wtype != awst::WType::voidType())
	{
		auto var = awst::makeVarExpression(
			"__seq_" + std::to_string(awst::NameGen::next("ContractContext.seqCounter")),
			_value->wtype, _loc);
		prePendingStatements.push_back(
			awst::makeAssignmentStatement(var, std::move(_value), _loc));
		_value = var;
	}
	for (auto& s: _d.post)
		prePendingStatements.push_back(std::move(s));
	return _value;
}

} // namespace puyasol::builder::eb
