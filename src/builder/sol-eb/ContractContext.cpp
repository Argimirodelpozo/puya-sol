#include "builder/sol-eb/ContractContext.h"

#include <cassert>

#include "awst/NameGen.h"
#include "builder/sol-ast/Context.h"
#include "builder/sol-ast/SolExpressionDispatch.h"
#include "builder/sol-eb/BinaryOpBuilder.h"
#include "builder/sol-eb/BuilderRegistry.h"
#include "builder/storage/StorageMapper.h"
// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

// Scope-bound accessor bridges moved to sol_ast::Context (Context.cpp);
// no callers remained here after visitors/builders switched to m_scope directly.

ContractContext::ContractContext(
	TypeMapper& _typeMapper,
	StorageMapper& _storageMapper,
	std::string const& _sourceFile,
	std::string const& _contractName,
	std::unordered_set<std::string> const& _overloadedNames,
	FunctionSymbolTable const& _functionSymbols,
	FunctionPointerRegistry& _functionPointers
)
	: typeMapper(_typeMapper),
	  storageMapper(_storageMapper),
	  sourceFile(_sourceFile),
	  contractName(_contractName),
	  overloadedNames(_overloadedNames),
	  functionSymbols(_functionSymbols),
	  functionPointers(_functionPointers),
	  registry(std::make_unique<BuilderRegistry>())
{}

ContractContext::~ContractContext() = default;

awst::SourceLocation ContractContext::makeLoc(int _start, int _end) const
{
	return typeMapper.sourceMap().toAwstLoc(sourceFile, _start, _end);
}

std::shared_ptr<awst::Expression> ContractContext::buildValue(
	solidity::frontend::Expression const& _expr)
{
	auto value = sol_ast::buildExpression(*this, _expr);
	// Aggregate storage expressions still denote places (including aliases).
	// Only a scalar rvalue is necessarily a bounded read at this boundary.
	if (!_expr.annotation().willBeWrittenTo && _expr.annotation().type
		&& _expr.annotation().type->isValueType() && value)
		value = StorageMapper::makePartialBoxReadWithDefault(
			typeMapper, std::move(value), preEffects(), makeLoc(
				_expr.location().start, _expr.location().end));
	return value;
}

ContractContext::LoweredExpression ContractContext::build(
	solidity::frontend::Expression const& _expr,
	bool _conditional)
{
	auto result = lowerOperand([&] { return buildValue(_expr); }, _conditional);
	return {std::move(result.value), std::move(result.effects),
		_expr.annotation().type};
}

std::shared_ptr<awst::Expression> ContractContext::buildExpr(
	solidity::frontend::Expression const& _expr)
{
	auto lowered = build(_expr, false);
	restoreOperandDeltas(std::move(lowered.effects));
	return std::move(lowered.value);
}

void ContractContext::evaluateForEffects(
	solidity::frontend::Expression const& _expr,
	awst::SourceLocation const& _loc)
{
	auto lowered = build(_expr, false);
	for (auto& statement: lowered.effects.pre)
		preEffects().push_back(std::move(statement));
	if (lowered.value)
		preEffects().push_back(
			awst::makeExpressionStatement(std::move(lowered.value), _loc));
	for (auto& statement: lowered.effects.post)
		preEffects().push_back(std::move(statement));
}

std::shared_ptr<awst::Expression> ContractContext::buildBinaryOp(
	solidity::frontend::Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::WType const* _resultType,
	awst::SourceLocation const& _loc)
{
	assert(currentScope && "buildBinaryOp called with no current scope");
	return eb::buildBinaryOp(*this, *currentScope, _op,
		std::move(_left), std::move(_right), _resultType, _loc);
}

std::unique_ptr<InstanceBuilder> ContractContext::builderForInstance(
	solidity::frontend::Type const* _solType,
	std::shared_ptr<awst::Expression> _expr)
{
	return registry->tryBuildInstance(*this, _solType, std::move(_expr));
}

std::vector<std::shared_ptr<awst::Statement>> ContractContext::takePostEffects()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.swap(activeEffects().post);
	return result;
}

std::vector<std::shared_ptr<awst::Statement>> ContractContext::takePreEffects()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.swap(activeEffects().pre);
	return result;
}

void ContractContext::appendEffectsTo(
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	for (auto& p: takePreEffects())
		_out.push_back(std::move(p));
	for (auto& p: takePostEffects())
		_out.push_back(std::move(p));
}

std::shared_ptr<awst::Expression> ContractContext::emitSequencedOperand(
	OperandDeltas&& _d,
	std::shared_ptr<awst::Expression> _value,
	bool _pin,
	awst::SourceLocation const& _loc)
{
	for (auto& s: _d.pre)
		preEffects().push_back(std::move(s));
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
		preEffects().push_back(
			awst::makeAssignmentStatement(var, std::move(_value), _loc));
		_value = var;
	}
	for (auto& s: _d.post)
		preEffects().push_back(std::move(s));
	return _value;
}

} // namespace puyasol::builder::eb
