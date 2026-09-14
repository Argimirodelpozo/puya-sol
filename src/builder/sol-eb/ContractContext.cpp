#include "builder/sol-eb/ContractContext.h"

#include <cassert>

#include "awst/NameGen.h"
#include "builder/sol-ast/Context.h"
#include "builder/sol-ast/SolExpressionDispatch.h"
#include "builder/sol-eb/BinaryOpBuilder.h"
#include "builder/sol-eb/SolIntegerBuilder.h"
#include "builder/sol-eb/SolBoolBuilder.h"
#include "builder/sol-eb/SolAddressBuilder.h"
#include "builder/sol-eb/SolArrayBuilder.h"
#include "builder/sol-eb/SolStructBuilder.h"
#include "builder/sol-eb/SolEnumBuilder.h"
#include "builder/sol-eb/SolFixedBytesBuilder.h"
#include "builder/storage/StorageMapper.h"
// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

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
	  functionPointers(_functionPointers)
{
	viaIRSequencing = typeMapper.profile().viaIRSequencing;
}

ContractContext::~ContractContext() = default;

sol_ast::Context& ContractContext::scope() const
{
	assert(currentScope && "expression lowering requires an active scope");
	return *currentScope;
}

awst::SourceLocation ContractContext::makeLoc(int _start, int _end) const
{
	return typeMapper.sourceMap().toAwstLoc(sourceFile, _start, _end);
}

awst::SourceLocation ContractContext::makeLoc(solidity::langutil::SourceLocation const& _location) const
{
	return typeMapper.sourceMap().toAwstLoc(sourceFile, _location);
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
			typeMapper, std::move(value), preEffects(), makeLoc(_expr.location()));
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
	using namespace solidity::frontend;
	if (!_solType) return nullptr;
	switch (_solType->category())
	{
	case Type::Category::Integer:
		return std::make_unique<SolIntegerBuilder>(*this, static_cast<IntegerType const*>(_solType), std::move(_expr));
	case Type::Category::Bool:
		return std::make_unique<SolBoolBuilder>(*this, std::move(_expr));
	case Type::Category::Address:
		return std::make_unique<SolAddressBuilder>(*this, _solType, std::move(_expr));
	case Type::Category::Enum:
		return std::make_unique<SolEnumBuilder>(*this, static_cast<EnumType const*>(_solType), std::move(_expr));
	case Type::Category::FixedBytes:
		return std::make_unique<SolFixedBytesBuilder>(*this, static_cast<FixedBytesType const*>(_solType), std::move(_expr));
	case Type::Category::Array:
		return std::make_unique<SolArrayBuilder>(*this, static_cast<ArrayType const*>(_solType), std::move(_expr));
	case Type::Category::Struct:
		return std::make_unique<SolStructBuilder>(*this, static_cast<StructType const*>(_solType), std::move(_expr));
	default: return nullptr;
	}
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

std::shared_ptr<awst::Expression> ContractContext::emitConditional(
	std::shared_ptr<awst::Expression> _condition,
	LoweredValue<std::shared_ptr<awst::Expression>> _true,
	LoweredValue<std::shared_ptr<awst::Expression>> _false,
	awst::WType const* _type,
	awst::SourceLocation const& _loc)
{
	if (_true.effects.empty() && _false.effects.empty())
		return awst::makeConditional(std::move(_condition), std::move(_true.value),
			std::move(_false.value), _type, _loc);
	auto result = awst::makeVarExpression("__cond_"
		+ std::to_string(awst::NameGen::next("ContractContext.conditional")), _type, _loc);
	auto block = [&](auto branch) {
		if (_type != awst::WType::voidType())
			return makeScopedResultBlock(std::move(branch.effects.pre), result,
				std::move(branch.value), _loc, std::move(branch.effects.post));
		auto out = awst::makeBlock(_loc);
		out->body = std::move(branch.effects.pre);
		out->body.push_back(awst::makeExpressionStatement(std::move(branch.value), _loc));
		for (auto& stmt: branch.effects.post) out->body.push_back(std::move(stmt));
		return out;
	};
	preEffects().push_back(awst::makeIfElse(std::move(_condition),
		block(std::move(_true)), block(std::move(_false)), _loc));
	if (_type == awst::WType::voidType()) return awst::makeVoidConstant(_loc);
	return result;
}

} // namespace puyasol::builder::eb
