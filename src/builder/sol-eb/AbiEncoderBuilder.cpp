/// @file AbiEncoderBuilder.cpp
/// Handles abi.encode*, abi.decode — extracted from FunctionCallBuilder.

#include "builder/sol-eb/AbiEncoderBuilder.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::eb
{

class GenericAbiResult: public InstanceBuilder
{
public:
	GenericAbiResult(BuilderContext& _ctx, std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)) {}
	solidity::frontend::Type const* solType() const override { return nullptr; }
};

std::shared_ptr<awst::Expression> AbiEncoderBuilder::makeUint64(
	std::string _value, awst::SourceLocation const& _loc)
{
	auto e = awst::makeIntegerConstant(std::move(_value), _loc);
	return e;
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::leftPadBytes(
	std::shared_ptr<awst::Expression> _expr, int _n, awst::SourceLocation const& _loc)
{
	auto pad = std::make_shared<awst::IntrinsicCall>();
	pad->sourceLocation = _loc;
	pad->wtype = awst::WType::bytesType();
	pad->opCode = "bzero";
	pad->stackArgs.push_back(makeUint64(std::to_string(_n), _loc));

	auto cat = std::make_shared<awst::IntrinsicCall>();
	cat->sourceLocation = _loc;
	cat->wtype = awst::WType::bytesType();
	cat->opCode = "concat";
	cat->stackArgs.push_back(std::move(pad));
	cat->stackArgs.push_back(std::move(_expr));

	auto lenCall = std::make_shared<awst::IntrinsicCall>();
	lenCall->sourceLocation = _loc;
	lenCall->wtype = awst::WType::uint64Type();
	lenCall->opCode = "len";
	lenCall->stackArgs.push_back(cat);

	auto offset = std::make_shared<awst::IntrinsicCall>();
	offset->sourceLocation = _loc;
	offset->wtype = awst::WType::uint64Type();
	offset->opCode = "-";
	offset->stackArgs.push_back(std::move(lenCall));
	offset->stackArgs.push_back(makeUint64(std::to_string(_n), _loc));

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(std::move(cat));
	extract->stackArgs.push_back(std::move(offset));
	extract->stackArgs.push_back(makeUint64(std::to_string(_n), _loc));
	return extract;
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::concatByteExprs(
	std::vector<std::shared_ptr<awst::Expression>> _parts, awst::SourceLocation const& _loc)
{
	if (_parts.empty())
		return awst::makeBytesConstant({}, _loc);
	auto result = std::move(_parts[0]);
	for (size_t i = 1; i < _parts.size(); ++i)
	{
		auto concat = std::make_shared<awst::IntrinsicCall>();
		concat->sourceLocation = _loc;
		concat->wtype = awst::WType::bytesType();
		concat->opCode = "concat";
		concat->stackArgs.push_back(std::move(result));
		concat->stackArgs.push_back(std::move(_parts[i]));
		result = std::move(concat);
	}
	return result;
}

// ── toPackedBytes: convert expr to bytes with optional packed width ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::toPackedBytes(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _expr,
	solidity::frontend::Type const* _solType,
	bool _isPacked,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	int packedWidth = 0;
	if (_isPacked && _solType)
	{
		auto cat = _solType->category();
		if (cat == Type::Category::Integer)
		{
			auto const* intType = dynamic_cast<IntegerType const*>(_solType);
			if (intType) packedWidth = static_cast<int>(intType->numBits() / 8);
		}
		else if (cat == Type::Category::FixedBytes)
		{
			auto const* fbType = dynamic_cast<FixedBytesType const*>(_solType);
			if (fbType) packedWidth = static_cast<int>(fbType->numBytes());
		}
		else if (cat == Type::Category::Bool)
			packedWidth = 1;
	}

	std::shared_ptr<awst::Expression> bytesExpr;

	if (_expr->wtype == awst::WType::bytesType())
		bytesExpr = std::move(_expr);
	else if (_expr->wtype == awst::WType::stringType()
		|| (_expr->wtype && _expr->wtype->kind() == awst::WTypeKind::Bytes))
	{
		auto cast = awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), _loc);
		bytesExpr = std::move(cast);
	}
	else if (_expr->wtype == awst::WType::uint64Type())
	{
		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = _loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(_expr));
		// For non-packed (abi.encode), pad to 32-byte ABI word
		bytesExpr = _isPacked ? std::move(itob) : leftPadBytes(std::move(itob), 32, _loc);
	}
	else if (_expr->wtype == awst::WType::biguintType())
	{
		auto cast = awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), _loc);
		// For non-packed, ensure 32-byte padding
		bytesExpr = _isPacked ? std::move(cast) : leftPadBytes(std::move(cast), 32, _loc);
	}
	else if (_expr->wtype == awst::WType::accountType())
	{
		auto cast = awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), _loc);
		bytesExpr = std::move(cast);
	}
	else if (_expr->wtype == awst::WType::boolType())
	{
		auto boolToInt = std::make_shared<awst::IntrinsicCall>();
		boolToInt->sourceLocation = _loc;
		boolToInt->wtype = awst::WType::uint64Type();
		boolToInt->opCode = "select";
		boolToInt->stackArgs.push_back(makeUint64("0", _loc));
		boolToInt->stackArgs.push_back(makeUint64("1", _loc));
		boolToInt->stackArgs.push_back(std::move(_expr));

		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = _loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(boolToInt));
		bytesExpr = std::move(itob);
	}
	else
	{
		auto cast = awst::makeReinterpretCast(std::move(_expr), awst::WType::bytesType(), _loc);
		bytesExpr = std::move(cast);
	}

	// Packed width truncation/padding
	if (packedWidth > 0 && packedWidth != 8)
	{
		if (packedWidth <= 8)
		{
			auto extract = std::make_shared<awst::IntrinsicCall>();
			extract->sourceLocation = _loc;
			extract->wtype = awst::WType::bytesType();
			extract->opCode = "extract";
			extract->immediates.push_back(8 - packedWidth);
			extract->immediates.push_back(packedWidth);
			extract->stackArgs.push_back(std::move(bytesExpr));
			bytesExpr = std::move(extract);
		}
		else
			bytesExpr = leftPadBytes(std::move(bytesExpr), packedWidth, _loc);
	}

	return bytesExpr;
}

