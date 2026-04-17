/// @file TypeConversions.cpp
/// Solidity type conversion handlers.

#include "builder/sol-eb/TypeConversions.h"
#include "builder/sol-eb/SolAddressBuilder.h"
#include "builder/sol-eb/SolBoolBuilder.h"
#include "builder/sol-eb/SolEnumBuilder.h"
#include "builder/sol-eb/SolFixedBytesBuilder.h"
#include "builder/sol-eb/SolIntegerBuilder.h"
#include "builder/sol-types/TypeCoercion.h"

namespace puyasol::builder::eb
{

/// Simple wrapper for conversion results
class GenericConvertBuilder: public InstanceBuilder
{
public:
	GenericConvertBuilder(BuilderContext& _ctx, std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr))
	{
	}
	solidity::frontend::Type const* solType() const override { return nullptr; }
};

TypeConversionRegistry::TypeConversionRegistry()
{
	registerHandler(solidity::frontend::Type::Category::Integer, &convertToInteger);
	registerHandler(solidity::frontend::Type::Category::Bool, &convertToBool);
	registerHandler(solidity::frontend::Type::Category::Address, &convertToAddress);
	registerHandler(solidity::frontend::Type::Category::Contract, &convertToAddress);
	registerHandler(solidity::frontend::Type::Category::FixedBytes, &convertToFixedBytes);
	registerHandler(solidity::frontend::Type::Category::Enum, &convertToEnum);
}

void TypeConversionRegistry::registerHandler(
	solidity::frontend::Type::Category _cat, ConvertHandler _handler)
{
	m_handlers[static_cast<int>(_cat)] = std::move(_handler);
}

std::unique_ptr<InstanceBuilder> TypeConversionRegistry::tryConvert(
	BuilderContext& _ctx,
	solidity::frontend::Type const* _targetSolType,
	awst::WType const* _targetWType,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc) const
{
	if (!_targetSolType) return nullptr;
	auto it = m_handlers.find(static_cast<int>(_targetSolType->category()));
	if (it != m_handlers.end())
		return it->second(_ctx, _targetSolType, _targetWType, std::move(_arg), _loc);
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────
// Integer conversion: uint8(x), uint256(x), int64(x), etc.
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> TypeConversionRegistry::convertToInteger(
	BuilderContext& _ctx,
	solidity::frontend::Type const* _targetSolType,
	awst::WType const* _targetWType,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc)
{
	auto const* targetInt = dynamic_cast<solidity::frontend::IntegerType const*>(_targetSolType);
	if (!targetInt) return nullptr;

	unsigned targetBits = targetInt->numBits();
	bool targetIsBigUInt = targetBits > 64;
	auto* srcWType = _arg->wtype;

	// Same type → no-op
	if (srcWType == _targetWType)
		return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(_arg));

	// uint64 → biguint promotion
	if (!targetIsBigUInt && srcWType == awst::WType::biguintType())
	{
		// biguint → uint64: use safe extraction (btoi fails on >8 bytes)
		auto result = TypeCoercion::implicitNumericCast(std::move(_arg), awst::WType::uint64Type(), _loc);
		return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(result));
	}

	if (targetIsBigUInt && srcWType == awst::WType::uint64Type())
	{
		// uint64 → biguint: itob + ReinterpretCast
		auto result = TypeCoercion::implicitNumericCast(std::move(_arg), _targetWType, _loc);
		return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(result));
	}

	// Narrowing cast within biguint: uint160(uint256_val)
	// Need to mask: x & ((1 << targetBits) - 1)
	if (targetIsBigUInt && srcWType == awst::WType::biguintType())
	{
		// Narrowing: mask to targetBits
		if (targetBits < 256)
		{
			solidity::u256 mask = (solidity::u256(1) << targetBits) - 1;
			auto maskConst = std::make_shared<awst::IntegerConstant>();
			maskConst->sourceLocation = _loc;
			maskConst->wtype = awst::WType::biguintType();
			maskConst->value = mask.str();

			auto masked = std::make_shared<awst::BigUIntBinaryOperation>();
			masked->sourceLocation = _loc;
			masked->wtype = awst::WType::biguintType();
			masked->left = std::move(_arg);
			masked->op = awst::BigUIntBinaryOperator::BitAnd;
			masked->right = std::move(maskConst);

			return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(masked));
		}
		return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(_arg));
	}

	// bool → integer
	if (srcWType == awst::WType::boolType())
	{
		// bool is already 0/1 on AVM
		auto result = TypeCoercion::implicitNumericCast(std::move(_arg), _targetWType, _loc);
		return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(result));
	}

	// bytes[N] → integer
	if (srcWType && srcWType->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bytesWType = dynamic_cast<awst::BytesWType const*>(srcWType);
		// Dynamic-length bytes (unsized) or fixed-size > 8 bytes → biguint path.
		// btoi only handles ≤8 bytes; an unsized `bytes` from e.g. `keccak256`
		// is a 32-byte digest at runtime and must NOT go through btoi.
		bool knownSmall =
			bytesWType && bytesWType->length().has_value() && *bytesWType->length() <= 8;
		if (!knownSmall)
		{
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(_arg);
			auto result = TypeCoercion::implicitNumericCast(std::move(cast), _targetWType, _loc);
			return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(result));
		}
		// bytes[N≤8] → btoi → uint64/biguint
		auto toBytes = std::make_shared<awst::ReinterpretCast>();
		toBytes->sourceLocation = _loc;
		toBytes->wtype = awst::WType::bytesType();
		toBytes->expr = std::move(_arg);

		auto btoi = std::make_shared<awst::IntrinsicCall>();
		btoi->sourceLocation = _loc;
		btoi->wtype = awst::WType::uint64Type();
		btoi->opCode = "btoi";
		btoi->stackArgs.push_back(std::move(toBytes));

		auto result = TypeCoercion::implicitNumericCast(std::move(btoi), _targetWType, _loc);
		return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(result));
	}

	// General: try implicit numeric cast
	auto result = TypeCoercion::implicitNumericCast(std::move(_arg), _targetWType, _loc);
	return std::make_unique<SolIntegerBuilder>(_ctx, targetInt, std::move(result));
}

