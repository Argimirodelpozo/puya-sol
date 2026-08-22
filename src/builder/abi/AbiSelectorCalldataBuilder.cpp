#include "builder/abi/AbiSelectorCalldataBuilder.h"
#include "Logger.h"
#include "builder/abi/AbiEncoderBuilder.h"
#include "builder/SelectorSemantics.h"
#include "builder/SolcFacts.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/itxn/InnerCallHandlers.h"

namespace puyasol::builder::eb
{

// ── encodeCall ──

std::unique_ptr<InstanceBuilder> handleEncodeCall(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	if (_callNode.arguments().size() < 2)
		return nullptr;

	auto const& targetFnExpr = *_callNode.arguments()[0];
	FunctionDefinition const* targetFuncDef = nullptr;
	FunctionType const* fnType = dynamic_cast<FunctionType const*>(targetFnExpr.annotation().type);
	if (fnType && fnType->hasDeclaration())
		targetFuncDef = dynamic_cast<FunctionDefinition const*>(&fnType->declaration());

	// Compile-time selector when we have a function definition; otherwise
	// runtime-extract the Solidity-visible selector from the fn-ptr value. It is
	// always bytes 8..12; the flagged 16-byte layout appends an ARC-4 route.
	std::shared_ptr<awst::Expression> selector;
	if (targetFuncDef)
	{
		auto const* externalType = fnType
			? fnType : targetFuncDef->functionType(false);
		if (!externalType)
			return nullptr;
		// PROFILE-AWARE, not unconditionally EVM. The selector redesign is
		// opt-in: without --evm-selectors abi.encodeCall keeps its ARC-4
		// identity (sha512_256 over name(params)returns), which is what
		// puya-sol callees actually route on. Hard-coding the keccak selector
		// here made every default-profile encodeCall emit an EVM selector
		// (test_evm_selectors_default_compatibility).
		selector = builder::SelectorSemantics::functionSelector(
			_ctx, *externalType,
			InnerCallHandlers::buildMethodSelector(_ctx, targetFuncDef), _loc);
	}
	else if (fnType && fnType->kind() == FunctionType::Kind::External)
	{
		if (!_ctx.typeMapper.profile().evmSelectors)
		{
			Logger::instance().error(
				"abi.encodeCall with an opaque runtime external-function pointer "
				"requires --evm-selectors (the default compact pointer stores only "
				"its ARC4 route, not the Solidity selector)", _loc);
			return nullptr;
		}
		auto fnVal = _ctx.buildExpr(targetFnExpr);
		if (!fnVal)
			return nullptr;
		// Coerce the profile-sized external fn-ptr value to plain bytes.
		if (fnVal->wtype && fnVal->wtype->kind() == awst::WTypeKind::Bytes)
			fnVal = awst::makeAsBytes(std::move(fnVal), _loc);
		// Extract the public Solidity selector field at bytes 8..12.
		selector = awst::makeExtract(std::move(fnVal), 8, 4, _loc);
	}
	else
	{
		// Unsupported function-pointer kind (internal, library, etc.).
		return nullptr;
	}

	std::vector<std::shared_ptr<awst::Expression>> parts;
	parts.push_back(std::move(selector));

	auto const& argsExpr = *_callNode.arguments()[1];
	std::vector<ASTPointer<Expression const>> callArgs;
	if (auto const* tupleExpr = dynamic_cast<TupleExpression const*>(&argsExpr))
	{
		for (auto const& comp : tupleExpr->components())
			if (comp) callArgs.push_back(comp);
	}
	else
		callArgs.push_back(_callNode.arguments()[1]);

	// Encode each argument at the callee's declared Solidity type. This is
	// canonical EVM calldata regardless of the contract entry profile.
	std::vector<solidity::frontend::Type const*> paramTypes;
	if (targetFuncDef)
	{
		for (auto const& p : targetFuncDef->parameters())
			paramTypes.push_back(p ? p->type() : nullptr);
	}
	else if (fnType)
	{
		for (auto const* pt : fnType->parameterTypes())
			paramTypes.push_back(pt);
	}

	std::vector<std::shared_ptr<awst::Expression>> values;
	for (size_t i = 0; i < callArgs.size(); ++i)
	{
		auto value = _ctx.buildExpr(*callArgs[i]);
		if (i < paramTypes.size() && paramTypes[i])
			if (auto const* target = _ctx.typeMapper.map(paramTypes[i]))
				value = builder::ConversionPlan{
					callArgs[i]->annotation().type, paramTypes[i], target,
					builder::ConversionPlan::Context::AbiArgument}.emit(
						std::move(value), _loc);
		values.push_back(std::move(value));
	}
	parts.push_back(AbiEncoderBuilder::encodeValuesAsEvmAbi(
		_ctx, paramTypes, std::move(values), _loc));

	return std::make_unique<GenericAbiResult>(_ctx, AbiEncoderBuilder::concatByteExprs(std::move(parts), _loc));
}

// ── encodeWithSelector ──

std::unique_ptr<InstanceBuilder> handleEncodeWithSelector(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	auto const& args = _callNode.arguments();
	if (args.empty()) return nullptr;

	// selector (4 bytes) + abi.encode(remaining args). Solidity types the selector
	// bytes4, but buildExpr may hand back uint64/biguint for literals — coerce to 4B.
	auto selector = _ctx.buildExpr(*args[0]);
	auto const* selType = args[0]->annotation().type;
	bool selIsBytesN = false;
	if (auto const* fb = dynamic_cast<solidity::frontend::FixedBytesType const*>(selType))
		selIsBytesN = fb->numBytes() == 4;
	if (!selIsBytesN)
	{
		// Integer/biguint → itob → take last 4 bytes (big-endian, so the
		// low-order 4 bytes hold the selector value).
		std::shared_ptr<awst::Expression> asBytes = selector;
		if (selector->wtype == awst::WType::uint64Type())
		{
			asBytes = awst::makeItob(std::move(selector), _loc);
		}
		else if (selector->wtype == awst::WType::biguintType())
		{
			auto cast = awst::makeAsBytes(std::move(selector), _loc);
			asBytes = std::move(cast);
		}

		// Left-pad to ≥4B, take the last 4. makeExtractLastN wraps its input in
		// a SingleEvaluation, so a side-effecting selector evaluates once.
		selector = awst::makeExtractLastN(
			awst::makeLeftPad(std::move(asBytes), 4, _loc), 4, _loc);
	}

	if (args.size() == 1)
		return std::make_unique<GenericAbiResult>(_ctx, std::move(selector));

	std::vector<std::shared_ptr<awst::Expression>> parts;
	parts.push_back(std::move(selector));
	std::vector<solidity::frontend::Type const*> types;
	std::vector<std::shared_ptr<awst::Expression>> values;
	for (size_t i = 1; i < args.size(); ++i)
	{
		auto const* sourceType = args[i]->annotation().type;
		auto const* type = sourceType;
		if (type)
			if (auto const* mobile = type->mobileType())
				type = mobile;
		types.push_back(type);
		auto value = _ctx.buildExpr(*args[i]);
		if (type)
			if (auto const* target = _ctx.typeMapper.map(type);
				target && value->wtype != target)
				value = builder::ConversionPlan{
					sourceType, type, target,
					builder::ConversionPlan::Context::AbiArgument}.emit(
						std::move(value), _loc);
		values.push_back(std::move(value));
	}
	parts.push_back(AbiEncoderBuilder::encodeValuesAsEvmAbi(
		_ctx, types, std::move(values), _loc));
	return std::make_unique<GenericAbiResult>(_ctx, AbiEncoderBuilder::concatByteExprs(std::move(parts), _loc));
}

// ── encodeWithSignature ──

std::unique_ptr<InstanceBuilder> handleEncodeWithSignature(
	ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	auto const& args = _callNode.arguments();
	if (args.empty()) return nullptr;

	std::vector<std::shared_ptr<awst::Expression>> parts;

	// PROFILE-AWARE. Solidity fixes this to keccak256(signature)[:4] and the
	// EVM profile honours that, but the default profile keeps the ARC-4
	// identity a puya-sol callee actually routes on. Making it unconditionally
	// keccak silently changed every existing abi.encodeWithSignature call in
	// the default profile (test_evm_selectors_default_compatibility, which
	// states the redesign is opt-in and byte-for-byte compatible without
	// --evm-selectors).
	if (auto const* sigLit = dynamic_cast<solidity::frontend::Literal const*>(args[0].get()))
	{
		parts.push_back(builder::SelectorSemantics::signatureSelector(
			_ctx, sigLit->value(), _loc));
	}
	else
	{
		auto sigExpr = _ctx.buildExpr(*args[0]);
		auto hash = awst::makeIntrinsicCall(
			"keccak256", awst::WType::bytesType(), _loc);
		hash->stackArgs.push_back(std::move(sigExpr));
		parts.push_back(awst::makeExtract(std::move(hash), 0, 4, _loc));
	}

	if (args.size() == 1)
		return std::make_unique<GenericAbiResult>(_ctx, AbiEncoderBuilder::concatByteExprs(std::move(parts), _loc));

	std::vector<solidity::frontend::Type const*> types;
	std::vector<std::shared_ptr<awst::Expression>> values;
	for (size_t i = 1; i < args.size(); ++i)
	{
		auto const* sourceType = args[i]->annotation().type;
		auto const* type = sourceType;
		if (type)
			if (auto const* mobile = type->mobileType())
				type = mobile;
		types.push_back(type);
		auto value = _ctx.buildExpr(*args[i]);
		if (type)
			if (auto const* target = _ctx.typeMapper.map(type);
				target && value->wtype != target)
				value = builder::ConversionPlan{
					sourceType, type, target,
					builder::ConversionPlan::Context::AbiArgument}.emit(
						std::move(value), _loc);
		values.push_back(std::move(value));
	}
	parts.push_back(AbiEncoderBuilder::encodeValuesAsEvmAbi(
		_ctx, types, std::move(values), _loc));
	return std::make_unique<GenericAbiResult>(_ctx, AbiEncoderBuilder::concatByteExprs(std::move(parts), _loc));
}

} // namespace puyasol::builder::eb