// ── encodeArgAsARC4Bytes ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeArgAsARC4Bytes(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _argExpr,
	awst::SourceLocation const& _loc)
{
	auto* wtype = _argExpr->wtype;

	if (wtype == awst::WType::bytesType() || (wtype && wtype->kind() == awst::WTypeKind::Bytes))
		return _argExpr;
	if (wtype == awst::WType::uint64Type())
	{
		// Solidity ABI: all integers are 32-byte big-endian
		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = _loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(_argExpr));
		return leftPadBytes(std::move(itob), 32, _loc);
	}
	if (wtype == awst::WType::biguintType())
	{
		auto cast = awst::makeReinterpretCast(std::move(_argExpr), awst::WType::bytesType(), _loc);
		return leftPadBytes(std::move(cast), 32, _loc);
	}
	if (wtype == awst::WType::boolType())
	{
		// Solidity ABI: bool is 32-byte right-aligned (0x00...00 or 0x00...01)
		auto setbit = std::make_shared<awst::IntrinsicCall>();
		setbit->sourceLocation = _loc;
		setbit->wtype = awst::WType::bytesType();
		setbit->opCode = "setbit";
		setbit->stackArgs.push_back(awst::makeBytesConstant({0x00}, _loc));
		setbit->stackArgs.push_back(makeUint64("0", _loc));
		setbit->stackArgs.push_back(std::move(_argExpr));
		return leftPadBytes(std::move(setbit), 32, _loc);
	}
	if (wtype == awst::WType::accountType())
	{
		auto cast = awst::makeReinterpretCast(std::move(_argExpr), awst::WType::bytesType(), _loc);
		return cast;
	}
	if (wtype && wtype->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto* refArr = dynamic_cast<awst::ReferenceArray const*>(wtype);
		auto* elemType = refArr ? refArr->elementType() : nullptr;
		auto* arc4ElemType = elemType ? _ctx.typeMapper.mapToARC4Type(elemType) : nullptr;

		awst::WType const* arc4ArrayType = nullptr;
		if (arc4ElemType && refArr && refArr->arraySize())
			arc4ArrayType = _ctx.typeMapper.createType<awst::ARC4StaticArray>(
				arc4ElemType, *refArr->arraySize());
		else if (arc4ElemType)
			arc4ArrayType = _ctx.typeMapper.createType<awst::ARC4DynamicArray>(arc4ElemType);

		if (arc4ArrayType)
		{
			auto encode = std::make_shared<awst::ARC4Encode>();
			encode->sourceLocation = _loc;
			encode->wtype = arc4ArrayType;
			encode->value = std::move(_argExpr);

			auto cast = awst::makeReinterpretCast(std::move(encode), awst::WType::bytesType(), _loc);
			return cast;
		}
	}
	// ARC4 arrays are already encoded — just ReinterpretCast to bytes
	if (wtype && (wtype->kind() == awst::WTypeKind::ARC4StaticArray
		|| wtype->kind() == awst::WTypeKind::ARC4DynamicArray))
	{
		auto cast = awst::makeReinterpretCast(std::move(_argExpr), awst::WType::bytesType(), _loc);
		return cast;
	}

	auto cast = awst::makeReinterpretCast(std::move(_argExpr), awst::WType::bytesType(), _loc);
	return cast;
}

// ── buildARC4MethodSelector ──

std::string AbiEncoderBuilder::buildARC4MethodSelector(
	BuilderContext& _ctx,
	solidity::frontend::FunctionDefinition const* _funcDef)
{
	using namespace solidity::frontend;
	auto solTypeToARC4 = [&](Type const* _type) -> std::string {
		auto* wtype = _ctx.typeMapper.map(_type);
		if (wtype == awst::WType::biguintType()) return "uint256";
		if (wtype == awst::WType::uint64Type()) return "uint64";
		if (wtype == awst::WType::boolType()) return "bool";
		if (wtype == awst::WType::accountType()) return "address";
		if (wtype == awst::WType::bytesType()) return "byte[]";
		if (wtype == awst::WType::stringType()) return "string";
		if (wtype->kind() == awst::WTypeKind::Bytes)
		{
			auto const* bw = static_cast<awst::BytesWType const*>(wtype);
			if (bw->length().has_value())
				return "byte[" + std::to_string(bw->length().value()) + "]";
			return "byte[]";
		}
		if (auto const* structType = dynamic_cast<StructType const*>(_type))
			return "struct " + structType->structDefinition().name();
		return _type->toString(true);
	};

	std::string sel = _funcDef->name() + "(";
	bool first = true;
	for (auto const& param : _funcDef->parameters())
	{
		if (!first) sel += ",";
		sel += solTypeToARC4(param->type());
		first = false;
	}
	sel += ")";
	if (_funcDef->returnParameters().size() > 1)
	{
		sel += "(";
		bool firstRet = true;
		for (auto const& r : _funcDef->returnParameters())
		{
			if (!firstRet) sel += ",";
			sel += solTypeToARC4(r->type());
			firstRet = false;
		}
		sel += ")";
	}
	else if (_funcDef->returnParameters().size() == 1)
		sel += solTypeToARC4(_funcDef->returnParameters()[0]->type());
	else
		sel += "void";
	return sel;
}

// ── encodePacked / encode ──