// ─────────────────────────────────────────────────────────────────────
// Bool conversion: bool(x)
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> TypeConversionRegistry::convertToBool(
	BuilderContext& _ctx,
	solidity::frontend::Type const* /*_targetSolType*/,
	awst::WType const* /*_targetWType*/,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc)
{
	// integer → bool: x != 0
	if (_arg->wtype == awst::WType::uint64Type() || _arg->wtype == awst::WType::biguintType())
	{
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = _arg->wtype;
		zero->value = "0";

		auto cmp = std::make_shared<awst::NumericComparisonExpression>();
		cmp->sourceLocation = _loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(_arg);
		cmp->rhs = std::move(zero);
		cmp->op = awst::NumericComparison::Ne;

		return std::make_unique<SolBoolBuilder>(_ctx, std::move(cmp));
	}

	// Already bool → pass through
	if (_arg->wtype == awst::WType::boolType())
		return std::make_unique<SolBoolBuilder>(_ctx, std::move(_arg));

	return nullptr; // unhandled conversion
}

// ─────────────────────────────────────────────────────────────────────
// Address conversion: address(x)
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> TypeConversionRegistry::leftPadToN(
	std::shared_ptr<awst::Expression> _expr,
	int _n,
	awst::SourceLocation const& _loc)
{
	auto nConst = std::make_shared<awst::IntegerConstant>();
	nConst->sourceLocation = _loc;
	nConst->wtype = awst::WType::uint64Type();
	nConst->value = std::to_string(_n);

	auto padding = std::make_shared<awst::IntrinsicCall>();
	padding->sourceLocation = _loc;
	padding->wtype = awst::WType::bytesType();
	padding->opCode = "bzero";
	padding->stackArgs.push_back(nConst);

	auto padded = std::make_shared<awst::IntrinsicCall>();
	padded->sourceLocation = _loc;
	padded->wtype = awst::WType::bytesType();
	padded->opCode = "concat";
	padded->stackArgs.push_back(std::move(padding));
	padded->stackArgs.push_back(std::move(_expr));

	auto paddedLen = std::make_shared<awst::IntrinsicCall>();
	paddedLen->sourceLocation = _loc;
	paddedLen->wtype = awst::WType::uint64Type();
	paddedLen->opCode = "len";
	paddedLen->stackArgs.push_back(padded);

	auto nConst2 = std::make_shared<awst::IntegerConstant>();
	nConst2->sourceLocation = _loc;
	nConst2->wtype = awst::WType::uint64Type();
	nConst2->value = std::to_string(_n);

	auto offset = std::make_shared<awst::UInt64BinaryOperation>();
	offset->sourceLocation = _loc;
	offset->wtype = awst::WType::uint64Type();
	offset->left = std::move(paddedLen);
	offset->op = awst::UInt64BinaryOperator::Sub;
	offset->right = std::move(nConst2);

	auto nConst3 = std::make_shared<awst::IntegerConstant>();
	nConst3->sourceLocation = _loc;
	nConst3->wtype = awst::WType::uint64Type();
	nConst3->value = std::to_string(_n);

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(std::move(padded));
	extract->stackArgs.push_back(std::move(offset));
	extract->stackArgs.push_back(std::move(nConst3));
	return extract;
}

