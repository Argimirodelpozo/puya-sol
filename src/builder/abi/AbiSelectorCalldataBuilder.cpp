#include "builder/abi/AbiSelectorCalldataBuilder.h"
#include "builder/abi/AbiEncoderBuilder.h"
#include "builder/sol-ast/CallOperands.h"
#include "builder/sol-ast/members/SolSelectorAccess.h"
#include "builder/SolcFacts.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/TypeProvider.h>
#include <stdexcept>

namespace puyasol::builder::eb
{
using namespace solidity::frontend;

std::shared_ptr<awst::Expression> handleEncodeCall(
	ContractContext& ctx, FunctionCall const& call, awst::SourceLocation const& loc)
{
	if (call.arguments().size() != 2) throw std::logic_error("Invalid solc encodeCall arity");
	auto const& source = *call.arguments()[0];
	auto const* function = dynamic_cast<FunctionType const*>(source.annotation().type);
	auto const* external = function ? function->asExternallyCallableFunction(false) : nullptr;
	if (!external) throw std::logic_error("encodeCall target has no externally callable solc type");
	// Selector folding and receiver evaluation are independent. Reuse the
	// selector projection's scoped effects instead of constructing a compact
	// application pointer merely to discard its address.
	auto selector = sol_ast::SolSelectorAccess::selectorOf(
		ctx, source, awst::WType::bytesType(), loc, true);
	auto const paramTypes = external->parameterTypes();
	std::vector<ASTPointer<Expression const>> arguments;
	// Inline arrays also use TupleExpression; only solc's TupleType denotes
	// multiple call arguments. Match TypeChecker::typeCheckABIEncodeCallFunction.
	if (dynamic_cast<TupleType const*>(call.arguments()[1]->annotation().type))
	{
		auto const* tuple = dynamic_cast<TupleExpression const*>(call.arguments()[1].get());
		if (!tuple) throw std::logic_error("encodeCall tuple is not inline");
		arguments.assign(tuple->components().begin(), tuple->components().end());
	}
	else arguments.push_back(call.arguments()[1]);
	if (arguments.size() != paramTypes.size())
		throw std::logic_error("encodeCall arguments disagree with solc parameter types");
	std::vector<std::shared_ptr<awst::Expression>> values;
	for (size_t i = 0; i < arguments.size(); ++i)
	{
		if (!arguments[i]) throw std::logic_error("Missing encodeCall argument");
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
	if (auto const* literal = dynamic_cast<Literal const*>(args[0].get()))
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
