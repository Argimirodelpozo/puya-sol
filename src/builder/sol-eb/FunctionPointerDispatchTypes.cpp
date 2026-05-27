#include "builder/sol-eb/FunctionPointerDispatchTypes.h"
#include "builder/sol-eb/FunctionPointerBuilder.h"
#include "builder/sol-types/TypeMapper.h"

namespace puyasol::builder::eb
{

using namespace solidity::frontend;

awst::WType const* computeReturnType(ContractContext& _ctx, FunctionType const* _funcType)
{
	if (!_funcType || _funcType->returnParameterTypes().empty())
		return awst::WType::voidType();
	auto const& rts = _funcType->returnParameterTypes();
	if (rts.size() == 1)
		return _ctx.typeMapper.map(rts[0]);
	std::vector<awst::WType const*> retTypes;
	for (auto const* rt : rts)
		retTypes.push_back(_ctx.typeMapper.map(rt));
	return _ctx.typeMapper.createType<awst::WTuple>(std::move(retTypes), std::nullopt);
}

awst::WType const* dispatchPublicArgArc4Type(
	awst::WType const* _nativeType, solidity::frontend::Type const* _paramSolType)
{
	if (_nativeType == awst::WType::biguintType())
	{
		unsigned bits = 256;
		if (auto const* intType = dynamic_cast<IntegerType const*>(_paramSolType))
			bits = intType->numBits();
		return new awst::ARC4UIntN(static_cast<int>(bits));
	}
	if (_nativeType && _nativeType->kind() == awst::WTypeKind::Bytes
		&& dynamic_cast<FunctionType const*>(_paramSolType))
	{
		// External fn-ptr bytes[12] → arc4.static_array<arc4.uint8, 12>.
		// ContractBuilder only ARC4-remaps bytes[N] params when the Solidity
		// type is FunctionType (see ContractBuilder.cpp isAggregate check);
		// matching that rule here so the dispatch call-site wraps iff the
		// target's signature expects an ARC4 arg.
		auto const* bytesType = static_cast<awst::BytesWType const*>(_nativeType);
		if (bytesType->length().has_value())
		{
			auto const* arc4Byte = new awst::ARC4UIntN(8);
			return new awst::ARC4StaticArray(arc4Byte, bytesType->length().value());
		}
	}
	return nullptr;
}

awst::WType const* mapDispatchType(
	solidity::frontend::Type const* _solType, bool _promoteSignedI64Biguint)
{
	if (auto const* intType = dynamic_cast<IntegerType const*>(_solType))
	{
		if (intType->numBits() <= 64 && _promoteSignedI64Biguint && intType->isSigned())
			return awst::WType::biguintType();
		if (intType->numBits() <= 64)
			return awst::WType::uint64Type();
		return awst::WType::biguintType();
	}
	if (dynamic_cast<BoolType const*>(_solType))
		return awst::WType::boolType();
	if (auto const* arrType = dynamic_cast<ArrayType const*>(_solType))
	{
		if (arrType->isString())
			return awst::WType::stringType();
		if (arrType->isByteArray())
			return awst::WType::bytesType();
		return awst::WType::biguintType();
	}
	if (_solType && _solType->category() == Type::Category::StringLiteral)
		return awst::WType::stringType();
	if (auto const* fnType = dynamic_cast<FunctionType const*>(_solType))
		return FunctionPointerBuilder::mapFunctionType(fnType);
	if (auto const* fbType = dynamic_cast<FixedBytesType const*>(_solType))
		return new awst::BytesWType(static_cast<int>(fbType->numBytes()));
	return awst::WType::biguintType();
}

std::shared_ptr<awst::Expression> encodeArgForInnerTxn(
	std::shared_ptr<awst::Expression> _argExpr,
	solidity::frontend::Type const* _paramSolType,
	awst::SourceLocation const& _loc)
{
	unsigned targetBits = 256;
	if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(_paramSolType))
		targetBits = intType->numBits();
	unsigned targetBytes = targetBits / 8;

	if (_argExpr->wtype == awst::WType::uint64Type())
	{
		std::shared_ptr<awst::Expression> bytesExpr = awst::makeItob(std::move(_argExpr), _loc);
		if (targetBytes > 8 && dynamic_cast<solidity::frontend::IntegerType const*>(_paramSolType))
			bytesExpr = awst::makeZeroExtendToN(
				std::move(bytesExpr), static_cast<int>(targetBytes), _loc);
		return bytesExpr;
	}
	if (_argExpr->wtype == awst::WType::biguintType())
	{
		auto raw = awst::makeAsBytes(std::move(_argExpr), _loc);
		if (dynamic_cast<solidity::frontend::IntegerType const*>(_paramSolType))
			return awst::makeZeroExtendToN(
				std::move(raw), static_cast<int>(targetBytes), _loc);
		return raw;
	}
	if (_argExpr->wtype == awst::WType::boolType())
	{
		// ARC4 bool: 1 byte, 0x80 = true, 0x00 = false.
		return awst::makeSetbit(
			awst::makeBytesConstant(std::vector<uint8_t>{0}, _loc),
			awst::makeZero(_loc),
			std::move(_argExpr), _loc);
	}
	if (auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(_paramSolType);
		arrType && arrType->isByteArrayOrString())
	{
		// ARC4 byte[] encoding: uint16(length) ++ raw_bytes.
		if (_argExpr->wtype != awst::WType::bytesType())
			_argExpr = awst::makeAsBytes(std::move(_argExpr), _loc);
		auto header = awst::makeExtract(awst::makeItob(awst::makeLen(_argExpr, _loc), _loc), 6, 2, _loc);
		return awst::makeConcat(std::move(header), std::move(_argExpr), _loc);
	}
	// Fallback: reinterpret as bytes.
	if (_argExpr->wtype != awst::WType::bytesType())
		return awst::makeAsBytes(std::move(_argExpr), _loc);
	return _argExpr;
}

} // namespace puyasol::builder::eb
