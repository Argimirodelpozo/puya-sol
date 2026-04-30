/// @file AbiEncoderBuilder.cpp
/// Handles abi.encode*, abi.decode — extracted from FunctionCallBuilder.

#include "builder/sol-eb/AbiEncoderBuilder.h"
#include "builder/sol-eb/AbiCodecHelpers.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::eb
{
using namespace abi_codec;
}
namespace puyasol::builder::eb
{

class GenericAbiResult: public InstanceBuilder
{
public:
	GenericAbiResult(BuilderContext& _ctx, std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)) {}
	solidity::frontend::Type const* solType() const override { return nullptr; }
};


std::shared_ptr<awst::Expression> AbiEncoderBuilder::leftPadBytes(
	std::shared_ptr<awst::Expression> _expr, int _n, awst::SourceLocation const& _loc)
{
	auto pad = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	pad->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(_n), _loc));

	auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), _loc);
	cat->stackArgs.push_back(std::move(pad));
	cat->stackArgs.push_back(std::move(_expr));

	auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
	lenCall->stackArgs.push_back(cat);

	auto offset = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), _loc);
	offset->stackArgs.push_back(std::move(lenCall));
	offset->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(_n), _loc));

	auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	extract->stackArgs.push_back(std::move(cat));
	extract->stackArgs.push_back(std::move(offset));
	extract->stackArgs.push_back(awst::makeIntegerConstant(std::to_string(_n), _loc));
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
		boolToInt->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
		boolToInt->stackArgs.push_back(awst::makeIntegerConstant("1", _loc));
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
		setbit->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
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

		auto zero = awst::makeIntegerConstant("0", _loc);
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
		head->stackArgs.push_back(awst::makeIntegerConstant("0", _loc));
		head->stackArgs.push_back(awst::makeIntegerConstant("32", _loc));
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
			auto offset = awst::makeIntegerConstant(std::to_string(i * 32), _loc);
			items.push_back(decodeAbiValue(_ctx, dataExpr, std::move(offset), components[i], _loc));
		}
		auto tuple = std::make_shared<awst::TupleExpression>();
		tuple->sourceLocation = _loc;
		tuple->wtype = targetType;
		tuple->items = std::move(items);
		return std::make_unique<GenericAbiResult>(_ctx, std::move(tuple));
	}

	// Single value decode at offset 0
	auto offset = awst::makeIntegerConstant("0", _loc);
	auto result = decodeAbiValue(_ctx, dataExpr, std::move(offset), callType, _loc);
	return std::make_unique<GenericAbiResult>(_ctx, std::move(result));
}

// ── uint64FromAbiWord: extract uint64 from 32-byte ABI word ──


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
	std::shared_ptr<awst::Expression> currentTailOffset = awst::makeIntegerConstant(std::to_string(headSize), _loc);

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



} // namespace puyasol::builder::eb
