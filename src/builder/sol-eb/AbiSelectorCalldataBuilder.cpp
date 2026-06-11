#include "builder/sol-eb/AbiSelectorCalldataBuilder.h"
#include "builder/sol-eb/AbiEncoderBuilder.h"

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
	// runtime-extract from the fn-ptr value (external fn-ptrs are encoded
	// as 12 bytes: itob(appId, 8) ++ selector(4)).
	std::shared_ptr<awst::Expression> selector;
	if (targetFuncDef)
	{
		selector = awst::makeMethodConstant(
			AbiEncoderBuilder::buildARC4MethodSelector(_ctx, targetFuncDef), awst::WType::bytesType(), _loc);
	}
	else if (fnType && fnType->kind() == FunctionType::Kind::External)
	{
		auto fnVal = _ctx.buildExpr(targetFnExpr);
		if (!fnVal)
			return nullptr;
		// Coerce fn-ptr value to plain bytes (it's typed as bytes[12] at the
		// AWST level for external fn-ptrs).
		if (fnVal->wtype && fnVal->wtype->kind() == awst::WTypeKind::Bytes)
			fnVal = awst::makeAsBytes(std::move(fnVal), _loc);
		// Extract bytes 8..12 (the 4-byte selector at the tail).
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

	// Encode each arg using the target parameter type (not the source
	// expression type) so that implicit conversions at the callsite — e.g.
	// `0x1234` → bytes2, `"ab"` → bytes2 — produce the EVM ABI layout for
	// that parameter (bytesN left-aligned in a 32-byte word).
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
	for (size_t i = 0; i < callArgs.size(); ++i)
	{
		auto const& arg = callArgs[i];
		auto expr = _ctx.buildExpr(*arg);
		std::shared_ptr<awst::Expression> encoded;

		solidity::frontend::Type const* paramType =
			i < paramTypes.size() ? paramTypes[i] : nullptr;
		auto const* fb = dynamic_cast<FixedBytesType const*>(paramType);
		if (fb)
		{
			unsigned n = fb->numBytes();
			// Coerce source to exactly n bytes, left-aligned.
			std::shared_ptr<awst::Expression> bytesN;
			if (expr->wtype == awst::WType::uint64Type())
			{
				auto itob = awst::makeItob(std::move(expr), _loc);
				if (n <= 8)
				{
					// Take last n bytes of the 8-byte itob result.
					auto off = awst::makeIntegerConstant(8 - n, _loc);
					auto nConst = awst::makeIntegerConstant(n, _loc);
					auto extract = awst::makeExtract3(std::move(itob), std::move(off), std::move(nConst), _loc);
					bytesN = std::move(extract);
				}
				else
				{
					// n > 8: left-pad itob to n bytes.
					bytesN = awst::makeLeftPad(std::move(itob), n - 8, _loc);
				}
			}
			else if (expr->wtype == awst::WType::biguintType())
			{
				auto asBytes = awst::makeAsBytes(std::move(expr), _loc);
				// biguint is 32-byte big-endian: take last n bytes.
				auto off = awst::makeIntegerConstant(32 - n, _loc);
				auto nConst = awst::makeIntegerConstant(n, _loc);
				auto extract = awst::makeExtract3(std::move(asBytes), std::move(off), std::move(nConst), _loc);
				bytesN = std::move(extract);
			}
			else
			{
				// Source is already bytes (string literal, bytesN, etc.).
				bytesN = awst::makeAsBytes(std::move(expr), _loc);
			}

			// Right-pad bytesN to 32 bytes.
			if (n < 32)
				encoded = awst::makeRightPad(std::move(bytesN), 32 - n, _loc);
			else
				encoded = std::move(bytesN);
		}
		else
			encoded = AbiEncoderBuilder::encodeArgAsARC4Bytes(_ctx, std::move(expr), _loc);

		parts.push_back(std::move(encoded));
	}

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

	// selector (4 bytes) + abi.encode(remaining args).
	// The selector argument is `bytes4` in Solidity (integer literals are
	// implicitly cast). Our buildExpr may return a uint64 or biguint for
	// integer literals, so coerce to exactly 4 bytes here.
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

		// Left-pad to ≥4 bytes then take the last 4. makeExtractLastN wraps
		// its input in a SingleEvaluation, so a side-effecting selector
		// expression evaluates once (the previous hand-rolled len+extract3
		// referenced the padded value twice).
		selector = awst::makeExtractLastN(
			awst::makeLeftPad(std::move(asBytes), 4, _loc), 4, _loc);
	}

	if (args.size() == 1)
		return std::make_unique<GenericAbiResult>(_ctx, std::move(selector));

	std::vector<std::shared_ptr<awst::Expression>> parts;
	parts.push_back(std::move(selector));
	parts.push_back(AbiEncoderBuilder::encodeArgsHeadTail(_ctx, _callNode, 1, _loc));
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
	auto sigExpr = _ctx.buildExpr(*args[0]);

	// Solidity's abi.encodeWithSignature uses keccak256 (EVM selector); AVM
	// has a native keccak256 opcode so we emit it directly. For literal
	// signatures we still call keccak256 at runtime — we could fold at compile
	// time but runtime keeps the code simpler and fits in the 700-op budget.
	auto hash = awst::makeKeccak256(std::move(sigExpr), _loc);
	parts.push_back(awst::makeExtract(std::move(hash), 0, 4, _loc));

	if (args.size() == 1)
		return std::make_unique<GenericAbiResult>(_ctx, AbiEncoderBuilder::concatByteExprs(std::move(parts), _loc));

	parts.push_back(AbiEncoderBuilder::encodeArgsHeadTail(_ctx, _callNode, 1, _loc));
	return std::make_unique<GenericAbiResult>(_ctx, AbiEncoderBuilder::concatByteExprs(std::move(parts), _loc));
}

} // namespace puyasol::builder::eb