std::unique_ptr<InstanceBuilder> TypeConversionRegistry::convertToAddress(
	BuilderContext& _ctx,
	solidity::frontend::Type const* _targetSolType,
	awst::WType const* /*_targetWType*/,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc)
{
	auto* srcWType = _arg->wtype;

	// Already account → no-op
	if (srcWType == awst::WType::accountType())
		return std::make_unique<SolAddressBuilder>(_ctx, _targetSolType, std::move(_arg));

	// Integer → left-pad to 32 bytes → account
	if (srcWType == awst::WType::uint64Type() || srcWType == awst::WType::biguintType())
	{
		auto promoted = TypeCoercion::implicitNumericCast(
			std::move(_arg), awst::WType::biguintType(), _loc);
		auto toBytes = std::make_shared<awst::ReinterpretCast>();
		toBytes->sourceLocation = _loc;
		toBytes->wtype = awst::WType::bytesType();
		toBytes->expr = std::move(promoted);

		auto padded = leftPadToN(std::move(toBytes), 32, _loc);

		auto result = std::make_shared<awst::ReinterpretCast>();
		result->sourceLocation = _loc;
		result->wtype = awst::WType::accountType();
		result->expr = std::move(padded);
		return std::make_unique<SolAddressBuilder>(_ctx, _targetSolType, std::move(result));
	}

	// Bytes → reinterpret as account
	if (srcWType == awst::WType::bytesType()
		|| (srcWType && srcWType->kind() == awst::WTypeKind::Bytes))
	{
		auto result = std::make_shared<awst::ReinterpretCast>();
		result->sourceLocation = _loc;
		result->wtype = awst::WType::accountType();
		result->expr = std::move(_arg);
		return std::make_unique<SolAddressBuilder>(_ctx, _targetSolType, std::move(result));
	}

	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────
// FixedBytes conversion: bytes32(x), bytes4(x), etc.
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> TypeConversionRegistry::convertToFixedBytes(
	BuilderContext& _ctx,
	solidity::frontend::Type const* _targetSolType,
	awst::WType const* _targetWType,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc)
{
	auto const* fbType = dynamic_cast<solidity::frontend::FixedBytesType const*>(_targetSolType);
	if (!fbType) return nullptr;

	auto* srcWType = _arg->wtype;

	// Same type → no-op
	if (srcWType == _targetWType)
		return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType, std::move(_arg));

	// Integer → itob (+ padding/truncation for byte width)
	if (srcWType == awst::WType::uint64Type())
	{
		unsigned byteWidth = fbType->numBytes();
		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = _loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(_arg));

		std::shared_ptr<awst::Expression> result;
		if (byteWidth < 8)
		{
			// Truncate: extract last byteWidth bytes from 8-byte itob result
			auto off = std::make_shared<awst::IntegerConstant>();
			off->sourceLocation = _loc;
			off->wtype = awst::WType::uint64Type();
			off->value = std::to_string(8 - byteWidth);
			auto len = std::make_shared<awst::IntegerConstant>();
			len->sourceLocation = _loc;
			len->wtype = awst::WType::uint64Type();
			len->value = std::to_string(byteWidth);

			auto extract = std::make_shared<awst::IntrinsicCall>();
			extract->sourceLocation = _loc;
			extract->wtype = awst::WType::bytesType();
			extract->opCode = "extract3";
			extract->stackArgs.push_back(std::move(itob));
			extract->stackArgs.push_back(std::move(off));
			extract->stackArgs.push_back(std::move(len));
			result = std::move(extract);
		}
		else if (byteWidth > 8)
		{
			// Pad: concat(bzero(byteWidth), itob) → extract last byteWidth
			result = leftPadToN(std::move(itob), byteWidth, _loc);
		}
		else
			result = std::move(itob);

		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = _targetWType;
		cast->expr = std::move(result);
		return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType, std::move(cast));
	}

	// Biguint → bytes → pad/truncate
	if (srcWType == awst::WType::biguintType())
	{
		unsigned byteWidth = fbType->numBytes();
		auto toBytes = std::make_shared<awst::ReinterpretCast>();
		toBytes->sourceLocation = _loc;
		toBytes->wtype = awst::WType::bytesType();
		toBytes->expr = std::move(_arg);

		auto padded = leftPadToN(std::move(toBytes), byteWidth, _loc);

		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = _targetWType;
		cast->expr = std::move(padded);
		return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType, std::move(cast));
	}

	// FixedBytes[M] → FixedBytes[N]: pad or truncate
	if (srcWType && srcWType->kind() == awst::WTypeKind::Bytes)
	{
		auto const* srcBytes = dynamic_cast<awst::BytesWType const*>(srcWType);
		int srcLen = srcBytes && srcBytes->length() ? *srcBytes->length() : 0;
		int tgtLen = static_cast<int>(fbType->numBytes());

		if (srcLen > 0 && tgtLen > 0 && srcLen != tgtLen)
		{
			// Convert to raw bytes first
			auto toBytes = std::make_shared<awst::ReinterpretCast>();
			toBytes->sourceLocation = _loc;
			toBytes->wtype = awst::WType::bytesType();
			toBytes->expr = std::move(_arg);

			std::shared_ptr<awst::Expression> result;
			if (tgtLen > srcLen)
			{
				// Right-pad: concat(input, bzero(N-M))
				auto padSize = std::make_shared<awst::IntegerConstant>();
				padSize->sourceLocation = _loc;
				padSize->wtype = awst::WType::uint64Type();
				padSize->value = std::to_string(tgtLen - srcLen);
				auto pad = std::make_shared<awst::IntrinsicCall>();
				pad->sourceLocation = _loc;
				pad->wtype = awst::WType::bytesType();
				pad->opCode = "bzero";
				pad->stackArgs.push_back(std::move(padSize));
				auto cat = std::make_shared<awst::IntrinsicCall>();
				cat->sourceLocation = _loc;
				cat->wtype = awst::WType::bytesType();
				cat->opCode = "concat";
				cat->stackArgs.push_back(std::move(toBytes));
				cat->stackArgs.push_back(std::move(pad));
				result = std::move(cat);
			}
			else
			{
				// Left-truncate: extract(0, N)
				auto zero = std::make_shared<awst::IntegerConstant>();
				zero->sourceLocation = _loc;
				zero->wtype = awst::WType::uint64Type();
				zero->value = "0";
				auto len = std::make_shared<awst::IntegerConstant>();
				len->sourceLocation = _loc;
				len->wtype = awst::WType::uint64Type();
				len->value = std::to_string(tgtLen);
				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = _loc;
				extract->wtype = awst::WType::bytesType();
				extract->opCode = "extract3";
				extract->stackArgs.push_back(std::move(toBytes));
				extract->stackArgs.push_back(std::move(zero));
				extract->stackArgs.push_back(std::move(len));
				result = std::move(extract);
			}
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = _targetWType;
			cast->expr = std::move(result);
			return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType, std::move(cast));
		}

		// Same size or unsized → reinterpret
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = _targetWType;
		cast->expr = std::move(_arg);
		return std::make_unique<SolFixedBytesBuilder>(_ctx, fbType, std::move(cast));
	}

	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────
