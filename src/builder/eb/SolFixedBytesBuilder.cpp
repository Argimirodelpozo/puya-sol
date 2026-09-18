/// @file SolFixedBytesBuilder.cpp
/// Solidity fixed-size bytes type builder (bytes1..bytes32).

#include "builder/eb/SolFixedBytesBuilder.h"
#include "builder/eb/SolBoolBuilder.h"
#include "builder/eb/BinaryOpBuilder.h"
#include "builder/eb/BigUIntMathHelpers.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/TypeMapper.h"

#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::eb
{

SolFixedBytesBuilder::SolFixedBytesBuilder(
	ContractContext& _ctx,
	solidity::frontend::FixedBytesType const* _bytesType,
	std::shared_ptr<awst::Expression> _expr)
	: InstanceBuilder(_ctx, std::move(_expr)),
	  m_bytesType(_bytesType),
	  m_numBytes(_bytesType->numBytes())
{
}

std::unique_ptr<InstanceBuilder> SolFixedBytesBuilder::binary_op(
	InstanceBuilder& _other, BuilderBinaryOp _op,
	awst::SourceLocation const& _loc)
{
	// bytesN bit shift: `b << k` / `b >> k` shift the N-byte value by k BITS (k is a uint, not bytes),
	// truncating the result to N bytes. Lower via biguint — (asBiguint(b) shifted) then keep the LOW N
	// bytes — instead of the generic integer path, which coerces bytes->uint64->bytes (puya rejects the
	// uint64->bytes cast, and uint64 can't hold bytes>8 anyway). buildBigUIntShift already saturates a
	// shift >= 256 to 0 (shiftAmountToUint64 clamps a biguint amount so huge >=2^64 amounts saturate
	// too, instead of shifting by amount mod 2^64); makeLeftPadToN does the mod-2^(8N) truncation
	// (drops the high bytes).
	if (_op == BuilderBinaryOp::LShift || _op == BuilderBinaryOp::RShift)
	{
		int n = static_cast<int>(m_bytesType->numBytes());
		auto value = awst::makeAsBiguint(resolve(), _loc);
		auto shiftAmt = shiftAmountToUint64(_other.resolve(), _loc);
		auto shifted = buildBigUIntShift(std::move(value), std::move(shiftAmt),
			_op == BuilderBinaryOp::LShift, _loc);
		auto trimmed = awst::makeLeftPadToN(awst::makeAsBytes(std::move(shifted), _loc), n, _loc);
		// Retag with the SIZED bytes[N] wtype: the expression otherwise carries plain
		// unsized `bytes`, and bytesN(M→N) NARROWING of it (coerceScalar) can't
		// see the source length → degenerated to a reinterpret no-op, so
		// `uint32(bytes4(b32 << k))` btoi'd all 32 bytes and reverted.
		auto* sized = m_ctx.typeMapper.createType<awst::BytesWType>(n);
		auto retagged = awst::makeReinterpretCast(std::move(trimmed), sized, _loc);
		return std::make_unique<SolFixedBytesBuilder>(m_ctx, m_bytesType, std::move(retagged));
	}

	bool isBitwiseOp = (_op == BuilderBinaryOp::BitOr
		|| _op == BuilderBinaryOp::BitXor
		|| _op == BuilderBinaryOp::BitAnd);
	if (!isBitwiseOp)
		return nullptr;

	// Accept other bytes-backed types
	bool otherIsBytes = _other.wtype() && _other.wtype()->kind() == awst::WTypeKind::Bytes;
	if (!otherIsBytes)
		return nullptr;

	auto lhs = resolve();
	auto rhs = _other.resolve();

	// The AST supplies solc's common type (compound assignments use their
	// destination type). Byte strings widen on the right, unlike AVM b&/b|.
	auto* sized = m_ctx.typeMapper.map(m_bytesType);
	lhs = TypeCoercion::coerceScalar(std::move(lhs), sized, _loc);
	rhs = TypeCoercion::coerceScalar(std::move(rhs), sized, _loc);

	awst::BytesBinaryOperator bytesOp = awst::BytesBinaryOperator::BitOr;
	switch (_op)
	{
	case BuilderBinaryOp::BitOr: bytesOp = awst::BytesBinaryOperator::BitOr; break;
	case BuilderBinaryOp::BitXor: bytesOp = awst::BytesBinaryOperator::BitXor; break;
	case BuilderBinaryOp::BitAnd: bytesOp = awst::BytesBinaryOperator::BitAnd; break;
	default: break;
	}
	auto e = awst::makeBytesBinOp(std::move(lhs), bytesOp, std::move(rhs), _loc);
	return std::make_unique<SolFixedBytesBuilder>(m_ctx, m_bytesType,
		awst::makeReinterpretCast(std::move(e), sized, _loc));
}

void padBytesOperandsToCommonWidth(
	ContractContext& _ctx,
	std::shared_ptr<awst::Expression>& _lhs,
	std::shared_ptr<awst::Expression>& _rhs)
{
	auto width = std::max(awst::fixedBytesLength(_lhs->wtype).value_or(0),
		awst::fixedBytesLength(_rhs->wtype).value_or(0));
	if (!width) return;
	auto* type = _ctx.typeMapper.createType<awst::BytesWType>(width);
	for (auto* operand: {&_lhs, &_rhs})
	{
		auto const loc = (*operand)->sourceLocation;
		*operand = TypeCoercion::coerceScalar(std::move(*operand), type, loc);
	}
}

std::unique_ptr<InstanceBuilder> SolFixedBytesBuilder::compare(
	InstanceBuilder& _other, BuilderComparisonOp _op,
	awst::SourceLocation const& _loc)
{
	bool otherIsBytes = _other.wtype() && _other.wtype()->kind() == awst::WTypeKind::Bytes;
	bool otherIsAccount = _other.wtype() == awst::WType::accountType();
	if (!otherIsBytes && !otherIsAccount)
		return nullptr;

	auto lhs = resolve();
	auto rhs = _other.resolve();

	// EVM left-aligned bytesN compare: pad both sides to the common declared
	// width (equality AND the ordered b</b> intrinsics below need it).
	padBytesOperandsToCommonWidth(m_ctx, lhs, rhs);

	return std::make_unique<SolBoolBuilder>(m_ctx,
		buildBytesComparison(_op, std::move(lhs), std::move(rhs), _loc));
}

} // namespace puyasol::builder::eb