std::unique_ptr<InstanceBuilder> AbiEncoderBuilder::handleEncodePacked(
	BuilderContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	bool _isPacked,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	auto const& args = _callNode.arguments();

	if (args.empty())
		return std::make_unique<GenericAbiResult>(
			_ctx, awst::makeBytesConstant({}, _loc));

	// Pack each argument, expanding arrays element-by-element for encodePacked
	auto packArg = [&](size_t argIdx) -> std::shared_ptr<awst::Expression> {
		auto const* solType = args[argIdx]->annotation().type;

		auto const* arrType = dynamic_cast<ArrayType const*>(solType);
		if (!arrType && solType && solType->category() == Type::Category::UserDefinedValueType)
		{
			auto const* udvt = dynamic_cast<UserDefinedValueType const*>(solType);
			if (udvt)
				arrType = dynamic_cast<ArrayType const*>(&udvt->underlyingType());
		}

		if (arrType && !arrType->isByteArrayOrString())
		{
			auto arrayExpr = _ctx.buildExpr(*args[argIdx]);
			auto const* elemSolType = arrType->baseType();

			if (!arrType->isDynamicallySized())
			{
				int len = static_cast<int>(arrType->length());
				std::shared_ptr<awst::Expression> packed;
				for (int j = 0; j < len; ++j)
				{
					auto idx = awst::makeIntegerConstant(std::to_string(j), _loc);

					auto indexExpr = std::make_shared<awst::IndexExpression>();
					indexExpr->sourceLocation = _loc;
					indexExpr->base = arrayExpr;
					indexExpr->index = std::move(idx);
					// Use ARC4 element type if base is ARC4 array
					if (arrayExpr->wtype
						&& (arrayExpr->wtype->kind() == awst::WTypeKind::ARC4StaticArray
							|| arrayExpr->wtype->kind() == awst::WTypeKind::ARC4DynamicArray))
					{
						awst::WType const* arc4ElemType = nullptr;
						if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(arrayExpr->wtype))
							arc4ElemType = sa->elementType();
						else if (auto const* da = dynamic_cast<awst::ARC4DynamicArray const*>(arrayExpr->wtype))
							arc4ElemType = da->elementType();
						indexExpr->wtype = arc4ElemType ? arc4ElemType : _ctx.typeMapper.map(elemSolType);
					}
					else
						indexExpr->wtype = _ctx.typeMapper.map(elemSolType);

					auto elemBytes = toPackedBytes(_ctx, std::move(indexExpr), elemSolType, _isPacked, _loc);
					// abi.encode (non-packed) requires each element in
					// arrays to occupy a full 32-byte word, but the ARC4
					// element access for small uintN returns only N/8 raw
					// bytes. Left-pad to 32 so the resulting blob matches
					// EVM's head/tail layout.
					if (!_isPacked && elemBytes)
						elemBytes = leftPadBytes(std::move(elemBytes), 32, _loc);
					if (!packed)
						packed = std::move(elemBytes);
					else
					{
						auto cat = std::make_shared<awst::IntrinsicCall>();
						cat->sourceLocation = _loc;
						cat->wtype = awst::WType::bytesType();
						cat->opCode = "concat";
						cat->stackArgs.push_back(std::move(packed));
						cat->stackArgs.push_back(std::move(elemBytes));
						packed = std::move(cat);
					}
				}
				return packed ? packed : toPackedBytes(_ctx, _ctx.buildExpr(*args[argIdx]), solType, _isPacked, _loc);
			}
			else
			{
				auto encode = std::make_shared<awst::ARC4Encode>();
				encode->sourceLocation = _loc;
				encode->wtype = awst::WType::bytesType();
				encode->value = std::move(arrayExpr);
				return std::shared_ptr<awst::Expression>(std::move(encode));
			}
		}

		return toPackedBytes(_ctx, _ctx.buildExpr(*args[argIdx]), solType, _isPacked, _loc);
	};

	auto result = packArg(0);
	for (size_t i = 1; i < args.size(); ++i)
	{
		auto arg = packArg(i);
		auto concat = std::make_shared<awst::IntrinsicCall>();
		concat->sourceLocation = _loc;
		concat->wtype = awst::WType::bytesType();
		concat->opCode = "concat";
		concat->stackArgs.push_back(std::move(result));
		concat->stackArgs.push_back(std::move(arg));
		result = std::move(concat);
	}
	return std::make_unique<GenericAbiResult>(_ctx, std::move(result));
}

// ── encodeCall ──

std::unique_ptr<InstanceBuilder> AbiEncoderBuilder::handleEncodeCall(
	BuilderContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	if (_callNode.arguments().size() < 2)
		return nullptr;

	auto const& targetFnExpr = *_callNode.arguments()[0];
	FunctionDefinition const* targetFuncDef = nullptr;
	if (auto const* fnType = dynamic_cast<FunctionType const*>(targetFnExpr.annotation().type))
		if (fnType->hasDeclaration())
			targetFuncDef = dynamic_cast<FunctionDefinition const*>(&fnType->declaration());

	if (!targetFuncDef)
		return nullptr;

	std::string methodSig = buildARC4MethodSelector(_ctx, targetFuncDef);
	auto methodConst = std::make_shared<awst::MethodConstant>();
	methodConst->sourceLocation = _loc;
	methodConst->wtype = awst::WType::bytesType();
	methodConst->value = methodSig;

	std::vector<std::shared_ptr<awst::Expression>> parts;
	parts.push_back(std::move(methodConst));

	auto const& argsExpr = *_callNode.arguments()[1];
	std::vector<ASTPointer<Expression const>> callArgs;
	if (auto const* tupleExpr = dynamic_cast<TupleExpression const*>(&argsExpr))
	{
		for (auto const& comp : tupleExpr->components())
			if (comp) callArgs.push_back(comp);
	}
	else
		callArgs.push_back(_callNode.arguments()[1]);

	for (auto const& arg : callArgs)
		parts.push_back(encodeArgAsARC4Bytes(_ctx, _ctx.buildExpr(*arg), _loc));

	return std::make_unique<GenericAbiResult>(_ctx, concatByteExprs(std::move(parts), _loc));
}

// ── encodeWithSelector ──