// Enum conversion: MyEnum(x)
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> TypeConversionRegistry::convertToEnum(
	BuilderContext& _ctx,
	solidity::frontend::Type const* _targetSolType,
	awst::WType const* /*_targetWType*/,
	std::shared_ptr<awst::Expression> _arg,
	awst::SourceLocation const& _loc)
{
	auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(_targetSolType);
	if (!enumType) return nullptr;

	// Coerce to uint64
	auto result = TypeCoercion::implicitNumericCast(
		std::move(_arg), awst::WType::uint64Type(), _loc);

	// EVM reverts with Panic(0x21) if value >= numMembers
	unsigned numMembers = enumType->numberOfMembers();
	auto maxVal = std::make_shared<awst::IntegerConstant>();
	maxVal->sourceLocation = _loc;
	maxVal->wtype = awst::WType::uint64Type();
	maxVal->value = std::to_string(numMembers);

	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = result; // shared_ptr ref
	cmp->op = awst::NumericComparison::Lt;
	cmp->rhs = std::move(maxVal);

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = _loc;
	stmt->expr = awst::makeAssert(std::move(cmp), _loc, "enum out of range");
	_ctx.prePendingStatements.push_back(std::move(stmt));

	return std::make_unique<SolEnumBuilder>(_ctx, enumType, std::move(result));
}

} // namespace puyasol::builder::eb
