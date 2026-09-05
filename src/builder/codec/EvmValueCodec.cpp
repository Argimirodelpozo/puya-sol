#include "builder/codec/EvmValueCodec.h"

#include "Logger.h"
#include "builder/sol-types/FunctionPointerKind.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"

// canRoundTripEvmAbi walks StructDefinition members and TupleType components.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder::codec
{
using namespace solidity::frontend;

Type const* underlyingType(Type const* type)
{
	while (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(type))
		type = &udvt->underlyingType();
	return type;
}

bool isWordType(Type const* type)
{
	type = underlyingType(type);
	return dynamic_cast<IntegerType const*>(type)
		|| dynamic_cast<BoolType const*>(type)
		|| dynamic_cast<AddressType const*>(type)
		|| dynamic_cast<ContractType const*>(type)
		|| dynamic_cast<EnumType const*>(type)
		|| dynamic_cast<FixedBytesType const*>(type)
		|| isExternalFunctionPointer(dynamic_cast<FunctionType const*>(type));
}

namespace
{
std::shared_ptr<awst::Expression> rawBytes(
	std::shared_ptr<awst::Expression> value,
	awst::SourceLocation const& loc)
{
	if (value->wtype == awst::WType::bytesType())
		return value;
	if (value->wtype == awst::WType::uint64Type())
		return awst::makeItob(std::move(value), loc);
	return awst::makeAsBytes(std::move(value), loc);
}

std::shared_ptr<awst::Expression> boolWord(
	std::shared_ptr<awst::Expression> value,
	awst::SourceLocation const& loc)
{
	auto oneOrZero = awst::makeConditional(
		std::move(value), awst::makeOne(loc), awst::makeZero(loc),
		awst::WType::uint64Type(), loc);
	return awst::makeLeftPadToN(awst::makeItob(std::move(oneOrZero), loc), 32, loc);
}

void assertZeroBytes(
	std::shared_ptr<awst::Expression> bytes,
	int count,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out,
	std::string message)
{
	if (count <= 0)
		return;
	out.push_back(awst::makeExpressionStatement(
		awst::makeAssert(
			awst::makeBytesComparison(std::move(bytes),
				awst::EqualityComparison::Eq, awst::makeBzero(count, loc), loc),
			loc, std::move(message)), loc));
}

void assertIntegerPadding(
	std::shared_ptr<awst::Expression> value,
	IntegerType const& integer,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out)
{
	int const width = static_cast<int>(integer.numBits() / 8);
	int const prefixLength = 32 - width;
	if (prefixLength == 0)
		return;
	auto prefix = awst::makeExtract(value, 0, prefixLength, loc);
	if (!integer.isSigned())
	{
		assertZeroBytes(std::move(prefix), prefixLength, loc, out,
			"invalid EVM ABI unsigned integer padding");
		return;
	}
	auto signByte = awst::makeBtoi(
		awst::makeExtract(value, prefixLength, 1, loc), loc);
	auto negative = awst::makeNumericCompare(
		std::move(signByte), awst::NumericComparison::Gte,
		awst::makeIntegerConstant(uint64_t{128}, loc), loc);
	auto expected = awst::makeConditional(
		std::move(negative),
		awst::makeBytesConstant(
			std::vector<uint8_t>(static_cast<size_t>(prefixLength), 0xff), loc),
		awst::makeBzero(prefixLength, loc), awst::WType::bytesType(), loc);
	out.push_back(awst::makeExpressionStatement(
		awst::makeAssert(
			awst::makeBytesComparison(std::move(prefix),
				awst::EqualityComparison::Eq, std::move(expected), loc),
			loc, "invalid EVM ABI signed integer padding"), loc));
}
}

std::shared_ptr<awst::Expression> signExtendToWord(
	std::shared_ptr<awst::Expression> bytes,
	awst::SourceLocation const& loc)
{
	auto once = awst::makeEvalOnce(std::move(bytes), loc);
	auto signByte = awst::makeBtoi(awst::makeExtract(once, 0, 1, loc), loc);
	auto negative = awst::makeNumericCompare(
		std::move(signByte), awst::NumericComparison::Gte,
		awst::makeIntegerConstant(uint64_t{128}, loc), loc);
	auto fill = awst::makeConditional(
		std::move(negative),
		awst::makeBytesConstant(std::vector<uint8_t>(32, 0xff), loc),
		awst::makeBzero(32, loc), awst::WType::bytesType(), loc);
	auto start = awst::makeUInt64BinOp(
		awst::makeIntegerConstant(uint64_t{32}, loc),
		awst::UInt64BinaryOperator::Sub, awst::makeLen(once, loc), loc);
	return awst::makeReplace3(std::move(fill), std::move(start), once, loc);
}

std::shared_ptr<awst::Expression> valueFromEvmWord(
	TypeMapper& typeMapper,
	Type const* solType,
	std::shared_ptr<awst::Expression> word,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& out,
	PaddingPolicy padding)
{
	auto const* type = underlyingType(solType);
	auto const* native = typeMapper.map(solType);

	if (auto const* integer = dynamic_cast<IntegerType const*>(type))
	{
		word = awst::makeEvalOnce(std::move(word), loc);
		if (padding == PaddingPolicy::Validate)
			assertIntegerPadding(word, *integer, loc, out);
		else
		{
			// solc's cleanup_t_uintN / signextend: keep the declared width and
			// rebuild the word from it, discarding whatever sat above.
			int const width = static_cast<int>(integer->numBits() / 8);
			if (width < 32)
			{
				auto low = awst::makeExtract(std::move(word), 32 - width, width, loc);
				word = integer->isSigned()
					? signExtendToWord(std::move(low), loc)
					: awst::makeLeftPadToN(std::move(low), 32, loc);
			}
		}
		if (integer->numBits() <= 64)
			return awst::makeWord32ToUInt64(std::move(word), loc);
		return awst::makeAsBiguint(std::move(word), loc);
	}
	if (dynamic_cast<BoolType const*>(type))
	{
		auto fullWord = awst::makeEvalOnce(std::move(word), loc);
		// solc's cleanup_t_bool is iszero(iszero(v)) -- any non-zero word is
		// true. Only the decoder insists on a canonical 0/1.
		if (padding == PaddingPolicy::Validate)
		{
			auto fullValue = awst::makeAsBiguint(fullWord, loc);
			out.push_back(awst::makeExpressionStatement(
				awst::makeAssert(
					awst::makeNumericCompare(std::move(fullValue),
						awst::NumericComparison::Lte,
						awst::makeIntegerConstant("1", loc,
							awst::WType::biguintType()), loc),
					loc, "invalid EVM ABI bool"), loc));
		}
		auto value = awst::makeAsBiguint(fullWord, loc);
		return awst::makeNumericCompare(
			value, awst::NumericComparison::Ne,
			awst::makeZero(loc, awst::WType::biguintType()), loc);
	}
	if (auto const* fixed = dynamic_cast<FixedBytesType const*>(type))
	{
		int const n = static_cast<int>(fixed->numBytes());
		word = awst::makeEvalOnce(std::move(word), loc);
		// The extract below already truncates, which IS solc's cleanup for
		// bytesN. A `bytes memory` element read carries the following array
		// bytes in the same word, so validating here rejects normal programs.
		if (padding == PaddingPolicy::Validate)
			assertZeroBytes(awst::makeExtract(word, n, 32 - n, loc),
				32 - n, loc, out, "invalid EVM ABI fixed-bytes padding");
		auto result = awst::makeExtract(std::move(word), 0, n, loc);
		result->wtype = native;
		return result;
	}
	if (dynamic_cast<AddressType const*>(type)
		|| dynamic_cast<ContractType const*>(type))
	{
		auto value = awst::makeEvalOnce(std::move(word), loc);
		if (padding == PaddingPolicy::Validate)
			out.push_back(awst::makeExpressionStatement(
				awst::makeAssert(
					awst::makeBytesComparison(
						awst::makeExtract(value, 0, 12, loc),
						awst::EqualityComparison::Eq, awst::makeBzero(12, loc), loc),
					loc, "invalid EVM ABI address padding"), loc));
		else
			// solc masks to 160 bits; keep the account 32 bytes wide by
			// re-zeroing the top 12 rather than trusting them.
			value = awst::makeConcat(
				awst::makeBzero(12, loc),
				awst::makeExtract(value, 12, 20, loc), loc);
		return awst::makeAsAccount(std::move(value), loc);
	}
	if (auto const* function = dynamic_cast<FunctionType const*>(type);
		isExternalFunctionPointer(function))
	{
		auto value = awst::makeEvalOnce(std::move(word), loc);
		if (padding == PaddingPolicy::Validate)
			assertZeroBytes(awst::makeExtract(value, 24, 8, loc),
				8, loc, out, "invalid EVM ABI external-function padding");
		auto appId = awst::makeExtract(value, 12, 8, loc);
		auto selector = awst::makeEvalOnce(
			awst::makeExtract(value, 20, 4, loc), loc);
		auto pointer = awst::makeConcat(
			std::move(appId), selector, loc);
		if (typeMapper.profile().evmSelectors)
			pointer = awst::makeConcat(
				std::move(pointer), selector, loc);
		return awst::makeReinterpretCast(std::move(pointer), native, loc);
	}
	if (auto const* enumeration = dynamic_cast<EnumType const*>(type))
	{
		auto fullWord = awst::makeEvalOnce(std::move(word), loc);
		assertZeroBytes(awst::makeExtract(fullWord, 0, 24, loc),
			24, loc, out, "invalid EVM ABI enum padding");
		auto value = awst::makeEvalOnce(
			awst::makeWord32ToUInt64(fullWord, loc), loc);
		out.push_back(awst::makeExpressionStatement(
			awst::makeEnumRangeAssert(value, enumeration->numberOfMembers(), loc), loc));
		return value;
	}
	return awst::makeReinterpretCast(std::move(word), native, loc);
}

std::shared_ptr<awst::Expression> valueFromArc4(
	TypeMapper& typeMapper,
	Type const* solType,
	std::shared_ptr<awst::Expression> value,
	awst::SourceLocation const& loc)
{
	auto const* type = underlyingType(solType);
	auto const* native = typeMapper.map(solType);
	if (awst::structurallyEquivalent(value->wtype, native))
		return value;
	// Public ARC4 wrappers widen signed returns to a uint256 carrier, while the
	// function body and canonical EVM codec use the declared native tier. This
	// is a representation conversion (low-word extraction / promotion), not an
	// ARC4 byte decode. Keep it generic for both numeric tiers so every aggregate
	// element follows the same recursive path.
	if (awst::isNumericWType(value->wtype) && awst::isNumericWType(native))
		return TypeCoercion::coerceForAssignment(
			std::move(value), native, loc);
	// ARC4 public signed returns use arc4.uint256 regardless of the declared
	// signed width. Decode that carrier as a biguint first, then lower it to the
	// declared native numeric tier by preserving the low two's-complement bits.
	// This rule is width-generic and is applied at every recursive leaf.
	if (auto const* integer = dynamic_cast<IntegerType const*>(type);
		integer && integer->isSigned())
		if (auto const* carrier = dynamic_cast<awst::ARC4UIntN const*>(value->wtype);
			carrier && carrier->n() == 256)
		{
			auto decoded = awst::makeARC4Decode(
				std::move(value), awst::WType::biguintType(), loc);
			return TypeCoercion::coerceForAssignment(
				std::move(decoded), native, loc);
		}

	if (auto const* array = dynamic_cast<ArrayType const*>(type);
		array && array->isByteArrayOrString())
	{
		auto bytes = awst::makeEvalOnce(rawBytes(std::move(value), loc), loc);
		auto count = awst::makeExtractUInt16(
			bytes, awst::makeIntegerConstant(uint64_t{0}, loc), loc);
		auto payload = awst::makeExtract3(
			bytes, awst::makeIntegerConstant(uint64_t{2}, loc), std::move(count), loc);
		if (array->isString())
			return awst::makeReinterpretCast(
				std::move(payload), awst::WType::stringType(), loc);
		return payload;
	}

	// Arrays and structs are already represented by their ARC4 aggregate type.
	if (dynamic_cast<ArrayType const*>(type) || dynamic_cast<StructType const*>(type))
		return value;
	return awst::makeARC4Decode(std::move(value), native, loc);
}

std::shared_ptr<awst::Expression> valueToArc4(
	TypeMapper& typeMapper,
	Type const* solType,
	std::shared_ptr<awst::Expression> value,
	awst::WType const* arc4Type,
	awst::SourceLocation const& loc)
{
	if (!arc4Type || value->wtype == arc4Type)
		return value;
	auto const* type = underlyingType(solType);
	if (auto const* array = dynamic_cast<ArrayType const*>(type);
		array && array->isByteArrayOrString())
	{
		auto bytes = awst::makeEvalOnce(rawBytes(std::move(value), loc), loc);
		auto encoded = awst::makeConcat(
			awst::makeUInt16Bytes(awst::makeLen(bytes, loc), loc), bytes, loc);
		return awst::makeReinterpretCast(std::move(encoded), arc4Type, loc);
	}
	return awst::makeARC4Encode(std::move(value), arc4Type, loc);
}

std::shared_ptr<awst::Expression> valueToEvmWord(
	TypeMapper& typeMapper,
	Type const* solType,
	std::shared_ptr<awst::Expression> value,
	awst::SourceLocation const& loc)
{
	auto const* type = underlyingType(solType);
	value = valueFromArc4(typeMapper, solType, std::move(value), loc);

	if (dynamic_cast<BoolType const*>(type))
		return boolWord(std::move(value), loc);
	if (auto const* fixed = dynamic_cast<FixedBytesType const*>(type))
		return awst::makeRightPad(rawBytes(std::move(value), loc),
			32 - static_cast<int>(fixed->numBytes()), loc);
	if (dynamic_cast<AddressType const*>(type)
		|| dynamic_cast<ContractType const*>(type))
		return awst::makeLeftPadToN(
			awst::makeExtractLastN(rawBytes(std::move(value), loc), 20, loc),
			32, loc);
	if (auto const* function = dynamic_cast<FunctionType const*>(type);
		isExternalFunctionPointer(function))
	{
		if (!typeMapper.profile().evmSelectors)
			Logger::instance().error(
				"canonical ABI encoding of an opaque external-function pointer "
				"requires --evm-selectors", loc);
		auto pointer = awst::makeEvalOnce(rawBytes(std::move(value), loc), loc);
		auto address = awst::makeLeftPadToN(
			awst::makeExtract(pointer, 0, 8, loc), 20, loc);
		auto selector = awst::makeExtract(pointer, 8, 4, loc);
		return awst::makeConcat(
			awst::makeConcat(std::move(address), std::move(selector), loc),
			awst::makeBzero(8, loc), loc);
	}

	auto bytes = rawBytes(std::move(value), loc);
	if (auto const* integer = dynamic_cast<IntegerType const*>(type);
		integer && integer->isSigned())
		return signExtendToWord(std::move(bytes), loc);
	return awst::makeLeftPadToN(std::move(bytes), 32, loc);
}

bool canRoundTripEvmAbi(solidity::frontend::Type const* type, std::set<int64_t>& visiting)
{
	using namespace solidity::frontend;
	type = underlyingType(type);
	if (!type)
		return false;
	if (isWordType(type))
		return true;
	if (auto const* array = dynamic_cast<ArrayType const*>(type))
		return array->isByteArrayOrString()
			|| canRoundTripEvmAbi(array->baseType(), visiting);
	if (auto const* structure = dynamic_cast<StructType const*>(type))
	{
		auto id = structure->structDefinition().id();
		if (!visiting.insert(id).second || structure->containsNestedMapping())
			return false;
		for (auto const& member: structure->structDefinition().members())
			if (!member || !canRoundTripEvmAbi(member->type(), visiting))
			{
				visiting.erase(id);
				return false;
			}
		visiting.erase(id);
		return true;
	}
	if (auto const* tuple = dynamic_cast<TupleType const*>(type))
	{
		for (auto const* component: tuple->components())
			if (!canRoundTripEvmAbi(component, visiting))
				return false;
		return true;
	}
	return false;
}

} // namespace puyasol::builder::codec
