#include "builder/lowering/abi/AbiSelectorCalldataBuilder.h"
#include "builder/lowering/abi/AbiEncoderBuilder.h"
#include "builder/eb/CallOperands.h"
#include "builder/ast/members/SolSelectorAccess.h"
#include "builder/solc/SolcFacts.h"
#include "builder/types/ConversionPlan.h"
#include "builder/types/TypeMapper.h"

#include <libsolidity/ast/TypeProvider.h>
#include <stdexcept>

namespace puyasol::builder::eb
{
using namespace solidity::frontend;

AbiCall::AbiCall(FunctionCall const& call)
{
	if (call.arguments().size() != 2) throw std::logic_error("Invalid solc encodeCall arity");
	target = call.arguments()[0].get();
	auto const* function = dynamic_cast<FunctionType const*>(target->annotation().type);
	type = function ? function->asExternallyCallableFunction(false) : nullptr;
	if (!type) throw std::logic_error("encodeCall target has no externally callable solc type");
	// Inline arrays also use TupleExpression; only solc's TupleType denotes
	// multiple call arguments. Match TypeChecker::typeCheckABIEncodeCallFunction.
	if (dynamic_cast<TupleType const*>(call.arguments()[1]->annotation().type))
	{
		auto const* tuple = SolcFacts::expressionAs<TupleExpression>(call.arguments()[1].get());
		if (!tuple) throw std::logic_error("encodeCall tuple is not inline");
		arguments.assign(tuple->components().begin(), tuple->components().end());
	}
	else arguments.push_back(call.arguments()[1]);
	if (arguments.size() != type->parameterTypes().size())
		throw std::logic_error("encodeCall arguments disagree with solc parameter types");
	for (auto const& argument: arguments)
		if (!argument) throw std::logic_error("Missing encodeCall argument");
}

std::shared_ptr<awst::Expression> handleEncodeCall(
	ContractContext& ctx, FunctionCall const& call, awst::SourceLocation const& loc)
{
	AbiCall facts(call);
	// Fold the selector independently of evaluating the receiver for effects.
	auto selector = sol_ast::SolSelectorAccess::selectorOf(
		ctx, *facts.target, awst::WType::bytesType(), loc, true);
	auto const& paramTypes = facts.type->parameterTypes();
	auto const& arguments = facts.arguments;
	std::vector<std::shared_ptr<awst::Expression>> values;
	for (size_t i = 0; i < arguments.size(); ++i)
	{
		auto value = sol_ast::CallOperands::evaluate(ctx, *arguments[i], loc);
		values.push_back(ConversionPlan{arguments[i]->annotation().type, paramTypes[i],
			ctx.typeMapper.map(paramTypes[i]), ConversionPlan::Context::AbiArgument}.emit(std::move(value), loc));
	}
	return awst::makeConcat(std::move(selector),
		AbiEncoderBuilder::encodeValuesAsEvmAbi(ctx, paramTypes, std::move(values), loc), loc);
}

std::shared_ptr<awst::Expression> handleEncodeWithSelector(
	ContractContext& ctx, FunctionCall const& call, awst::SourceLocation const& loc)
{
	auto const& args = call.arguments();
	if (args.empty()) throw std::logic_error("Missing ABI selector");
	auto const* type = TypeProvider::fixedBytes(4);
	auto selector = ConversionPlan{args[0]->annotation().type, type, ctx.typeMapper.map(type),
		ConversionPlan::Context::AbiArgument}.emit(
			sol_ast::CallOperands::evaluate(ctx, *args[0], loc), loc);
	return awst::makeConcat(std::move(selector),
		AbiEncoderBuilder::encodeArgsAsEvmAbi(ctx, args, 1, loc), loc);
}

std::shared_ptr<awst::Expression> handleEncodeWithSignature(
	ContractContext& ctx, FunctionCall const& call, awst::SourceLocation const& loc)
{
	auto const& args = call.arguments();
	if (args.empty()) throw std::logic_error("Missing ABI signature");
	std::shared_ptr<awst::Expression> selector;
	if (auto const* literal = SolcFacts::expressionAs<Literal>(args[0].get()))
		selector = awst::makeBytesConstant(SolcFacts::externalSelector(literal->value()),
			loc, awst::BytesEncoding::Base16, awst::WType::bytesType());
	else
	{
		auto hash = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), loc);
		hash->stackArgs.push_back(sol_ast::CallOperands::evaluate(ctx, *args[0], loc));
		selector = awst::makeExtract(std::move(hash), 0, 4, loc);
	}
	return awst::makeConcat(std::move(selector),
		AbiEncoderBuilder::encodeArgsAsEvmAbi(ctx, args, 1, loc), loc);
}
} // namespace puyasol::builder::eb
