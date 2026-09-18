/// @file CallResolver.cpp
/// Resolves function call targets from Solidity AST nodes.

#include "builder/lowering/calls/CallResolver.h"
#include "builder/solc/FunctionIdentity.h"
#include "builder/solc/SolcFacts.h"
#include "builder/solc/OverloadSuffix.h"
#include "builder/types/ConversionPlan.h"
#include "builder/types/TypeMapper.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

std::shared_ptr<awst::Expression> CallResolver::buildOperatorCall(
	ContractContext& _ctx,
	solidity::frontend::FunctionDefinition const& _function,
	std::initializer_list<solidity::frontend::Expression const*> _operands,
	awst::SourceLocation const& _loc)
{
	ResolvedCall resolved;
	if (!tryResolveLibraryOrFree(_ctx, &_function, resolved))
	{
		Logger::instance().error("unresolved user-defined operator function", _loc);
		return nullptr;
	}
	auto call = awst::makeSubroutineCall(
		std::move(resolved.target),
		_ctx.typeMapper.functionReturnPlan(_function).nativeType, _loc);
	size_t index = 0;
	for (auto const* operand: _operands)
	{
		auto const& parameter = *_function.parameters().at(index);
		auto lowered = _ctx.lowerOperand([&]() {
			return builder::ConversionPlan{
				operand->annotation().type, parameter.type(),
				_ctx.typeMapper.map(parameter.type()),
				builder::ConversionPlan::Context::Argument}.emit(
					_ctx.buildExpr(*operand), _loc);
		}, false);
		// Capture an earlier operand before lowering the next one's effects.
		// Operators only take value types, so this cannot break reference aliases.
		auto value = _ctx.emitSequencedOperand(std::move(lowered.effects),
			std::move(lowered.value), index + 1 < _operands.size(), _loc);
		awst::pushCallArg(call->args,
			parameter.name().empty() ? "_param" + std::to_string(index) : parameter.name(),
			std::move(value));
		++index;
	}
	return call;
}

CallPlan CallResolver::plan(solidity::frontend::FunctionCall const& _call)
{
	using namespace solidity::frontend;
	CallPlan result;
	result.functionType = dynamic_cast<FunctionType const*>(
		_call.expression().annotation().type);
	result.callee = &SolcFacts::functionExpression(_call.expression());
	if (!result.functionType)
		return result;

	auto kind = result.functionType->kind();
	result.transport = (kind == FunctionType::Kind::External
		|| kind == FunctionType::Kind::DelegateCall)
		? CallTransport::External : CallTransport::Internal;

	if (auto const* member = SolcFacts::expressionAs<MemberAccess>(result.callee))
	{
		result.declaration = dynamic_cast<FunctionDefinition const*>(
			member->annotation().referencedDeclaration);
		if (result.declaration && result.declaration->annotation().contract
			&& result.declaration->annotation().contract->isLibrary())
			result.transport = CallTransport::Internal;

		if (SolcFacts::isThis(member->expression()))
		{
			result.isSelfCall = true;
			result.transport = CallTransport::Internal;
		}
		// Type-level inherited state pointers and function-valued struct fields
		// are data. Contract-instance members are getters/methods instead.
		auto const* baseType = member->expression().annotation().type;
		if (dynamic_cast<TypeType const*>(baseType) || dynamic_cast<StructType const*>(baseType))
			if (auto const* variable = dynamic_cast<VariableDeclaration const*>(
					member->annotation().referencedDeclaration);
				variable && dynamic_cast<FunctionType const*>(variable->type()))
			{
				result.isFunctionPointer = true;
				result.transport = CallTransport::Internal;
			}
	}
	else if (auto const* identifier = SolcFacts::expressionAs<Identifier>(result.callee))
	{
		if (auto const* variable = dynamic_cast<VariableDeclaration const*>(
				identifier->annotation().referencedDeclaration);
			variable && dynamic_cast<FunctionType const*>(variable->type()))
		{
			result.isFunctionPointer = true;
			result.transport = CallTransport::Internal;
		}
	}
	else if (kind == FunctionType::Kind::Internal || kind == FunctionType::Kind::External)
	{
		result.isFunctionPointer = true;
		result.transport = CallTransport::Internal;
	}

	return result;
}

