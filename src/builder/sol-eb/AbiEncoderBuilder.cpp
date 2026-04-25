/// @file AbiEncoderBuilder.cpp
/// Handles abi.encode*, abi.decode — extracted from FunctionCallBuilder.

#include "builder/sol-eb/AbiEncoderBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
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
	auto pad = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	pad->stackArgs.push_back(makeUint64(std::to_string(_n), _loc));

	auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
	cat->stackArgs.push_back(std::move(pad));
	cat->stackArgs.push_back(std::move(_expr));

	auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
	lenCall->stackArgs.push_back(cat);

	auto offset = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), _loc);
	offset->stackArgs.push_back(std::move(lenCall));
	offset->stackArgs.push_back(makeUint64(std::to_string(_n), _loc));

	auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
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
		auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
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

	// Structs need head/tail EVM encoding (each field in a 32-byte slot);
	// route through encodeDynamicTail even when the struct is statically
	// sized so small uint fields get 32-byte padding rather than the raw
	// ARC4 packed width.
	if (!_isPacked && dynamic_cast<StructType const*>(_solType))
		return encodeDynamicTail(_ctx, std::move(_expr), _solType, _loc);

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
		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
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
		auto boolToInt = awst::makeIntrinsicCall("select", awst::WType::uint64Type(), _loc);
		boolToInt->stackArgs.push_back(makeUint64("0", _loc));
		boolToInt->stackArgs.push_back(makeUint64("1", _loc));
		boolToInt->stackArgs.push_back(std::move(_expr));

		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
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
			auto extract = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
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

	// Dynamic bytes/string pass through raw (caller handles length header).
	if (wtype == awst::WType::bytesType())
		return _argExpr;
	// Fixed-size bytesN (bytes1..bytes32): EVM stores these left-aligned in a
	// 32-byte word (value at high bytes, zero at low bytes). Right-pad so
	// abi.encodeCall lays out arguments exactly like the EVM ABI — otherwise
	// a bytes2 arg occupies only 2 bytes and the caller sees shifted data.
	if (wtype && wtype->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bw = dynamic_cast<awst::BytesWType const*>(wtype);
		int len = bw && bw->length() ? *bw->length() : 0;
		if (len > 0 && len < 32)
		{
			auto asBytes = awst::makeReinterpretCast(std::move(_argExpr), awst::WType::bytesType(), _loc);
			auto padSize = awst::makeIntegerConstant(std::to_string(32 - len), _loc);
			auto pad = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
			pad->stackArgs.push_back(std::move(padSize));
			auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
			cat->stackArgs.push_back(std::move(asBytes));
			cat->stackArgs.push_back(std::move(pad));
			return cat;
		}
		return _argExpr;
	}
	if (wtype == awst::WType::uint64Type())
	{
		// Solidity ABI: all integers are 32-byte big-endian
		auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
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
		auto setbit = awst::makeIntrinsicCall("setbit", awst::WType::bytesType(), _loc);
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
						auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
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
		auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
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

	// Encode each arg using the target parameter type (not the source
	// expression type) so that implicit conversions at the callsite — e.g.
	// `0x1234` → bytes2, `"ab"` → bytes2 — produce the EVM ABI layout for
	// that parameter (bytesN left-aligned in a 32-byte word).
	auto const& targetParams = targetFuncDef->parameters();
	for (size_t i = 0; i < callArgs.size(); ++i)
	{
		auto const& arg = callArgs[i];
		auto expr = _ctx.buildExpr(*arg);
		std::shared_ptr<awst::Expression> encoded;

		solidity::frontend::Type const* paramType =
			i < targetParams.size() && targetParams[i] ? targetParams[i]->type() : nullptr;
		auto const* fb = dynamic_cast<FixedBytesType const*>(paramType);
		if (fb)
		{
			unsigned n = fb->numBytes();
			// Coerce source to exactly n bytes, left-aligned.
			std::shared_ptr<awst::Expression> bytesN;
			if (expr->wtype == awst::WType::uint64Type())
			{
				auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
				itob->stackArgs.push_back(std::move(expr));
				if (n <= 8)
				{
					// Take last n bytes of the 8-byte itob result.
					auto off = awst::makeIntegerConstant(std::to_string(8 - n), _loc);
					auto nConst = awst::makeIntegerConstant(std::to_string(n), _loc);
					auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
					extract->stackArgs.push_back(std::move(itob));
					extract->stackArgs.push_back(std::move(off));
					extract->stackArgs.push_back(std::move(nConst));
					bytesN = std::move(extract);
				}
				else
				{
					// n > 8: left-pad itob to n bytes.
					auto padSize = awst::makeIntegerConstant(std::to_string(n - 8), _loc);
					auto pad = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
					pad->stackArgs.push_back(std::move(padSize));
					auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
					cat->stackArgs.push_back(std::move(pad));
					cat->stackArgs.push_back(std::move(itob));
					bytesN = std::move(cat);
				}
			}
			else if (expr->wtype == awst::WType::biguintType())
			{
				auto asBytes = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), _loc);
				// biguint is 32-byte big-endian: take last n bytes.
				auto off = awst::makeIntegerConstant(std::to_string(32 - n), _loc);
				auto nConst = awst::makeIntegerConstant(std::to_string(n), _loc);
				auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
				extract->stackArgs.push_back(std::move(asBytes));
				extract->stackArgs.push_back(std::move(off));
				extract->stackArgs.push_back(std::move(nConst));
				bytesN = std::move(extract);
			}
			else
			{
				// Source is already bytes (string literal, bytesN, etc.).
				bytesN = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), _loc);
			}

			// Right-pad bytesN to 32 bytes.
			if (n < 32)
			{
				auto padSize = awst::makeIntegerConstant(std::to_string(32 - n), _loc);
				auto pad = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
				pad->stackArgs.push_back(std::move(padSize));
				auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
				cat->stackArgs.push_back(std::move(bytesN));
				cat->stackArgs.push_back(std::move(pad));
				encoded = std::move(cat);
			}
			else
				encoded = std::move(bytesN);
		}
		else
			encoded = encodeArgAsARC4Bytes(_ctx, std::move(expr), _loc);

		parts.push_back(std::move(encoded));
	}

	return std::make_unique<GenericAbiResult>(_ctx, concatByteExprs(std::move(parts), _loc));
}