std::unique_ptr<InstanceBuilder> AbiEncoderBuilder::handleEncodeWithSelector(
	BuilderContext& _ctx,
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
			auto itob = std::make_shared<awst::IntrinsicCall>();
			itob->sourceLocation = _loc;
			itob->wtype = awst::WType::bytesType();
			itob->opCode = "itob";
			itob->stackArgs.push_back(std::move(selector));
			asBytes = std::move(itob);
		}
		else if (selector->wtype == awst::WType::biguintType())
		{
			auto cast = awst::makeReinterpretCast(std::move(selector), awst::WType::bytesType(), _loc);
			asBytes = std::move(cast);
		}

		// Left-pad to ≥4 bytes then extract the last 4 bytes.
		auto bzeroSize = awst::makeIntegerConstant("4", _loc);
		auto zeros = std::make_shared<awst::IntrinsicCall>();
		zeros->sourceLocation = _loc;
		zeros->wtype = awst::WType::bytesType();
		zeros->opCode = "bzero";
		zeros->stackArgs.push_back(std::move(bzeroSize));

		auto cat = std::make_shared<awst::IntrinsicCall>();
		cat->sourceLocation = _loc;
		cat->wtype = awst::WType::bytesType();
		cat->opCode = "concat";
		cat->stackArgs.push_back(std::move(zeros));
		cat->stackArgs.push_back(std::move(asBytes));

		auto lenCall = std::make_shared<awst::IntrinsicCall>();
		lenCall->sourceLocation = _loc;
		lenCall->wtype = awst::WType::uint64Type();
		lenCall->opCode = "len";
		lenCall->stackArgs.push_back(cat);

		auto four = awst::makeIntegerConstant("4", _loc);

		auto offset = std::make_shared<awst::IntrinsicCall>();
		offset->sourceLocation = _loc;
		offset->wtype = awst::WType::uint64Type();
		offset->opCode = "-";
		offset->stackArgs.push_back(std::move(lenCall));
		offset->stackArgs.push_back(four);

		auto extract = std::make_shared<awst::IntrinsicCall>();
		extract->sourceLocation = _loc;
		extract->wtype = awst::WType::bytesType();
		extract->opCode = "extract3";
		extract->stackArgs.push_back(std::move(cat));
		extract->stackArgs.push_back(std::move(offset));
		extract->stackArgs.push_back(std::move(four));
		selector = std::move(extract);
	}

	if (args.size() == 1)
		return std::make_unique<GenericAbiResult>(_ctx, std::move(selector));

	// Check if trailing args have any dynamic types. StringLiteralType is
	// static under Solidity's TypeSystem, but its mobileType `string memory`
	// is dynamically encoded — abi.encodeWithSelector(sel, "") should emit
	// the head/tail layout, not just the bare selector.
	auto isDynArg = [](solidity::frontend::Type const* _t) -> bool {
		if (!_t) return false;
		if (_t->isDynamicallyEncoded()) return true;
		if (_t->category() == solidity::frontend::Type::Category::StringLiteral)
			return true;
		return false;
	};
	bool hasDynamic = false;
	for (size_t i = 1; i < args.size(); ++i)
	{
		auto const* solType = args[i]->annotation().type;
		if (isDynArg(solType))
			hasDynamic = true;
	}

	std::vector<std::shared_ptr<awst::Expression>> parts;
	parts.push_back(std::move(selector));

	if (!hasDynamic)
	{
		// All static: simple concatenation
		for (size_t i = 1; i < args.size(); ++i)
			parts.push_back(toPackedBytes(_ctx, _ctx.buildExpr(*args[i]), args[i]->annotation().type, false, _loc));
	}
	else
	{
		// Head/tail encoding for trailing args
		size_t numTrailing = args.size() - 1;
		size_t headSize = numTrailing * 32;

		std::vector<std::shared_ptr<awst::Expression>> headParts;
		std::vector<std::shared_ptr<awst::Expression>> tailParts;
		auto currentOffset = makeUint64(std::to_string(headSize), _loc);

		for (size_t i = 1; i < args.size(); ++i)
		{
			auto const* solType = args[i]->annotation().type;
			bool isDyn = isDynArg(solType);
			auto expr = _ctx.buildExpr(*args[i]);

			if (!isDyn)
			{
				headParts.push_back(toPackedBytes(_ctx, std::move(expr), solType, false, _loc));
			}
			else
			{
				auto offsetItob = std::make_shared<awst::IntrinsicCall>();
				offsetItob->sourceLocation = _loc;
				offsetItob->wtype = awst::WType::bytesType();
				offsetItob->opCode = "itob";
				offsetItob->stackArgs.push_back(currentOffset);
				headParts.push_back(leftPadBytes(std::move(offsetItob), 32, _loc));

				auto tail = encodeDynamicTail(_ctx, std::move(expr), solType, _loc);
				auto tailLen = std::make_shared<awst::IntrinsicCall>();
				tailLen->sourceLocation = _loc;
				tailLen->wtype = awst::WType::uint64Type();
				tailLen->opCode = "len";
				tailLen->stackArgs.push_back(tail);

				auto newOffset = std::make_shared<awst::UInt64BinaryOperation>();
				newOffset->sourceLocation = _loc;
				newOffset->wtype = awst::WType::uint64Type();
				newOffset->op = awst::UInt64BinaryOperator::Add;
				newOffset->left = std::move(currentOffset);
				newOffset->right = std::move(tailLen);
				currentOffset = std::move(newOffset);
				tailParts.push_back(std::move(tail));
			}
		}
		auto encoded = concatByteExprs(std::move(headParts), _loc);
		if (!tailParts.empty())
		{
			auto tail = concatByteExprs(std::move(tailParts), _loc);
			auto concat = std::make_shared<awst::IntrinsicCall>();
			concat->sourceLocation = _loc;
			concat->wtype = awst::WType::bytesType();
			concat->opCode = "concat";
			concat->stackArgs.push_back(std::move(encoded));
			concat->stackArgs.push_back(std::move(tail));
			encoded = std::move(concat);
		}
		parts.push_back(std::move(encoded));
	}
	return std::make_unique<GenericAbiResult>(_ctx, concatByteExprs(std::move(parts), _loc));
}

// ── encodeWithSignature ──

std::unique_ptr<InstanceBuilder> AbiEncoderBuilder::handleEncodeWithSignature(
	BuilderContext& _ctx,
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
	auto hash = std::make_shared<awst::IntrinsicCall>();
	hash->sourceLocation = _loc;
	hash->wtype = awst::WType::bytesType();
	hash->opCode = "keccak256";
	hash->stackArgs.push_back(std::move(sigExpr));

	auto extract4 = std::make_shared<awst::IntrinsicCall>();
	extract4->sourceLocation = _loc;
	extract4->wtype = awst::WType::bytesType();
	extract4->opCode = "extract";
	extract4->immediates = {0, 4};
	extract4->stackArgs.push_back(std::move(hash));
	parts.push_back(std::move(extract4));

	if (args.size() == 1)
		return std::make_unique<GenericAbiResult>(_ctx, concatByteExprs(std::move(parts), _loc));

	// Check if trailing args have dynamic types. StringLiteralType is
	// static per Solidity's type system, but its encoding is exactly
	// what ABI dynamic encoding expects (a bytes blob), so we treat it
	// as dynamic here to match Solidity's head/tail layout.
	auto isDynArg = [](solidity::frontend::Type const* _t) -> bool {
		if (!_t) return false;
		if (_t->isDynamicallyEncoded()) return true;
		return dynamic_cast<solidity::frontend::StringLiteralType const*>(_t) != nullptr;
	};
	bool hasDynamic = false;
	for (size_t i = 1; i < args.size(); ++i)
	{
		auto const* solType = args[i]->annotation().type;
		if (isDynArg(solType))
			hasDynamic = true;
	}

	if (!hasDynamic)
	{
		for (size_t i = 1; i < args.size(); ++i)
			parts.push_back(toPackedBytes(_ctx, _ctx.buildExpr(*args[i]), args[i]->annotation().type, false, _loc));
	}
	else
	{
		// Head/tail encoding
		size_t numTrailing = args.size() - 1;
		size_t headSize = numTrailing * 32;
		std::vector<std::shared_ptr<awst::Expression>> headParts;
		std::vector<std::shared_ptr<awst::Expression>> tailParts;
		auto currentOffset = makeUint64(std::to_string(headSize), _loc);

		for (size_t i = 1; i < args.size(); ++i)
		{
			auto const* solType = args[i]->annotation().type;
			bool isDyn = isDynArg(solType);
			auto expr = _ctx.buildExpr(*args[i]);

			if (!isDyn)
			{
				headParts.push_back(toPackedBytes(_ctx, std::move(expr), solType, false, _loc));
			}
			else
			{
				auto offsetItob = std::make_shared<awst::IntrinsicCall>();
				offsetItob->sourceLocation = _loc;
				offsetItob->wtype = awst::WType::bytesType();
				offsetItob->opCode = "itob";
				offsetItob->stackArgs.push_back(currentOffset);
				headParts.push_back(leftPadBytes(std::move(offsetItob), 32, _loc));

				auto tail = encodeDynamicTail(_ctx, std::move(expr), solType, _loc);
				auto tailLen = std::make_shared<awst::IntrinsicCall>();
				tailLen->sourceLocation = _loc;
				tailLen->wtype = awst::WType::uint64Type();
				tailLen->opCode = "len";
				tailLen->stackArgs.push_back(tail);

				auto newOffset = std::make_shared<awst::UInt64BinaryOperation>();
				newOffset->sourceLocation = _loc;
				newOffset->wtype = awst::WType::uint64Type();
				newOffset->op = awst::UInt64BinaryOperator::Add;
				newOffset->left = std::move(currentOffset);
				newOffset->right = std::move(tailLen);
				currentOffset = std::move(newOffset);
				tailParts.push_back(std::move(tail));
			}
		}
		auto encoded = concatByteExprs(std::move(headParts), _loc);
		if (!tailParts.empty())
		{
			auto tail = concatByteExprs(std::move(tailParts), _loc);
			auto concat = std::make_shared<awst::IntrinsicCall>();
			concat->sourceLocation = _loc;
			concat->wtype = awst::WType::bytesType();
			concat->opCode = "concat";
			concat->stackArgs.push_back(std::move(encoded));
			concat->stackArgs.push_back(std::move(tail));
			encoded = std::move(concat);
		}
		parts.push_back(std::move(encoded));
	}
	return std::make_unique<GenericAbiResult>(_ctx, concatByteExprs(std::move(parts), _loc));
}

