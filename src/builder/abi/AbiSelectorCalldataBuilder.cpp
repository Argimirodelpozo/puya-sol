#include "builder/abi/AbiSelectorCalldataBuilder.h"
#include "builder/abi/AbiEncoderBuilder.h"
#include "builder/SelectorSemantics.h"
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
		selector = builder::SelectorSemantics::functionSelector(
			_ctx, *externalType,
			InnerCallHandlers::buildMethodSelector(_ctx, targetFuncDef), _loc);
	}
	else if (fnType && fnType->kind() == FunctionType::Kind::External)
	{
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

	// Encode each arg at its DECLARED param type (not the source expr type) so
	// callsite conversions land on the param's ARC4 width (`0x1234`→bytes2,
	// `"ab"`→bytes2, a small literal→uint256), then ARC4-encode like abi.encode.
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

	parts.push_back(AbiEncoderBuilder::arc4EncodeArgsAtParamTypes(
		_ctx, callArgs, paramTypes, _loc));

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
	parts.push_back(AbiEncoderBuilder::encodeArgsAsArc4(_ctx, _callNode, 1, _loc));
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

	// Compatibility mode preserves the ARC-4 selector. --evm-selectors gives
	// abi.encodeWithSignature its Solidity keccak header; recognised low-level
	// call shapes translate back to ARC-4 at the transport boundary.
	if (auto const* sigLit = dynamic_cast<solidity::frontend::Literal const*>(args[0].get()))
	{
		parts.push_back(builder::SelectorSemantics::signatureSelector(
			_ctx, sigLit->value(), _loc));
	}
	else
	{
		auto sigExpr = _ctx.buildExpr(*args[0]);
		auto hash = awst::makeIntrinsicCall(
			builder::SelectorSemantics::enabled(_ctx.typeMapper)
				? "keccak256" : "sha512_256",
			awst::WType::bytesType(), _loc);
		hash->stackArgs.push_back(std::move(sigExpr));
		parts.push_back(awst::makeExtract(std::move(hash), 0, 4, _loc));
	}

	if (args.size() == 1)
		return std::make_unique<GenericAbiResult>(_ctx, AbiEncoderBuilder::concatByteExprs(std::move(parts), _loc));

	parts.push_back(AbiEncoderBuilder::encodeArgsAsArc4(_ctx, _callNode, 1, _loc));
	return std::make_unique<GenericAbiResult>(_ctx, AbiEncoderBuilder::concatByteExprs(std::move(parts), _loc));
}

} // namespace puyasol::builder::eb