// ── encodeWithSelector ──

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeArgsHeadTail(
	BuilderContext& _ctx,
	solidity::frontend::FunctionCall const& _callNode,
	size_t _startIdx,
	awst::SourceLocation const& _loc)
{
	auto const& args = _callNode.arguments();
	// StringLiteralType is nominally static per Solidity's type system, but
	// its encoding is dynamic (length + data) — treat as dynamic so offsets
	// get emitted in the head.
	auto isDynArg = [](solidity::frontend::Type const* _t) {
		if (!_t) return false;
		return _t->isDynamicallyEncoded()
			|| _t->category() == solidity::frontend::Type::Category::StringLiteral;
	};
	bool hasDynamic = false;
	for (size_t i = _startIdx; i < args.size(); ++i)
		if (isDynArg(args[i]->annotation().type))
			hasDynamic = true;

	if (!hasDynamic)
	{
		std::vector<std::shared_ptr<awst::Expression>> parts;
		for (size_t i = _startIdx; i < args.size(); ++i)
			parts.push_back(toPackedBytes(_ctx, _ctx.buildExpr(*args[i]), args[i]->annotation().type, false, _loc));
		return concatByteExprs(std::move(parts), _loc);
	}

	// Head/tail encoding: each arg in head gets either its packed value
	// (static) or a 32-byte offset pointer (dynamic) into the tail.
	size_t numTrailing = args.size() - _startIdx;
	size_t headSize = numTrailing * 32;
	std::vector<std::shared_ptr<awst::Expression>> headParts;
	std::vector<std::shared_ptr<awst::Expression>> tailParts;
	auto currentOffset = makeUint64(std::to_string(headSize), _loc);

	for (size_t i = _startIdx; i < args.size(); ++i)
	{
		auto const* solType = args[i]->annotation().type;
		auto expr = _ctx.buildExpr(*args[i]);
		if (!isDynArg(solType))
		{
			headParts.push_back(toPackedBytes(_ctx, std::move(expr), solType, false, _loc));
			continue;
		}
		auto offsetItob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
		offsetItob->stackArgs.push_back(currentOffset);
		headParts.push_back(leftPadBytes(std::move(offsetItob), 32, _loc));

		auto tail = encodeDynamicTail(_ctx, std::move(expr), solType, _loc);
		auto tailLen = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
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

	auto encoded = concatByteExprs(std::move(headParts), _loc);
	if (!tailParts.empty())
	{
		auto tail = concatByteExprs(std::move(tailParts), _loc);
		auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
		concat->stackArgs.push_back(std::move(encoded));
		concat->stackArgs.push_back(std::move(tail));
		encoded = std::move(concat);
	}
	return encoded;
}

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
			auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
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
		auto zeros = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
		zeros->stackArgs.push_back(std::move(bzeroSize));

		auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
		cat->stackArgs.push_back(std::move(zeros));
		cat->stackArgs.push_back(std::move(asBytes));

		auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
		lenCall->stackArgs.push_back(cat);

		auto four = awst::makeIntegerConstant("4", _loc);

		auto offset = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), _loc);
		offset->stackArgs.push_back(std::move(lenCall));
		offset->stackArgs.push_back(four);

		auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		extract->stackArgs.push_back(std::move(cat));
		extract->stackArgs.push_back(std::move(offset));
		extract->stackArgs.push_back(std::move(four));
		selector = std::move(extract);
	}

	if (args.size() == 1)
		return std::make_unique<GenericAbiResult>(_ctx, std::move(selector));

	std::vector<std::shared_ptr<awst::Expression>> parts;
	parts.push_back(std::move(selector));
	parts.push_back(encodeArgsHeadTail(_ctx, _callNode, 1, _loc));
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
	auto hash = awst::makeIntrinsicCall("keccak256", awst::WType::bytesType(), _loc);
	hash->stackArgs.push_back(std::move(sigExpr));

	auto extract4 = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
	extract4->immediates = {0, 4};
	extract4->stackArgs.push_back(std::move(hash));
	parts.push_back(std::move(extract4));

	if (args.size() == 1)
		return std::make_unique<GenericAbiResult>(_ctx, concatByteExprs(std::move(parts), _loc));

	parts.push_back(encodeArgsHeadTail(_ctx, _callNode, 1, _loc));
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
		auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
		btoi->stackArgs.push_back(std::move(bytesExpr));

		auto zero = makeUint64("0", _loc);
		auto cmp = awst::makeNumericCompare(std::move(btoi), awst::NumericComparison::Ne, std::move(zero), _loc);
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
		auto head = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
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
	auto last8 = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
	last8->immediates = {24, 8};
	last8->stackArgs.push_back(std::move(_word32));

	auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
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
	auto headWord = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
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
		auto cmp = awst::makeNumericCompare(std::move(val), awst::NumericComparison::Ne, std::move(zero), _loc);
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
		auto extractN = awst::makeIntrinsicCall("extract", _ctx.typeMapper.createType<awst::BytesWType>(n), _loc);
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
		auto lenWord = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		lenWord->stackArgs.push_back(_data);
		lenWord->stackArgs.push_back(tailOffset);
		lenWord->stackArgs.push_back(makeUint64("32", _loc));

		auto elemCount = uint64FromAbiWord(std::move(lenWord), _loc);

		// Data starts at tailOffset + 32
		auto dataStart = awst::makeUInt64BinOp(std::move(tailOffset), awst::UInt64BinaryOperator::Add, makeUint64("32", _loc), _loc);

		// ARC4DynamicArray with fixed-size element: translate EVM-ABI layout
		// ([32-byte length][N × 32 bytes]) to ARC4 layout
		// ([uint16 BE length][N × elemSize bytes]). EVM pads each element to 32
		// bytes regardless of its logical size, so we bulk-copy N*elemSize bytes.
		// (Only correct when the element has a fixed ARC4 encoded size and the
		// EVM encoded size also equals 32 bytes per element, which holds for
		// uintN/intN/bool/address/bytesN but not e.g. uint256[][] or structs
		// containing dynamic fields — those keep the fallback path.)
		if (wtype->kind() == awst::WTypeKind::ARC4DynamicArray)
		{
			auto const* dynArr = static_cast<awst::ARC4DynamicArray const*>(wtype);
			auto const* elemType = dynArr->elementType();
			int elemSize = ::puyasol::builder::TypeCoercion::computeEncodedElementSize(elemType);
			// Only safe when EVM encoded size (always 32 bytes per slot for
			// value-typed elements) matches the ARC4 encoded size — i.e., 32.
			// Covers uint256/int256/bytes32/address/contract arrays. Smaller
			// widths (uint128[], uint8[]) fall through to the generic fallback
			// because EVM slot-pads each element to 32 bytes while ARC4 packs.
			if (elemSize == 32)
			{
				// byteCount = elemCount * elemSize
				auto byteCount = awst::makeUInt64BinOp(
					elemCount,
					awst::UInt64BinaryOperator::Mult,
					makeUint64(std::to_string(elemSize), _loc),
					_loc);

				// elemBytes = extract3(_data, dataStart, byteCount)
				auto elemBytes = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
				elemBytes->stackArgs.push_back(_data);
				elemBytes->stackArgs.push_back(std::move(dataStart));
				elemBytes->stackArgs.push_back(std::move(byteCount));

				// arc4Header = extract3(itob(elemCount), 6, 2) — uint16 BE length
				auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
				itob->stackArgs.push_back(std::move(elemCount));
				auto header = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
				header->stackArgs.push_back(std::move(itob));
				header->stackArgs.push_back(makeUint64("6", _loc));
				header->stackArgs.push_back(makeUint64("2", _loc));

				// concat(header, elemBytes)
				auto arc4Bytes = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
				arc4Bytes->stackArgs.push_back(std::move(header));
				arc4Bytes->stackArgs.push_back(std::move(elemBytes));

				// ReinterpretCast to ARC4DynamicArray<elem>
				return awst::makeReinterpretCast(std::move(arc4Bytes), wtype, _loc);
			}
		}

		// Extract data bytes (length word is interpreted as byte count — correct
		// for bytes/string where elements are 1 byte each)
		auto dataBytes = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		dataBytes->stackArgs.push_back(_data);
		dataBytes->stackArgs.push_back(std::move(dataStart));
		dataBytes->stackArgs.push_back(std::move(elemCount));

		// Cast to target type (string, bytes, etc.)
		if (wtype == awst::WType::stringType())
		{
			auto cast = awst::makeReinterpretCast(std::move(dataBytes), awst::WType::stringType(), _loc);
			return cast;
		}
		// ARC4-shaped targets (static arrays of dynamic elems, structs with
		// dynamic fields, tuples with dynamic elems, dynamic arrays with
		// dynamic element size): wrap the ABI-decoded bytes in ARC4FromBytes so
		// the assignment target sees a properly-typed value. The resulting
		// layout is not actually ARC4 (EVM ABI differs), so downstream access
		// will likely trap at runtime — matches the semantic-test expectation
		// of FAILURE for corrupt-input decode cases.
		auto kind = wtype->kind();
		if (kind == awst::WTypeKind::ARC4DynamicArray
			|| kind == awst::WTypeKind::ARC4StaticArray
			|| kind == awst::WTypeKind::ARC4Struct
			|| kind == awst::WTypeKind::ARC4Tuple)
		{
			auto fromBytes = std::make_shared<awst::ARC4FromBytes>();
			fromBytes->sourceLocation = _loc;
			fromBytes->wtype = wtype;
			fromBytes->value = std::move(dataBytes);
			fromBytes->validate = false;
			return fromBytes;
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
	auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
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
	auto zeros = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	zeros->stackArgs.push_back(makeUint64("31", _loc));

	auto padded = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
	padded->stackArgs.push_back(std::move(_expr));
	padded->stackArgs.push_back(std::move(zeros));

	// extract3(padded, 0, paddedLen)
	auto result = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
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
			auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
			lenCall->stackArgs.push_back(bytesExpr);

			auto lenItob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
			lenItob->stackArgs.push_back(std::move(lenCall));
			auto lenPadded = leftPadBytes(std::move(lenItob), 32, _loc);

			// data right-padded to 32-byte boundary
			auto dataPadded = rightPadTo32(std::move(bytesExpr), _loc);

			// concat length + data
			auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
			concat->stackArgs.push_back(std::move(lenPadded));
			concat->stackArgs.push_back(std::move(dataPadded));
			return concat;
		}
	}

	// Dynamic T[] array (non-byte elements). Three sub-cases:
	//   (a) Static element with byte size == 32: fast path — strip the
	//       ARC4 uint16 length header and prepend a uint256 length word.
	//       Body bytes are already EVM-ABI-aligned.
	//   (b) Static element with byte size < 32: per-element pad to 32
	//       (left-pad for uints/bool/address, right-pad for bytesN).
	//       Emits a runtime while loop.
	//   (c) Dynamic element (nested dynamic): full head/tail
	//       re-encoding with recursion via `encodeFromArc4Bytes`.
	//       Emits a runtime while loop.
	if (auto const* arrType = dynamic_cast<ArrayType const*>(_solType))
	{
		if (arrType->isDynamicallySized() && !arrType->isByteArrayOrString())
		{
			auto const* elemSolType = arrType->baseType();
			bool elemIsDyn = elemSolType
				&& (elemSolType->isDynamicallyEncoded()
					|| elemSolType->category() == Type::Category::StringLiteral);

			unsigned elemByteSize = 32;
			bool isFixedBytes = false;
			if (auto const* intType = dynamic_cast<IntegerType const*>(elemSolType))
				elemByteSize = std::max(1u, intType->numBits() / 8);
			else if (auto const* fbType = dynamic_cast<FixedBytesType const*>(elemSolType))
			{
				elemByteSize = std::max(1u, (unsigned) fbType->numBytes());
				isFixedBytes = true;
			}
			else if (elemSolType
				&& elemSolType->category() == Type::Category::Bool)
				elemByteSize = 1;
			else if (elemSolType
				&& elemSolType->category() == Type::Category::Address)
				elemByteSize = 20;
			else if (auto const* innerArr = dynamic_cast<ArrayType const*>(elemSolType))
			{
				// Nested static array of static elements (e.g. uint256[3] as
				// elem of T[]). EVM packs each elem as N × 32 bytes — same as
				// our ARC4 packing — so the fast path can copy through if the
				// nested elem is itself fully static and 32-byte-aligned.
				// Only computable when the nested array isn't dynamic.
				if (!innerArr->isDynamicallyEncoded()
					&& !innerArr->isDynamicallySized()
					&& !innerArr->isByteArrayOrString())
				{
					unsigned innerLen = static_cast<unsigned>(innerArr->length());
					auto const* innerBase = innerArr->baseType();
					unsigned innerElemSize = 0;
					if (auto const* it2 = dynamic_cast<IntegerType const*>(innerBase))
						innerElemSize = std::max(1u, it2->numBits() / 8);
					else if (auto const* fb2 = dynamic_cast<FixedBytesType const*>(innerBase))
						innerElemSize = std::max(1u, (unsigned) fb2->numBytes());
					else if (innerBase && innerBase->category() == Type::Category::Address)
						innerElemSize = 20;
					else if (innerBase && innerBase->category() == Type::Category::Bool)
						innerElemSize = 1;
					if (innerElemSize == 32)
						elemByteSize = innerLen * 32;  // exact match: elem is N×32
				}
			}

			// (a) Fast path for elements whose ARC4-encoded byte width
			// already matches their EVM-ABI byte width (i.e. multiples of
			// 32 with no per-element padding needed). Includes uint256[],
			// bytes32[], and nested-static cases like uint256[3][] where
			// each elem is 96 bytes in both encodings.
			if (!elemIsDyn && elemByteSize > 0 && elemByteSize % 32 == 0)
			{
				auto arrayExpr = _expr;
				auto asBytes = awst::makeReinterpretCast(arrayExpr, awst::WType::bytesType(), _loc);

				auto rawLen = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
				rawLen->stackArgs.push_back(asBytes);

				auto two = awst::makeIntegerConstant("2", _loc);

				auto contentBytes = awst::makeUInt64BinOp(std::move(rawLen), awst::UInt64BinaryOperator::Sub, std::move(two), _loc);

				auto elemSize = awst::makeIntegerConstant(std::to_string(elemByteSize), _loc);

				auto lenExpr = awst::makeUInt64BinOp(std::move(contentBytes), awst::UInt64BinaryOperator::FloorDiv, std::move(elemSize), _loc);

				auto lenItob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
				lenItob->stackArgs.push_back(std::move(lenExpr));
				auto lenPadded = leftPadBytes(std::move(lenItob), 32, _loc);

				auto stripHeader = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), _loc);
				stripHeader->immediates = {2, 0};
				{
					auto bytesCast = awst::makeReinterpretCast(arrayExpr, awst::WType::bytesType(), _loc);
					stripHeader->stackArgs.push_back(std::move(bytesCast));
				}

				auto concatArr = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
				concatArr->stackArgs.push_back(std::move(lenPadded));
				concatArr->stackArgs.push_back(std::move(stripHeader));
				return concatArr;
			}

			// (b) Small static element: per-element pad via runtime loop.
			if (!elemIsDyn && elemByteSize > 0 && elemByteSize < 32)
			{
				return encodeDynArrayPadSmallElems(
					_ctx, _expr, elemSolType, elemByteSize, isFixedBytes, _loc);
			}

			// (c) Dynamic element: head/tail re-encoding via runtime loop.
			if (elemIsDyn)
			{
				return encodeDynArrayDynElems(_ctx, _expr, elemSolType, _loc);
			}
		}
	}

	// Struct: recursively encode fields with EVM head/tail layout.
	// For a struct S { T1 f1; T2 f2; ... }, the EVM ABI encoding is:
	//   head = concat(encode_field_i_static | offset_i_if_dynamic) for each i
	//   tail = concat(encode_dynamic_tail_i) for each dynamic i
	if (auto const* structType = dynamic_cast<StructType const*>(_solType))
	{
		auto const& structDef = structType->structDefinition();
		size_t numFields = structDef.members().size();
		if (numFields > 0)
		{
			size_t headSize = numFields * 32;

			std::vector<std::shared_ptr<awst::Expression>> headParts;
			std::vector<std::shared_ptr<awst::Expression>> tailParts;
			std::shared_ptr<awst::Expression> currentOffset = makeUint64(std::to_string(headSize), _loc);

			for (auto const& memberDecl : structDef.members())
			{
				auto const* fieldSolType = memberDecl->type();
				bool isDyn = fieldSolType
					&& (fieldSolType->isDynamicallyEncoded()
						|| fieldSolType->category() == Type::Category::StringLiteral);

				// Build a FieldExpression that pulls the ARC4 field value out
				// of the struct's packed representation, then ARC4Decode to
				// the native wtype so downstream encoders see the "logical"
				// value (e.g. biguint for uint256 rather than arc4.uint256).
				auto* fieldNativeType = _ctx.typeMapper.map(fieldSolType);
				awst::WType const* arc4FieldType = nullptr;
				if (auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(_expr->wtype))
				{
					for (auto const& [fname, ftype]: arc4Struct->fields())
						if (fname == memberDecl->name())
						{
							arc4FieldType = ftype;
							break;
						}
				}

				auto fieldExpr = std::make_shared<awst::FieldExpression>();
				fieldExpr->sourceLocation = _loc;
				fieldExpr->base = _expr;
				fieldExpr->name = memberDecl->name();
				fieldExpr->wtype = arc4FieldType ? arc4FieldType : fieldNativeType;

				std::shared_ptr<awst::Expression> fieldValue = fieldExpr;
				if (arc4FieldType && arc4FieldType != fieldNativeType)
				{
					auto decode = std::make_shared<awst::ARC4Decode>();
					decode->sourceLocation = _loc;
					decode->wtype = fieldNativeType;
					decode->value = std::move(fieldValue);
					fieldValue = std::move(decode);
				}

				if (!isDyn)
					headParts.push_back(toPackedBytes(_ctx, std::move(fieldValue), fieldSolType, false, _loc));
				else
				{
					auto offItob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
					offItob->stackArgs.push_back(currentOffset);
					headParts.push_back(leftPadBytes(std::move(offItob), 32, _loc));

					auto tail = encodeDynamicTail(_ctx, std::move(fieldValue), fieldSolType, _loc);
					auto tailLen = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
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

			auto head = concatByteExprs(std::move(headParts), _loc);
			if (tailParts.empty())
				return head;
			auto tail = concatByteExprs(std::move(tailParts), _loc);
			auto result = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
			result->stackArgs.push_back(std::move(head));
			result->stackArgs.push_back(std::move(tail));
			return result;
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
			auto offsetItob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
			offsetItob->stackArgs.push_back(currentTailOffset);
			headParts.push_back(leftPadBytes(std::move(offsetItob), 32, _loc));

			// Update running offset: currentTailOffset += len(tail_i)
			auto tailLen = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
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
		auto result = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
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

// ── Loop-based EVM-ABI encoders for non-trivial dynamic-array shapes ──
//
// `encodeDynamicTail` handles the trivial cases inline. The two helpers
// below cover the cases that need runtime loops over array elements:
//   (b) per-element padding for small static elements (uint8[], etc.)
//   (c) head/tail re-encoding for nested dynamic elements (uint256[][], etc.)
// Both emit `while` loops into `_ctx.prePendingStatements` and return a
// fresh local var holding the EVM-ABI-encoded bytes.

namespace
{
	// Fresh-name counter shared across all loop emitters. Static so each
	// encoder call produces unique local var names that won't collide
	// across multiple encoders in the same function body.
	int s_encLoopCounter = 0;

	std::shared_ptr<awst::AssignmentStatement> assignFresh(
		std::shared_ptr<awst::Expression> _target,
		std::shared_ptr<awst::Expression> _value,
		awst::SourceLocation const& _loc)
	{
		return awst::makeAssignmentStatement(std::move(_target), std::move(_value), _loc);
	}

	std::shared_ptr<awst::Expression> u64Const(std::string const& _v, awst::SourceLocation const& _loc)
	{
		return awst::makeIntegerConstant(_v, _loc);
	}

	// concat(a, b) on bytes
	std::shared_ptr<awst::Expression> bytesConcat(
		std::shared_ptr<awst::Expression> _a,
		std::shared_ptr<awst::Expression> _b,
		awst::SourceLocation const& _loc)
	{
		auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
		cat->stackArgs.push_back(std::move(_a));
		cat->stackArgs.push_back(std::move(_b));
		return cat;
	}

	// extract3(bytes, start, length) on bytes
	std::shared_ptr<awst::Expression> bytesExtract3(
		std::shared_ptr<awst::Expression> _bytes,
		std::shared_ptr<awst::Expression> _start,
		std::shared_ptr<awst::Expression> _length,
		awst::SourceLocation const& _loc)
	{
		auto e = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
		e->stackArgs.push_back(std::move(_bytes));
		e->stackArgs.push_back(std::move(_start));
		e->stackArgs.push_back(std::move(_length));
		return e;
	}

	// extract_uint16(bytes, byte_offset) → uint64
	std::shared_ptr<awst::Expression> bytesExtractU16(
		std::shared_ptr<awst::Expression> _bytes,
		std::shared_ptr<awst::Expression> _offset,
		awst::SourceLocation const& _loc)
	{
		auto e = awst::makeIntrinsicCall("extract_uint16", awst::WType::uint64Type(), _loc);
		e->stackArgs.push_back(std::move(_bytes));
		e->stackArgs.push_back(std::move(_offset));
		return e;
	}

	// len(bytes) → uint64
	std::shared_ptr<awst::Expression> bytesLen(
		std::shared_ptr<awst::Expression> _bytes,
		awst::SourceLocation const& _loc)
	{
		auto e = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
		e->stackArgs.push_back(std::move(_bytes));
		return e;
	}

	// itob(uint64) → bytes (8 bytes BE)
	std::shared_ptr<awst::Expression> u64Itob(
		std::shared_ptr<awst::Expression> _v,
		awst::SourceLocation const& _loc)
	{
		auto e = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
		e->stackArgs.push_back(std::move(_v));
		return e;
	}

	// bzero(n) → bytes of n zero bytes
	std::shared_ptr<awst::Expression> bytesBzero(
		std::shared_ptr<awst::Expression> _n,
		awst::SourceLocation const& _loc)
	{
		auto e = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
		e->stackArgs.push_back(std::move(_n));
		return e;
	}
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeDynArrayPadSmallElems(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _expr,
	solidity::frontend::Type const* _elemSolType,
	unsigned _elemByteSize,
	bool _isFixedBytes,
	awst::SourceLocation const& _loc)
{
	(void) _elemSolType;
	auto const bytesT = awst::WType::bytesType();
	auto const u64T = awst::WType::uint64Type();

	int tc = s_encLoopCounter++;
	auto suffix = std::to_string(tc);

	// arr_b = ReinterpretCast(_expr, bytes)
	std::string arrName = "__abi_smelem_in_" + suffix;
	auto arrVar = awst::makeVarExpression(arrName, bytesT, _loc);
	{
		auto cast = awst::makeReinterpretCast(_expr, bytesT, _loc);
		_ctx.prePendingStatements.push_back(assignFresh(arrVar, cast, _loc));
	}

	// n = (len(arr_b) - 2) / elemByteSize    (ARC4: 2-byte length header)
	std::string nName = "__abi_smelem_n_" + suffix;
	auto nVar = awst::makeVarExpression(nName, u64T, _loc);
	{
		auto rawLen = bytesLen(arrVar, _loc);
		auto minus2 = awst::makeUInt64BinOp(
			std::move(rawLen), awst::UInt64BinaryOperator::Sub, u64Const("2", _loc), _loc);
		auto n = awst::makeUInt64BinOp(
			std::move(minus2), awst::UInt64BinaryOperator::FloorDiv,
			u64Const(std::to_string(_elemByteSize), _loc), _loc);
		_ctx.prePendingStatements.push_back(assignFresh(nVar, n, _loc));
	}

	// acc = leftpad32(itob(n))           — leading uint256 length word
	std::string accName = "__abi_smelem_acc_" + suffix;
	auto accVar = awst::makeVarExpression(accName, bytesT, _loc);
	{
		auto padded = leftPadBytes(u64Itob(nVar, _loc), 32, _loc);
		_ctx.prePendingStatements.push_back(assignFresh(accVar, padded, _loc));
	}

	// i = 0
	std::string iName = "__abi_smelem_i_" + suffix;
	auto iVar = awst::makeVarExpression(iName, u64T, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(iVar, u64Const("0", _loc), _loc));

	// while i < n: { elem = extract3(arr_b, 2 + i*sz, sz);
	//                acc = concat(acc, padded(elem)); i += 1; }
	auto loop = std::make_shared<awst::WhileLoop>();
	loop->sourceLocation = _loc;
	loop->condition = awst::makeNumericCompare(iVar, awst::NumericComparison::Lt, nVar, _loc);

	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = _loc;

	// elem_off = 2 + i*sz
	auto iScaled = awst::makeUInt64BinOp(
		iVar, awst::UInt64BinaryOperator::Mult,
		u64Const(std::to_string(_elemByteSize), _loc), _loc);
	auto elemOff = awst::makeUInt64BinOp(
		u64Const("2", _loc), awst::UInt64BinaryOperator::Add,
		std::move(iScaled), _loc);

	// elem = extract3(arr_b, elem_off, sz)
	auto elem = bytesExtract3(arrVar, std::move(elemOff),
		u64Const(std::to_string(_elemByteSize), _loc), _loc);

	// padded = (left|right)pad32(elem)
	std::shared_ptr<awst::Expression> padded;
	if (_isFixedBytes)
	{
		// bytesN: right-pad with zeros to 32 (low bytes).
		// elem ++ bzero(32 - sz)
		auto pad = bytesBzero(u64Const(std::to_string(32 - _elemByteSize), _loc), _loc);
		padded = bytesConcat(std::move(elem), std::move(pad), _loc);
	}
	else
	{
		// uint/bool/address: left-pad with zeros to 32 (high bytes).
		// bzero(32 - sz) ++ elem
		auto pad = bytesBzero(u64Const(std::to_string(32 - _elemByteSize), _loc), _loc);
		padded = bytesConcat(std::move(pad), std::move(elem), _loc);
	}

	// acc = concat(acc, padded)
	body->body.push_back(assignFresh(accVar,
		bytesConcat(accVar, std::move(padded), _loc), _loc));

	// i += 1
	body->body.push_back(assignFresh(iVar,
		awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Add,
			u64Const("1", _loc), _loc),
		_loc));

	loop->loopBody = std::move(body);
	_ctx.prePendingStatements.push_back(std::move(loop));

	return accVar;
}

std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeDynArrayDynElems(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _expr,
	solidity::frontend::Type const* _elemSolType,
	awst::SourceLocation const& _loc)
{
	auto const bytesT = awst::WType::bytesType();
	auto const u64T = awst::WType::uint64Type();

	int tc = s_encLoopCounter++;
	auto suffix = std::to_string(tc);

	// arr_b = ReinterpretCast(_expr, bytes)
	std::string arrName = "__abi_dynelem_in_" + suffix;
	auto arrVar = awst::makeVarExpression(arrName, bytesT, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(arrVar,
		awst::makeReinterpretCast(_expr, bytesT, _loc), _loc));

	// outer_n = extract_uint16(arr_b, 0)
	std::string nName = "__abi_dynelem_n_" + suffix;
	auto nVar = awst::makeVarExpression(nName, u64T, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(nVar,
		bytesExtractU16(arrVar, u64Const("0", _loc), _loc), _loc));

	// total_bytes = len(arr_b)
	std::string totName = "__abi_dynelem_tot_" + suffix;
	auto totVar = awst::makeVarExpression(totName, u64T, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(totVar,
		bytesLen(arrVar, _loc), _loc));

	// acc_head = bzero(0)
	std::string headName = "__abi_dynelem_head_" + suffix;
	auto headVar = awst::makeVarExpression(headName, bytesT, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(headVar,
		bytesBzero(u64Const("0", _loc), _loc), _loc));

	// acc_tail = bzero(0)
	std::string tailName = "__abi_dynelem_tail_" + suffix;
	auto tailVar = awst::makeVarExpression(tailName, bytesT, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(tailVar,
		bytesBzero(u64Const("0", _loc), _loc), _loc));

	// off = outer_n * 32             (initial running EVM-ABI offset)
	std::string offName = "__abi_dynelem_off_" + suffix;
	auto offVar = awst::makeVarExpression(offName, u64T, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(offVar,
		awst::makeUInt64BinOp(nVar, awst::UInt64BinaryOperator::Mult,
			u64Const("32", _loc), _loc), _loc));

	// i = 0
	std::string iName = "__abi_dynelem_i_" + suffix;
	auto iVar = awst::makeVarExpression(iName, u64T, _loc);
	_ctx.prePendingStatements.push_back(assignFresh(iVar, u64Const("0", _loc), _loc));

	// While loop body
	auto loop = std::make_shared<awst::WhileLoop>();
	loop->sourceLocation = _loc;
	loop->condition = awst::makeNumericCompare(iVar, awst::NumericComparison::Lt, nVar, _loc);
	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = _loc;

	// inner_arc4_off = extract_uint16(arr_b, 2 + i*2)
	std::string innArcOffName = "__abi_dynelem_iaoff_" + suffix;
	auto innArcOffVar = awst::makeVarExpression(innArcOffName, u64T, _loc);
	{
		auto iX2 = awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Mult,
			u64Const("2", _loc), _loc);
		auto pos = awst::makeUInt64BinOp(u64Const("2", _loc),
			awst::UInt64BinaryOperator::Add, std::move(iX2), _loc);
		body->body.push_back(assignFresh(innArcOffVar,
			bytesExtractU16(arrVar, std::move(pos), _loc), _loc));
	}

	// inner_start = 2 + inner_arc4_off
	std::string innStartName = "__abi_dynelem_istart_" + suffix;
	auto innStartVar = awst::makeVarExpression(innStartName, u64T, _loc);
	body->body.push_back(assignFresh(innStartVar,
		awst::makeUInt64BinOp(u64Const("2", _loc), awst::UInt64BinaryOperator::Add,
			innArcOffVar, _loc), _loc));

	// inner_end = if (i+1 < n) then 2 + extract_uint16(arr_b, 2 + (i+1)*2)
	//             else total_bytes
	// Implementation: compute next_off = 2 + extract_uint16(arr_b, 2 + (i+1)*2)
	// when i+1 < n; else next_off = total_bytes. Use a temp + IfElse.
	std::string innEndName = "__abi_dynelem_iend_" + suffix;
	auto innEndVar = awst::makeVarExpression(innEndName, u64T, _loc);
	{
		// Default: inner_end = total_bytes
		body->body.push_back(assignFresh(innEndVar, totVar, _loc));

		// if (i+1 < n) inner_end = 2 + extract_uint16(arr_b, 2 + (i+1)*2)
		auto iPlus1 = awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Add,
			u64Const("1", _loc), _loc);
		auto cond = awst::makeNumericCompare(iPlus1, awst::NumericComparison::Lt, nVar, _loc);

		auto thenBlock = std::make_shared<awst::Block>();
		thenBlock->sourceLocation = _loc;
		auto iPlus1Again = awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Add,
			u64Const("1", _loc), _loc);
		auto i1X2 = awst::makeUInt64BinOp(std::move(iPlus1Again),
			awst::UInt64BinaryOperator::Mult, u64Const("2", _loc), _loc);
		auto pos = awst::makeUInt64BinOp(u64Const("2", _loc),
			awst::UInt64BinaryOperator::Add, std::move(i1X2), _loc);
		auto nxtArcOff = bytesExtractU16(arrVar, std::move(pos), _loc);
		auto nxtStart = awst::makeUInt64BinOp(u64Const("2", _loc),
			awst::UInt64BinaryOperator::Add, std::move(nxtArcOff), _loc);
		thenBlock->body.push_back(assignFresh(innEndVar, std::move(nxtStart), _loc));

		auto ifStmt = std::make_shared<awst::IfElse>();
		ifStmt->sourceLocation = _loc;
		ifStmt->condition = std::move(cond);
		ifStmt->ifBranch = std::move(thenBlock);
		body->body.push_back(std::move(ifStmt));
	}

	// inner_size = inner_end - inner_start
	std::string innSizeName = "__abi_dynelem_isz_" + suffix;
	auto innSizeVar = awst::makeVarExpression(innSizeName, u64T, _loc);
	body->body.push_back(assignFresh(innSizeVar,
		awst::makeUInt64BinOp(innEndVar, awst::UInt64BinaryOperator::Sub,
			innStartVar, _loc), _loc));

	// inner_bytes = extract3(arr_b, inner_start, inner_size)
	std::string innBytesName = "__abi_dynelem_ib_" + suffix;
	auto innBytesVar = awst::makeVarExpression(innBytesName, bytesT, _loc);
	body->body.push_back(assignFresh(innBytesVar,
		bytesExtract3(arrVar, innStartVar, innSizeVar, _loc), _loc));

	// inner_evm = encodeFromArc4Bytes(inner_bytes, _elemSolType)
	// Note: this recursive call may itself emit prePending statements.
	// Since we're inside a loop body (Block), we need to capture those
	// and inline them into the body — they shouldn't escape to the outer
	// function-level prePending. Use a temporary swap of prePending to
	// collect inner-emitter-emitted statements, then prepend them to the
	// loop body before this assignment.
	std::string innEvmName = "__abi_dynelem_iev_" + suffix;
	auto innEvmVar = awst::makeVarExpression(innEvmName, bytesT, _loc);
	{
		std::vector<std::shared_ptr<awst::Statement>> savedPre;
		savedPre.swap(_ctx.prePendingStatements);
		auto innEvm = encodeFromArc4Bytes(_ctx, innBytesVar, _elemSolType, _loc);
		// Splice any child-emitted prePending statements into body BEFORE
		// the assignment that consumes them.
		for (auto& s: _ctx.prePendingStatements)
			body->body.push_back(std::move(s));
		_ctx.prePendingStatements = std::move(savedPre);
		body->body.push_back(assignFresh(innEvmVar, std::move(innEvm), _loc));
	}

	// acc_head = concat(acc_head, leftpad32(itob(off)))
	{
		auto offPadded = leftPadBytes(u64Itob(offVar, _loc), 32, _loc);
		body->body.push_back(assignFresh(headVar,
			bytesConcat(headVar, std::move(offPadded), _loc), _loc));
	}

	// acc_tail = concat(acc_tail, inner_evm)
	body->body.push_back(assignFresh(tailVar,
		bytesConcat(tailVar, innEvmVar, _loc), _loc));

	// off += len(inner_evm)
	body->body.push_back(assignFresh(offVar,
		awst::makeUInt64BinOp(offVar, awst::UInt64BinaryOperator::Add,
			bytesLen(innEvmVar, _loc), _loc), _loc));

	// i += 1
	body->body.push_back(assignFresh(iVar,
		awst::makeUInt64BinOp(iVar, awst::UInt64BinaryOperator::Add,
			u64Const("1", _loc), _loc), _loc));

	loop->loopBody = std::move(body);
	_ctx.prePendingStatements.push_back(std::move(loop));

	// Build result: leftpad32(itob(outer_n)) ++ acc_head ++ acc_tail
	auto outerLenWord = leftPadBytes(u64Itob(nVar, _loc), 32, _loc);
	auto headTail = bytesConcat(headVar, tailVar, _loc);
	return bytesConcat(std::move(outerLenWord), std::move(headTail), _loc);
}

// Recursive entry point used from inside loop bodies. The caller has
// already extracted a bytes blob from a parent ARC4 container (so the
// expression's wtype is `bytes`); this method re-types it via
// ReinterpretCast to whatever ARC4 wtype the inner Solidity type maps
// to, so the existing `encodeDynamicTail` branches (struct → field
// access, dyn-array → length+body, etc.) see a properly-typed value
// they can structurally walk. Without this cast, e.g. the struct
// branch's `FieldExpression` constructor would fail its assertion that
// the base wtype is `ARC4Struct | WTuple`.
std::shared_ptr<awst::Expression> AbiEncoderBuilder::encodeFromArc4Bytes(
	BuilderContext& _ctx,
	std::shared_ptr<awst::Expression> _bytesExpr,
	solidity::frontend::Type const* _solType,
	awst::SourceLocation const& _loc)
{
	auto* nativeType = _ctx.typeMapper.map(_solType);
	auto const* arc4Type = _ctx.typeMapper.mapToARC4Type(nativeType);
	auto recast = awst::makeReinterpretCast(std::move(_bytesExpr), arc4Type, _loc);
	return encodeDynamicTail(_ctx, std::move(recast), _solType, _loc);
}

} // namespace puyasol::builder::eb