// ── decode ──

std::unique_ptr<InstanceBuilder> AbiEncoderBuilder::handleDecode(
	BuilderContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	auto* targetType = _ctx.typeMapper.map(_callNode.annotation().type);
	if (_callNode.arguments().empty())
		return nullptr;

	auto decoded = _ctx.buildExpr(*_callNode.arguments()[0]);

	if (!targetType || decoded->wtype == targetType)
		return std::make_unique<GenericAbiResult>(_ctx, std::move(decoded));

	// Bool decode: btoi != 0
	if (targetType == awst::WType::boolType())
	{
		auto bytesExpr = std::move(decoded);
		if (bytesExpr->wtype != awst::WType::bytesType())
		{
			auto toBytes = awst::makeReinterpretCast(std::move(bytesExpr), awst::WType::bytesType(), _loc);
			bytesExpr = std::move(toBytes);
		}
		auto btoi = std::make_shared<awst::IntrinsicCall>();
		btoi->sourceLocation = _loc;
		btoi->wtype = awst::WType::uint64Type();
		btoi->opCode = "btoi";
		btoi->stackArgs.push_back(std::move(bytesExpr));

		auto zero = makeUint64("0", _loc);
		auto cmp = std::make_shared<awst::NumericComparisonExpression>();
		cmp->sourceLocation = _loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(btoi);
		cmp->rhs = std::move(zero);
		cmp->op = awst::NumericComparison::Ne;
		return std::make_unique<GenericAbiResult>(_ctx, std::move(cmp));
	}

	// uint64 decode: ABI-encoded value is a 32-byte big-endian word, so
	// take the last 8 bytes and btoi those — bare btoi on the full word
	// fails at runtime with "btoi arg too long, got 32 bytes".
	if (targetType == awst::WType::uint64Type())
	{
		auto bytesExpr = std::move(decoded);
		if (bytesExpr->wtype != awst::WType::bytesType())
		{
			auto toBytes = awst::makeReinterpretCast(std::move(bytesExpr), awst::WType::bytesType(), _loc);
			bytesExpr = std::move(toBytes);
		}
		// Pull out the first 32 bytes (the head word) — handles ABIv2
		// inputs that prefix with offsets etc. uint64FromAbiWord then
		// extracts the low 8 bytes.
		auto head = std::make_shared<awst::IntrinsicCall>();
		head->sourceLocation = _loc;
		head->wtype = awst::WType::bytesType();
		head->opCode = "extract3";
		head->stackArgs.push_back(std::move(bytesExpr));
		head->stackArgs.push_back(makeUint64("0", _loc));
		head->stackArgs.push_back(makeUint64("32", _loc));
		return std::make_unique<GenericAbiResult>(_ctx, uint64FromAbiWord(std::move(head), _loc));
	}

	// ── Generic ABI decode using decodeAbiValue ──

	// Get the Solidity types for decoding
	auto const* callType = _callNode.annotation().type;
	auto const* tupleType = dynamic_cast<solidity::frontend::TupleType const*>(callType);

	// Ensure data is bytes
	auto dataExpr = std::move(decoded);
	if (dataExpr->wtype != awst::WType::bytesType())
	{
		auto toBytes = awst::makeReinterpretCast(std::move(dataExpr), awst::WType::bytesType(), _loc);
		dataExpr = std::move(toBytes);
	}

	if (tupleType)
	{
		// Tuple decode: decode each element at offset i*32
		auto const& components = tupleType->components();
		std::vector<std::shared_ptr<awst::Expression>> items;
		for (size_t i = 0; i < components.size(); ++i)
		{
			auto offset = makeUint64(std::to_string(i * 32), _loc);
			items.push_back(decodeAbiValue(_ctx, dataExpr, std::move(offset), components[i], _loc));
		}
		auto tuple = std::make_shared<awst::TupleExpression>();
		tuple->sourceLocation = _loc;
		tuple->wtype = targetType;
		tuple->items = std::move(items);
		return std::make_unique<GenericAbiResult>(_ctx, std::move(tuple));
	}

	// Single value decode at offset 0
	auto offset = makeUint64("0", _loc);
	auto result = decodeAbiValue(_ctx, dataExpr, std::move(offset), callType, _loc);
	return std::make_unique<GenericAbiResult>(_ctx, std::move(result));
}

// ── uint64FromAbiWord: extract uint64 from 32-byte ABI word ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::uint64FromAbiWord(
	std::shared_ptr<awst::Expression> _word32,
	awst::SourceLocation const& _loc)
{
	// Last 8 bytes of a 32-byte word: extract(_word32, 24, 8) → btoi
	auto last8 = std::make_shared<awst::IntrinsicCall>();
	last8->sourceLocation = _loc;
	last8->wtype = awst::WType::bytesType();
	last8->opCode = "extract";
	last8->immediates = {24, 8};
	last8->stackArgs.push_back(std::move(_word32));

	auto btoi = std::make_shared<awst::IntrinsicCall>();
	btoi->sourceLocation = _loc;
	btoi->wtype = awst::WType::uint64Type();
	btoi->opCode = "btoi";
	btoi->stackArgs.push_back(std::move(last8));
	return btoi;
}