std::string CallResolver::resolveMethodName(
	ContractContext& _ctx,
	solidity::frontend::FunctionDefinition const& _func)
{
	using solidity::frontend::Visibility;
	if (_func.visibility() == Visibility::Internal
		|| _func.visibility() == Visibility::Private)
		if (auto symbol = functionSymbol(_func))
			return *symbol;
	std::string name = _func.name();
	if (_ctx.overloadedNames.count(name))
		appendOverloadSuffix(name, _func);
	return name;
}

std::string CallResolver::baseImplementationName(
	ContractContext& _ctx,
	solidity::frontend::FunctionDefinition const& _func)
{
	solAssert(_func.isImplemented(), "Unimplemented solc base target");
	if (_ctx.baseImplementationIds.insert(_func.id()).second)
		_ctx.pendingBaseImplementations.push_back(&_func);
	return _func.name() + "__impl_" + std::to_string(_func.id());
}

bool CallResolver::tryResolveLibraryOrFree(
	ContractContext& _ctx,
	solidity::frontend::FunctionDefinition const* _funcDef,
	ResolvedCall& _result)
{
	if (!_funcDef)
		return false;
	// Some free/library functions need a concrete host contract. They are
	// emitted as instance methods and keyed by solc declaration identity.
	auto const hostBound = _ctx.internalizedFunctionNames.find(_funcDef->id());
	if (hostBound != _ctx.internalizedFunctionNames.end())
	{
		_result.target = awst::InstanceMethodTarget{hostBound->second};
		_result.funcDef = _funcDef;
		return true;
	}

	auto const* contractDef = _funcDef->annotation().contract;
	bool const isLibrary = contractDef && contractDef->isLibrary();
	if (isLibrary || _funcDef->isFree())
	{
		if (auto symbol = functionSymbol(*_funcDef))
		{
			_result.target = awst::SubroutineID{*symbol};
			_result.funcDef = _funcDef;
			return true;
		}
	}

	return false;
}

std::optional<ResolvedCall> CallResolver::resolveFunction(
	ContractContext& _ctx,
	solidity::frontend::Expression const& _expression)
{
	using namespace solidity::frontend;
	auto const& expression = SolcFacts::functionExpression(_expression);
	auto const* function = SolcFacts::resolveFunction(expression, _ctx.currentContract);
	if (!function)
		return std::nullopt;

	ResolvedCall result;
	result.funcDef = function;
	if (tryResolveLibraryOrFree(_ctx, function, result))
	{
		auto const* type = dynamic_cast<FunctionType const*>(expression.annotation().type);
		result.isUsingForCall = type && type->hasBoundFirstArgument();
		return result;
	}

	// Type-level contract members are super.f or explicit Base.f. The solc
	// declaration is already concrete, so give it an internal copy even if its
	// public entrypoint is overridden or carries ABI-boundary checks.
	if (auto const* member = SolcFacts::expressionAs<MemberAccess>(&expression))
	{
		if (auto const* type = dynamic_cast<TypeType const*>(member->expression().annotation().type);
			type && dynamic_cast<ContractType const*>(type->actualType()))
		{
			result.target = awst::InstanceMethodTarget{baseImplementationName(_ctx, *function)};
			return result;
		}
		// This backend internalizes self calls. Their ABI selector denotes the
		// host's override, unlike the static source declaration on `this.f`.
		if (_ctx.currentContract && SolcFacts::isThis(member->expression()))
		{
			// External overrides may change calldata to memory. Internal virtual
			// lookup asserts equal source parameter types; solc's ABI table owns
			// the external target, including that legal data-location change.
			auto signature = function->externalSignature();
			for (auto const& [_, entry]: _ctx.currentContract->interfaceFunctionList(true))
				if (entry->externalSignature() == signature)
					if (auto const* implementation = dynamic_cast<FunctionDefinition const*>(&entry->declaration()))
						result.funcDef = function = implementation;
		}
	}
	result.target = awst::InstanceMethodTarget{resolveMethodName(_ctx, *function)};
	return result;
}

} // namespace puyasol::builder::eb
