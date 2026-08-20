/// @file SolFixedBytesBuilder.cpp
/// Solidity fixed-size bytes type builder (bytes1..bytes32).

#include "builder/sol-eb/SolFixedBytesBuilder.h"
#include "builder/sol-eb/BigUIntMathHelpers.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"

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
	awst::SourceLocation const& _loc, bool _reverse)
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
		if (_reverse)
			return nullptr;                                 // a uint shifted BY a bytesN is invalid Solidity
		int n = static_cast<int>(m_bytesType->numBytes());
		auto value = awst::makeAsBiguint(resolve(), _loc);
		auto shiftAmt = shiftAmountToUint64(_other.resolve(), _loc);
		auto shifted = buildBigUIntShift(std::move(value), std::move(shiftAmt),
			_op == BuilderBinaryOp::LShift, _loc);
		auto trimmed = awst::makeLeftPadToN(awst::makeAsBytes(std::move(shifted), _loc), n, _loc);
		// Retag with the SIZED bytes[N] wtype: the expression otherwise carries plain
		// unsized `bytes`, and bytesN(M→N) NARROWING of it (convertToFixedBytes) can't
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
	if (_reverse)
		std::swap(lhs, rhs);

	// Mixed widths are legal Solidity: bytesM implicitly widens to bytesN by
	// RIGHT-padding (`bytes2 & bytes4` operates at bytes4 with the bytes2
	// left-aligned). AVM b&/b|/b^ zero-fill the shorter operand on the LEFT
	// instead — pad the short side explicitly and type the result at the
	// common width.
	auto declaredLen = [](awst::Expression const& e) -> size_t {
		if (auto const* bw = dynamic_cast<awst::BytesWType const*>(e.wtype))
			if (bw->length().has_value())
				return *bw->length();
		return 0;
	};
	size_t common = std::max<size_t>(m_bytesType->numBytes(),
		std::max(declaredLen(*lhs), declaredLen(*rhs)));
	auto padTo = [&](std::shared_ptr<awst::Expression>& expr) {
		size_t len = declaredLen(*expr);
		if (len == 0 || len >= common)
			return;
		expr = awst::makeConcat(
			awst::makeAsBytes(std::move(expr), _loc),
			awst::makeBzero(static_cast<int>(common - len), _loc), _loc);
	};
	padTo(lhs);
	padTo(rhs);

	awst::BytesBinaryOperator bytesOp = awst::BytesBinaryOperator::BitOr;
	switch (_op)
	{
	case BuilderBinaryOp::BitOr: bytesOp = awst::BytesBinaryOperator::BitOr; break;
	case BuilderBinaryOp::BitXor: bytesOp = awst::BytesBinaryOperator::BitXor; break;
	case BuilderBinaryOp::BitAnd: bytesOp = awst::BytesBinaryOperator::BitAnd; break;
	default: break;
	}
	auto e = awst::makeBytesBinOp(std::move(lhs), bytesOp, std::move(rhs), _loc);
	// Retag with the sized bytes[common] wtype (same reason as the shift branch
	// above: `bytes4(a & b)` narrowing no-op'd on the unsized result).
	auto* sized = m_ctx.typeMapper.createType<awst::BytesWType>(
		static_cast<int>(common));
	auto retagged = awst::makeReinterpretCast(std::move(e), sized, _loc);
	auto const* resultType = common == m_bytesType->numBytes()
		? m_bytesType
		: solidity::frontend::TypeProvider::fixedBytes(
			static_cast<unsigned>(common));
	return std::make_unique<SolFixedBytesBuilder>(m_ctx, resultType, std::move(retagged));
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

	// EVM bytesN compares 32-byte LEFT-ALIGNED words; bytes3("abc")==bytes4("abc")
	// is true and "b" > "aa" (0x62.. > 0x6161..). AVM operands are N raw bytes, so
	// right-pad the shorter side to the common declared width: constants fold at
	// compile time (including bare string literals — "aa" == x with x bytes22
	// arrives as a 2-byte StringConstant); RUNTIME operands pad too — solc
	// legally widens bytesM→bytesN (`bytes2 a == bytes4 b`), and skipping them
	// compared 2 raw bytes against 4.
	auto bytesLen = [](awst::Expression const& e) -> size_t {
		if (auto const* bw = dynamic_cast<awst::BytesWType const*>(e.wtype))
			if (bw->length().has_value())
				return *bw->length();
		return 0;
	};
	auto padOperand = [&](std::shared_ptr<awst::Expression>& expr, size_t targetLen) {
		auto* newType = m_ctx.typeMapper.createType<awst::BytesWType>(
			static_cast<int>(targetLen));
		if (auto* bc = dynamic_cast<awst::BytesConstant*>(expr.get()))
		{
			if (bc->value.size() >= targetLen)
				return;
			std::vector<uint8_t> val = bc->value;
			val.resize(targetLen, 0);
			expr = awst::makeBytesConstant(
				std::move(val), expr->sourceLocation, bc->encoding, newType);
			return;
		}
		if (auto* sc = dynamic_cast<awst::StringConstant*>(expr.get()))
		{
			if (sc->value.size() > targetLen)
				return;
			std::vector<uint8_t> val(sc->value.begin(), sc->value.end());
			val.resize(targetLen, 0);
			expr = awst::makeBytesConstant(
				std::move(val), expr->sourceLocation, awst::BytesEncoding::Utf8, newType);
			return;
		}
		size_t len = bytesLen(*expr);
		if (len == 0 || len >= targetLen)
			return;
		auto loc = expr->sourceLocation;
		expr = awst::makeReinterpretCast(
			awst::makeConcat(
				awst::makeAsBytes(std::move(expr), loc),
				awst::makeBzero(static_cast<int>(targetLen - len), loc), loc),
			newType, loc);
	};
	size_t common = std::max(bytesLen(*lhs), bytesLen(*rhs));
	if (common > 0)
	{
		padOperand(lhs, common);
		padOperand(rhs, common);
	}

	if (_op == BuilderComparisonOp::Eq || _op == BuilderComparisonOp::Ne)
	{
		auto coerceToBytes = [&](std::shared_ptr<awst::Expression>& expr) {
			if (expr->wtype != awst::WType::bytesType()
				&& expr->wtype != awst::WType::accountType())
			{
				auto cast = awst::makeAsBytes(std::move(expr), _loc);
				expr = std::move(cast);
			}
		};
		if (lhs->wtype != rhs->wtype)
		{
			coerceToBytes(lhs);
			coerceToBytes(rhs);
		}

		auto e = awst::makeBytesComparison(std::move(lhs),
			(_op == BuilderComparisonOp::Eq) ? awst::EqualityComparison::Eq : awst::EqualityComparison::Ne,
			std::move(rhs), _loc);
		return std::make_unique<SolFixedBytesBuilder>(m_ctx, m_bytesType, std::move(e));
	}

	std::string opCode;
	switch (_op)
	{
	case BuilderComparisonOp::Lt: opCode = "b<"; break;
	case BuilderComparisonOp::Lte: opCode = "b<="; break;
	case BuilderComparisonOp::Gt: opCode = "b>"; break;
	case BuilderComparisonOp::Gte: opCode = "b>="; break;
	default: return nullptr;
	}

	// Ordered compares need the same left-aligned padding (lhs/rhs padded above):
	// b< is numeric big-endian, so an unpadded "b" (0x62) would sort BELOW "aa"
	// (0x6161) where EVM's left-aligned words sort it above.
	auto e = awst::makeIntrinsicCall(std::move(opCode), awst::WType::boolType(), _loc);
	e->stackArgs.push_back(std::move(lhs));
	e->stackArgs.push_back(std::move(rhs));
	return std::make_unique<SolFixedBytesBuilder>(m_ctx, m_bytesType, std::move(e));
}

std::unique_ptr<InstanceBuilder> SolFixedBytesBuilder::bool_eval(
	awst::SourceLocation const& _loc, bool _negate)
{
	auto zero = awst::makeBytesConstant(
		std::vector<uint8_t>(m_numBytes, 0), _loc, awst::BytesEncoding::Base16,
		m_expr->wtype); // same bytes[N] type

	auto e = awst::makeBytesComparison(resolve(),
		_negate ? awst::EqualityComparison::Eq : awst::EqualityComparison::Ne,
		std::move(zero), _loc);
	return std::make_unique<SolFixedBytesBuilder>(m_ctx, m_bytesType, std::move(e));
}

} // namespace puyasol::builder::eb