// ── decodeAbiValue: decode one value from EVM ABI bytes ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::decodeAbiValue(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _data,
	std::shared_ptr<awst::Expression> _offset,
	solidity::frontend::Type const* _solType,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	// Extract the 32-byte head word at _offset
	auto headWord = std::make_shared<awst::IntrinsicCall>();
	headWord->sourceLocation = _loc;
	headWord->wtype = awst::WType::bytesType();
	headWord->opCode = "extract3";
	headWord->stackArgs.push_back(_data);
	headWord->stackArgs.push_back(_offset);
	headWord->stackArgs.push_back(makeUint64("32", _loc));

	auto* wtype = _ctx.typeMapper.map(_solType);

	// ── Static types: value is in the 32-byte head word ──

	// Integer > 64 bits → biguint (ReinterpretCast)
	if (wtype == awst::WType::biguintType())
	{
		auto cast = awst::makeReinterpretCast(std::move(headWord), awst::WType::biguintType(), _loc);
		return cast;
	}

	// Integer ≤ 64 bits → uint64 (extract last 8 bytes, btoi)
	if (wtype == awst::WType::uint64Type())
		return uint64FromAbiWord(std::move(headWord), _loc);

	// Bool → btoi != 0
	if (wtype == awst::WType::boolType())
	{
		auto val = uint64FromAbiWord(std::move(headWord), _loc);
		auto zero = makeUint64("0", _loc);
		auto cmp = std::make_shared<awst::NumericComparisonExpression>();
		cmp->sourceLocation = _loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(val);
		cmp->rhs = std::move(zero);
		cmp->op = awst::NumericComparison::Ne;
		return cmp;
	}

	// Address → 32 bytes, take last 32 (it's the full word)
	if (wtype == awst::WType::accountType())
	{
		auto cast = awst::makeReinterpretCast(std::move(headWord), awst::WType::accountType(), _loc);
		return cast;
	}

	// Fixed bytes (bytes1..bytes32) → left-aligned in the word, take first N
	if (auto const* fbType = dynamic_cast<FixedBytesType const*>(_solType))
	{
		int n = static_cast<int>(fbType->numBytes());
		auto extractN = std::make_shared<awst::IntrinsicCall>();
		extractN->sourceLocation = _loc;
		extractN->wtype = _ctx.typeMapper.createType<awst::BytesWType>(n);
		extractN->opCode = "extract";
		extractN->immediates = {0, n};
		extractN->stackArgs.push_back(std::move(headWord));
		return extractN;
	}

	// ── Dynamic types: head word contains offset to tail data ──

	if (_solType->isDynamicallyEncoded())
	{
		// The 32-byte head word is an offset relative to the start of the data
		auto tailOffset = uint64FromAbiWord(std::move(headWord), _loc);

		// At tailOffset: [length as 32 bytes][data...]
		// Read length
		auto lenWord = std::make_shared<awst::IntrinsicCall>();
		lenWord->sourceLocation = _loc;
		lenWord->wtype = awst::WType::bytesType();
		lenWord->opCode = "extract3";
		lenWord->stackArgs.push_back(_data);
		lenWord->stackArgs.push_back(tailOffset);
		lenWord->stackArgs.push_back(makeUint64("32", _loc));

		auto dataLen = uint64FromAbiWord(std::move(lenWord), _loc);

		// Data starts at tailOffset + 32
		auto dataStart = std::make_shared<awst::UInt64BinaryOperation>();
		dataStart->sourceLocation = _loc;
		dataStart->wtype = awst::WType::uint64Type();
		dataStart->left = std::move(tailOffset);
		dataStart->op = awst::UInt64BinaryOperator::Add;
		dataStart->right = makeUint64("32", _loc);

		// Extract data bytes
		auto dataBytes = std::make_shared<awst::IntrinsicCall>();
		dataBytes->sourceLocation = _loc;
		dataBytes->wtype = awst::WType::bytesType();
		dataBytes->opCode = "extract3";
		dataBytes->stackArgs.push_back(_data);
		dataBytes->stackArgs.push_back(std::move(dataStart));
		dataBytes->stackArgs.push_back(std::move(dataLen));

		// Cast to target type (string, bytes, etc.)
		if (wtype == awst::WType::stringType())
		{
			auto cast = awst::makeReinterpretCast(std::move(dataBytes), awst::WType::stringType(), _loc);
			return cast;
		}
		return dataBytes;
	}

	// Fallback: ReinterpretCast the 32-byte word
	auto cast = awst::makeReinterpretCast(std::move(headWord), wtype, _loc);
	return cast;
}

// ── rightPadTo32: pad bytes to next 32-byte boundary ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::rightPadTo32(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	// Compute padding needed: (32 - (len % 32)) % 32
	// Then concat with bzero(padding)
	// For simplicity: concat(expr, bzero(32)), then extract first (len + padding) bytes
	// Actually simpler: concat(expr, bzero(31)), then extract first ((len + 31) / 32 * 32) bytes

	// len = len(expr)
	auto lenCall = std::make_shared<awst::IntrinsicCall>();
	lenCall->sourceLocation = _loc;
	lenCall->wtype = awst::WType::uint64Type();
	lenCall->opCode = "len";
	lenCall->stackArgs.push_back(_expr);

	// padded_len = ((len + 31) / 32) * 32
	auto len31 = std::make_shared<awst::UInt64BinaryOperation>();
	len31->sourceLocation = _loc;
	len31->wtype = awst::WType::uint64Type();
	len31->op = awst::UInt64BinaryOperator::Add;
	len31->left = std::move(lenCall);
	len31->right = makeUint64("31", _loc);

	auto div32 = std::make_shared<awst::UInt64BinaryOperation>();
	div32->sourceLocation = _loc;
	div32->wtype = awst::WType::uint64Type();
	div32->op = awst::UInt64BinaryOperator::FloorDiv;
	div32->left = std::move(len31);
	div32->right = makeUint64("32", _loc);

	auto paddedLen = std::make_shared<awst::UInt64BinaryOperation>();
	paddedLen->sourceLocation = _loc;
	paddedLen->wtype = awst::WType::uint64Type();
	paddedLen->op = awst::UInt64BinaryOperator::Mult;
	paddedLen->left = std::move(div32);
	paddedLen->right = makeUint64("32", _loc);

	// concat(expr, bzero(31)) — ensure enough zeros for any padding
	auto zeros = std::make_shared<awst::IntrinsicCall>();
	zeros->sourceLocation = _loc;
	zeros->wtype = awst::WType::bytesType();
	zeros->opCode = "bzero";
	zeros->stackArgs.push_back(makeUint64("31", _loc));

	auto padded = std::make_shared<awst::IntrinsicCall>();
	padded->sourceLocation = _loc;
	padded->wtype = awst::WType::bytesType();
	padded->opCode = "concat";
	padded->stackArgs.push_back(std::move(_expr));
	padded->stackArgs.push_back(std::move(zeros));

	// extract3(padded, 0, paddedLen)
	auto result = std::make_shared<awst::IntrinsicCall>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::bytesType();
	result->opCode = "extract3";
	result->stackArgs.push_back(std::move(padded));
	result->stackArgs.push_back(makeUint64("0", _loc));
	result->stackArgs.push_back(std::move(paddedLen));
	return result;
}

// ── encodeDynamicTail: [length as 32 bytes][data right-padded to 32] ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeDynamicTail(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _expr,
	solidity::frontend::Type const* _solType,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	// StringLiteralType: treat like bytes/string for encoding purposes.
	bool isStringLiteral = _solType
		&& _solType->category() == Type::Category::StringLiteral;

	// For bytes/string: [length][data padded to 32]
	if (isStringLiteral
		|| (dynamic_cast<ArrayType const*>(_solType) != nullptr
			&& dynamic_cast<ArrayType const*>(_solType)->isByteArrayOrString()))
	{
		{
			// Convert to bytes
			auto bytesExpr = std::move(_expr);
			if (bytesExpr->wtype != awst::WType::bytesType())
			{
				auto cast = awst::makeReinterpretCast(std::move(bytesExpr), awst::WType::bytesType(), _loc);
				bytesExpr = std::move(cast);
			}

			// length as 32-byte uint256
			auto lenCall = std::make_shared<awst::IntrinsicCall>();
			lenCall->sourceLocation = _loc;
			lenCall->wtype = awst::WType::uint64Type();
			lenCall->opCode = "len";
			lenCall->stackArgs.push_back(bytesExpr);

			auto lenItob = std::make_shared<awst::IntrinsicCall>();
			lenItob->sourceLocation = _loc;
			lenItob->wtype = awst::WType::bytesType();
			lenItob->opCode = "itob";
			lenItob->stackArgs.push_back(std::move(lenCall));
			auto lenPadded = leftPadBytes(std::move(lenItob), 32, _loc);

			// data right-padded to 32-byte boundary
			auto dataPadded = rightPadTo32(std::move(bytesExpr), _loc);

			// concat length + data
			auto concat = std::make_shared<awst::IntrinsicCall>();
			concat->sourceLocation = _loc;
			concat->wtype = awst::WType::bytesType();
			concat->opCode = "concat";
			concat->stackArgs.push_back(std::move(lenPadded));
			concat->stackArgs.push_back(std::move(dataPadded));
			return concat;
		}
	}

	// Dynamic T[] array (non-byte elements): [count as uint256][elem0..elemN]
	// where each element is the EVM ABI word encoding of T. On AVM we
	// store dynamic arrays as ARC4 with a 2-byte length header + raw
	// element data. To match Solidity's head/tail layout we:
	//  - compute count = (byte_len - 2) / elem_byte_size
	//  - emit [count_as_uint256] [raw_element_bytes]
	// This only matches EVM when the ARC4 element width equals the
	// abi.encode word width (32 bytes) — i.e. only uint256[], bytes32[]
	// and similar. For smaller element types (uint8[]) the concatenated
	// raw data is still packed rather than per-element 32-byte padded,
	// so the test harness will still diff. A fully general dynamic
	// array encoder would need a runtime loop which puya's AWST doesn't
	// currently allow as a sub-expression.
	if (auto const* arrType = dynamic_cast<ArrayType const*>(_solType))
	{
		if (arrType->isDynamicallySized() && !arrType->isByteArrayOrString())
		{
			auto const* elemSolType = arrType->baseType();
			unsigned elemByteSize = 32;
			if (auto const* intType = dynamic_cast<IntegerType const*>(elemSolType))
				elemByteSize = std::max(1u, intType->numBits() / 8);
			else if (auto const* fbType = dynamic_cast<FixedBytesType const*>(elemSolType))
				elemByteSize = std::max(1u, (unsigned) fbType->numBytes());

			// Only enable for 32-byte elements (uint256[], bytes32[]).
			// Smaller elements need per-element padding which requires
			// loops we can't emit as a sub-expression.
			if (elemByteSize == 32)
			{
				auto arrayExpr = _expr;
				auto asBytes = awst::makeReinterpretCast(arrayExpr, awst::WType::bytesType(), _loc);

				auto rawLen = std::make_shared<awst::IntrinsicCall>();
				rawLen->sourceLocation = _loc;
				rawLen->wtype = awst::WType::uint64Type();
				rawLen->opCode = "len";
				rawLen->stackArgs.push_back(asBytes);

				auto two = awst::makeIntegerConstant("2", _loc);

				auto contentBytes = std::make_shared<awst::UInt64BinaryOperation>();
				contentBytes->sourceLocation = _loc;
				contentBytes->wtype = awst::WType::uint64Type();
				contentBytes->left = std::move(rawLen);
				contentBytes->op = awst::UInt64BinaryOperator::Sub;
				contentBytes->right = std::move(two);

				auto elemSize = awst::makeIntegerConstant(std::to_string(elemByteSize), _loc);

				auto lenExpr = std::make_shared<awst::UInt64BinaryOperation>();
				lenExpr->sourceLocation = _loc;
				lenExpr->wtype = awst::WType::uint64Type();
				lenExpr->left = std::move(contentBytes);
				lenExpr->op = awst::UInt64BinaryOperator::FloorDiv;
				lenExpr->right = std::move(elemSize);

				auto lenItob = std::make_shared<awst::IntrinsicCall>();
				lenItob->sourceLocation = _loc;
				lenItob->wtype = awst::WType::bytesType();
				lenItob->opCode = "itob";
				lenItob->stackArgs.push_back(std::move(lenExpr));
				auto lenPadded = leftPadBytes(std::move(lenItob), 32, _loc);

				auto stripHeader = std::make_shared<awst::IntrinsicCall>();
				stripHeader->sourceLocation = _loc;
				stripHeader->wtype = awst::WType::bytesType();
				stripHeader->opCode = "extract";
				stripHeader->immediates = {2, 0};
				{
					auto bytesCast = awst::makeReinterpretCast(arrayExpr, awst::WType::bytesType(), _loc);
					stripHeader->stackArgs.push_back(std::move(bytesCast));
				}

				auto concatArr = std::make_shared<awst::IntrinsicCall>();
				concatArr->sourceLocation = _loc;
				concatArr->wtype = awst::WType::bytesType();
				concatArr->opCode = "concat";
				concatArr->stackArgs.push_back(std::move(lenPadded));
				concatArr->stackArgs.push_back(std::move(stripHeader));
				return concatArr;
			}
		}
	}

	// Fallback: just pad to 32
	return toPackedBytes(_ctx, std::move(_expr), _solType, false, _loc);
}

// ── handleEncode: EVM ABI encode with head/tail encoding ──

std::unique_ptr<InstanceBuilder> AbiEncoderBuilder::handleEncode(
	BuilderContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;
	auto const& args = _callNode.arguments();

	if (args.empty())
		return std::make_unique<GenericAbiResult>(
			_ctx, awst::makeBytesConstant({}, _loc));

	// Check if any argument is dynamically encoded.
	// StringLiteralType is static per Solidity's type system, but its
	// mobileType (`string memory`) is dynamic. abi.encode("...") treats
	// the literal as a string and emits head/tail encoding, so we classify
	// string literals as dynamic here.
	auto isDynArg = [](solidity::frontend::Type const* _t) -> bool {
		if (!_t) return false;
		if (_t->isDynamicallyEncoded()) return true;
		if (_t->category() == solidity::frontend::Type::Category::StringLiteral)
			return true;
		return false;
	};
	bool hasDynamic = false;
	for (auto const& arg : args)
	{
		if (isDynArg(arg->annotation().type))
		{
			hasDynamic = true;
			break;
		}
	}

	// If no dynamic types, fall back to simple concatenation (current behavior)
	if (!hasDynamic)
		return handleEncodePacked(_ctx, _callNode, /*isPacked=*/false, _loc);

	// Head/tail encoding:
	// Head: for each arg, either 32-byte value (static) or 32-byte offset (dynamic)
	// Tail: concatenated dynamic data
	size_t numArgs = args.size();
	size_t headSize = numArgs * 32; // each slot is 32 bytes

	// Build tail parts first to know their sizes
	// For compile-time-known sizes, compute offsets statically
	// For runtime sizes, we need runtime offset computation

	// Strategy: build all parts, track which are dynamic.
	// Then: head = concat of (static_value OR offset_to_tail) for each arg
	//       tail = concat of all dynamic tails
	// Offsets = headSize + sum of preceding tail sizes

	struct ArgInfo {
		bool isDynamic;
		std::shared_ptr<awst::Expression> headPart;  // 32-byte value or offset
		std::shared_ptr<awst::Expression> tailPart;   // null for static
	};
	std::vector<ArgInfo> argInfos;

	// First pass: build all expressions and classify
	std::vector<std::shared_ptr<awst::Expression>> tailParts;

	for (size_t i = 0; i < numArgs; ++i)
	{
		auto const* solType = args[i]->annotation().type;
		bool isDyn = isDynArg(solType);
		auto expr = _ctx.buildExpr(*args[i]);

		if (!isDyn)
		{
			// Static: encode as 32-byte value
			argInfos.push_back({false, toPackedBytes(_ctx, std::move(expr), solType, false, _loc), nullptr});
		}
		else
		{
			// Dynamic: tail data + placeholder for offset.
			// String literals need their mobile type (string memory) for
			// encodeDynamicTail's ArrayType dispatch.
			solidity::frontend::Type const* tailSolType = solType;
			if (solType && solType->category() == solidity::frontend::Type::Category::StringLiteral)
				tailSolType = solType->mobileType();
			auto tail = encodeDynamicTail(_ctx, std::move(expr), tailSolType, _loc);
			argInfos.push_back({true, nullptr, tail});
		}
	}

	// Second pass: compute offsets and build head
	// We need runtime offset computation because tail sizes may vary.
	// Use a running offset variable: start at headSize, add each tail's length.
	//
	// For simplicity, compute tail sizes at runtime using len() and build
	// offset values dynamically.
	//
	// offset_i = headSize + sum(len(tail_j) for j < i where j is dynamic)

	// Build tail concat and track cumulative sizes
	std::vector<std::shared_ptr<awst::Expression>> headParts;
	std::vector<std::shared_ptr<awst::Expression>> tailConcatParts;

	// Running tail offset as AWST expression (starts at headSize)
	std::shared_ptr<awst::Expression> currentTailOffset = makeUint64(std::to_string(headSize), _loc);

	for (size_t i = 0; i < numArgs; ++i)
	{
		if (!argInfos[i].isDynamic)
		{
			headParts.push_back(std::move(argInfos[i].headPart));
		}
		else
		{
			// Head: offset as 32-byte big-endian
			auto offsetItob = std::make_shared<awst::IntrinsicCall>();
			offsetItob->sourceLocation = _loc;
			offsetItob->wtype = awst::WType::bytesType();
			offsetItob->opCode = "itob";
			offsetItob->stackArgs.push_back(currentTailOffset);
			headParts.push_back(leftPadBytes(std::move(offsetItob), 32, _loc));

			// Update running offset: currentTailOffset += len(tail_i)
			auto tailLen = std::make_shared<awst::IntrinsicCall>();
			tailLen->sourceLocation = _loc;
			tailLen->wtype = awst::WType::uint64Type();
			tailLen->opCode = "len";
			tailLen->stackArgs.push_back(argInfos[i].tailPart);

			auto newOffset = std::make_shared<awst::UInt64BinaryOperation>();
			newOffset->sourceLocation = _loc;
			newOffset->wtype = awst::WType::uint64Type();
			newOffset->op = awst::UInt64BinaryOperator::Add;
			newOffset->left = std::move(currentTailOffset);
			newOffset->right = std::move(tailLen);
			currentTailOffset = std::move(newOffset);

			tailConcatParts.push_back(std::move(argInfos[i].tailPart));
		}
	}

	// Concat head + tail
	auto head = concatByteExprs(std::move(headParts), _loc);
	if (!tailConcatParts.empty())
	{
		auto tail = concatByteExprs(std::move(tailConcatParts), _loc);
		auto result = std::make_shared<awst::IntrinsicCall>();
		result->sourceLocation = _loc;
		result->wtype = awst::WType::bytesType();
		result->opCode = "concat";
		result->stackArgs.push_back(std::move(head));
		result->stackArgs.push_back(std::move(tail));
		return std::make_unique<GenericAbiResult>(_ctx, std::move(result));
	}
	return std::make_unique<GenericAbiResult>(_ctx, std::move(head));
}

// ── Top-level dispatcher ──

std::unique_ptr<InstanceBuilder> AbiEncoderBuilder::tryHandle(
	BuilderContext& _ctx,
	std::string const& _memberName,
	solidity::frontend::FunctionCall const& _callNode,
	awst::SourceLocation const& _loc)
{
	if (_memberName == "encodePacked")
		return handleEncodePacked(_ctx, _callNode, /*isPacked=*/true, _loc);
	if (_memberName == "encode")
		return handleEncode(_ctx, _callNode, _loc);
	if (_memberName == "encodeCall")
		return handleEncodeCall(_ctx, _callNode, _loc);
	if (_memberName == "encodeWithSelector")
		return handleEncodeWithSelector(_ctx, _callNode, _loc);
	if (_memberName == "encodeWithSignature")
		return handleEncodeWithSignature(_ctx, _callNode, _loc);
	if (_memberName == "decode")
		return handleDecode(_ctx, _callNode, _loc);
	return nullptr;
}

} // namespace puyasol::builder::eb
