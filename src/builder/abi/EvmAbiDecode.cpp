#include "builder/abi/EvmAbiDecode.h"

#include "Logger.h"
#include "awst/NameGen.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/sol-types/TypeMapper.h"

#include <set>

namespace puyasol::builder::abi
{
using namespace solidity::frontend;

namespace
{
using Statements = std::vector<std::shared_ptr<awst::Statement>>;

std::shared_ptr<awst::Expression> u64(uint64_t value,
	awst::SourceLocation const& loc)
{
	return awst::makeIntegerConstant(value, loc);
}

std::shared_ptr<awst::Expression> add(
	std::shared_ptr<awst::Expression> left,
	std::shared_ptr<awst::Expression> right,
	awst::SourceLocation const& loc)
{
	return awst::makeUInt64BinOp(std::move(left),
		awst::UInt64BinaryOperator::Add, std::move(right), loc);
}

bool canDecode(Type const* type, std::set<int64_t>& visiting)
{
	type = codec::underlyingType(type);
	if (!type)
		return false;
	if (codec::isWordType(type))
		return true;
	if (auto const* array = dynamic_cast<ArrayType const*>(type))
		return array->isByteArrayOrString()
			|| canDecode(array->baseType(), visiting);
	if (auto const* structure = dynamic_cast<StructType const*>(type))
	{
		auto id = structure->structDefinition().id();
		if (!visiting.insert(id).second || structure->containsNestedMapping())
			return false;
		for (auto const& member: structure->structDefinition().members())
			if (!member || !canDecode(member->type(), visiting))
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
			if (!canDecode(component, visiting))
				return false;
		return true;
	}
	return false;
}

awst::WType const* arrayElementType(awst::WType const* array)
{
	if (auto const* dynamic = dynamic_cast<awst::ARC4DynamicArray const*>(array))
		return dynamic->elementType();
	if (auto const* fixed = dynamic_cast<awst::ARC4StaticArray const*>(array))
		return fixed->elementType();
	if (auto const* reference = dynamic_cast<awst::ReferenceArray const*>(array))
		return reference->elementType();
	return nullptr;
}

class Decoder
{
public:
	Decoder(TypeMapper& typeMapper,
		std::shared_ptr<awst::Expression> blob,
		awst::SourceLocation const& loc):
		m_typeMapper(typeMapper), m_blob(std::move(blob)), m_loc(loc)
	{
	}

	std::shared_ptr<awst::Expression> tuple(
		std::vector<Type const*> const& components,
		awst::WType const* target, Statements& out)
	{
		if (components.size() == 1
			&& target->kind() != awst::WTypeKind::WTuple)
			return head(components[0], u64(0, m_loc), u64(0, m_loc), out);

		auto result = awst::makeTupleExpression(target, m_loc);
		uint64_t cursor = 0;
		for (auto const* component: components)
		{
			result->items.push_back(head(component, u64(0, m_loc),
				u64(cursor, m_loc), out));
			cursor += component->calldataHeadSize();
		}
		return result;
	}

private:
	void bounds(std::shared_ptr<awst::Expression> position,
		std::shared_ptr<awst::Expression> size, Statements& out)
	{
		auto end = add(std::move(position), std::move(size), m_loc);
		out.push_back(awst::makeExpressionStatement(
			awst::makeAssert(
				awst::makeNumericCompare(std::move(end),
					awst::NumericComparison::Lte, awst::makeLen(m_blob, m_loc), m_loc),
				m_loc, "EVM ABI decode out of bounds"), m_loc));
	}

	std::shared_ptr<awst::Expression> word(
		std::shared_ptr<awst::Expression> position, Statements& out)
	{
		auto pos = awst::makeEvalOnce(std::move(position), m_loc);
		bounds(pos, u64(32, m_loc), out);
		return awst::makeExtract3(m_blob, pos, u64(32, m_loc), m_loc);
	}

	std::shared_ptr<awst::Expression> smallWord(
		std::shared_ptr<awst::Expression> position, Statements& out,
		char const* what)
	{
		auto value = awst::makeEvalOnce(word(std::move(position), out), m_loc);
		auto prefix = awst::makeAsBiguint(
			awst::makeExtract(value, 0, 24, m_loc), m_loc);
		out.push_back(awst::makeExpressionStatement(
			awst::makeAssert(
				awst::makeNumericCompare(std::move(prefix),
					awst::NumericComparison::Eq,
					awst::makeIntegerConstant("0", m_loc,
						awst::WType::biguintType()), m_loc),
				m_loc, std::string("EVM ABI ") + what + " exceeds uint64"), m_loc));
		return awst::makeWord32ToUInt64(value, m_loc);
	}

	std::shared_ptr<awst::Expression> head(
		Type const* type,
		std::shared_ptr<awst::Expression> base,
		std::shared_ptr<awst::Expression> position,
		Statements& out)
	{
		type = codec::underlyingType(type);
		if (!type->isDynamicallyEncoded())
			return inlineValue(type, std::move(position), out);

		auto offset = awst::makeEvalOnce(
			smallWord(std::move(position), out, "offset"), m_loc);
		out.push_back(awst::makeExpressionStatement(
			awst::makeAssert(
				awst::makeNumericCompare(
					awst::makeUInt64BinOp(offset,
						awst::UInt64BinaryOperator::Mod, u64(32, m_loc), m_loc),
					awst::NumericComparison::Eq, u64(0, m_loc), m_loc),
				m_loc, "unaligned EVM ABI offset"), m_loc));
		return payload(type, add(std::move(base), offset, m_loc), out);
	}

	std::shared_ptr<awst::Expression> inlineValue(
		Type const* type,
		std::shared_ptr<awst::Expression> position,
		Statements& out)
	{
		if (codec::isWordType(type))
			return codec::valueFromEvmWord(
				m_typeMapper, type, word(std::move(position), out), m_loc, out);
		if (auto const* array = dynamic_cast<ArrayType const*>(type))
			return arrayValue(array, std::move(position), nullptr, out);
		if (auto const* structure = dynamic_cast<StructType const*>(type))
			return structValue(structure, std::move(position), out);
		if (auto const* tupleType = dynamic_cast<TupleType const*>(type))
			return tupleValue(tupleType, std::move(position), out);
		return awst::makeBytesConstant({}, m_loc);
	}

	std::shared_ptr<awst::Expression> payload(
		Type const* type,
		std::shared_ptr<awst::Expression> start,
		Statements& out)
	{
		if (auto const* array = dynamic_cast<ArrayType const*>(type))
		{
			if (array->isByteArrayOrString())
			{
				auto base = awst::makeEvalOnce(std::move(start), m_loc);
				auto count = awst::makeEvalOnce(smallWord(base, out, "length"), m_loc);
				out.push_back(awst::makeExpressionStatement(
					awst::makeAssert(
						awst::makeNumericCompare(count, awst::NumericComparison::Lte,
							u64(65535, m_loc), m_loc), m_loc,
						"EVM ABI byte sequence exceeds ARC4 uint16 length"), m_loc));
				auto dataStart = add(base, u64(32, m_loc), m_loc);
				bounds(dataStart, count, out);
				auto bytes = awst::makeExtract3(m_blob, std::move(dataStart), count, m_loc);
				if (array->isString())
					return awst::makeReinterpretCast(
						std::move(bytes), awst::WType::stringType(), m_loc);
				return bytes;
			}
			return arrayValue(array, std::move(start), nullptr, out);
		}
		if (auto const* structure = dynamic_cast<StructType const*>(type))
			return structValue(structure, std::move(start), out);
		if (auto const* tupleType = dynamic_cast<TupleType const*>(type))
			return tupleValue(tupleType, std::move(start), out);
		return inlineValue(type, std::move(start), out);
	}

	std::shared_ptr<awst::Expression> arrayValue(
		ArrayType const* array,
		std::shared_ptr<awst::Expression> start,
		std::shared_ptr<awst::Expression> knownCount,
		Statements& out)
	{
		auto const* arrayW = m_typeMapper.map(array);
		auto const* elemW = arrayElementType(arrayW);
		auto const* elemType = array->baseType();
		auto base = awst::makeEvalOnce(std::move(start), m_loc);

		if (array->isDynamicallySized())
		{
			auto count = knownCount ? std::move(knownCount)
				: smallWord(base, out, "array length");
			count = awst::makeEvalOnce(std::move(count), m_loc);
			out.push_back(awst::makeExpressionStatement(
				awst::makeAssert(
					awst::makeNumericCompare(count, awst::NumericComparison::Lte,
						u64(65535, m_loc), m_loc), m_loc,
					"EVM ABI array exceeds ARC4 uint16 length"), m_loc));
			auto elementsBase = awst::makeEvalOnce(
				add(base, u64(32, m_loc), m_loc), m_loc);

			int id = awst::NameGen::next("EvmAbiDecode.array");
			std::string suffix = std::to_string(id);
			auto arrVar = [&]() { return awst::makeVarExpression(
				"__evmabi_arr_" + suffix, arrayW, m_loc); };
			auto idxVar = [&]() { return awst::makeVarExpression(
				"__evmabi_i_" + suffix, awst::WType::uint64Type(), m_loc); };
			auto countVar = [&]() { return awst::makeVarExpression(
				"__evmabi_n_" + suffix, awst::WType::uint64Type(), m_loc); };
			auto baseVar = [&]() { return awst::makeVarExpression(
				"__evmabi_base_" + suffix, awst::WType::uint64Type(), m_loc); };

			out.push_back(awst::makeAssignmentStatement(
				arrVar(), awst::makeNewArray(arrayW, m_loc), m_loc));
			out.push_back(awst::makeAssignmentStatement(countVar(), count, m_loc));
			out.push_back(awst::makeAssignmentStatement(baseVar(), elementsBase, m_loc));
			out.push_back(awst::makeAssignmentStatement(idxVar(), u64(0, m_loc), m_loc));

			auto body = awst::makeBlock(m_loc);
			auto pos = add(baseVar(), awst::makeUInt64BinOp(
				idxVar(), awst::UInt64BinaryOperator::Mult,
				u64(elemType->calldataHeadSize(), m_loc), m_loc), m_loc);
			auto element = head(elemType, baseVar(), std::move(pos), body->body);
			element = codec::valueToArc4(
				m_typeMapper, elemType, std::move(element), elemW, m_loc);
			body->body.push_back(awst::makeExpressionStatement(
				awst::makeArrayPushOne(arrVar(), std::move(element), arrayW, m_loc), m_loc));
			body->body.push_back(awst::makeAssignmentStatement(
				idxVar(), add(idxVar(), u64(1, m_loc), m_loc), m_loc));
			out.push_back(awst::makeWhileLoop(
				awst::makeNumericCompare(idxVar(), awst::NumericComparison::Lt,
					countVar(), m_loc), std::move(body), m_loc));
			return arrVar();
		}

		auto result = awst::makeNewArray(arrayW, m_loc);
		uint64_t const count = static_cast<uint64_t>(array->length());
		for (uint64_t i = 0; i < count; ++i)
		{
			auto pos = add(base, u64(i * elemType->calldataHeadSize(), m_loc), m_loc);
			auto element = head(elemType, base, std::move(pos), out);
			result->values.push_back(codec::valueToArc4(
				m_typeMapper, elemType, std::move(element), elemW, m_loc));
		}
		return result;
	}

	std::shared_ptr<awst::Expression> structValue(
		StructType const* structure,
		std::shared_ptr<awst::Expression> start,
		Statements& out)
	{
		auto const* structW = dynamic_cast<awst::ARC4Struct const*>(
			m_typeMapper.map(structure));
		if (!structW)
			return awst::makeBytesConstant({}, m_loc);
		auto base = awst::makeEvalOnce(std::move(start), m_loc);
		auto result = awst::makeNewStruct(structW, m_loc);
		uint64_t cursor = 0;
		for (auto const& member: structure->structDefinition().members())
		{
			awst::WType const* fieldW = nullptr;
			for (auto const& [name, type]: structW->fields())
				if (name == member->name()) { fieldW = type; break; }
			auto field = head(member->type(), base,
				add(base, u64(cursor, m_loc), m_loc), out);
			result->values[member->name()] = codec::valueToArc4(
				m_typeMapper, member->type(), std::move(field), fieldW, m_loc);
			cursor += member->type()->calldataHeadSize();
		}
		return result;
	}

	std::shared_ptr<awst::Expression> tupleValue(
		TupleType const* tupleType,
		std::shared_ptr<awst::Expression> start,
		Statements& out)
	{
		auto const* tupleW = dynamic_cast<awst::WTuple const*>(
			m_typeMapper.map(tupleType));
		auto base = awst::makeEvalOnce(std::move(start), m_loc);
		auto result = awst::makeTupleExpression(tupleW, m_loc);
		uint64_t cursor = 0;
		for (auto const* component: tupleType->components())
		{
			result->items.push_back(head(component, base,
				add(base, u64(cursor, m_loc), m_loc), out));
			cursor += component->calldataHeadSize();
		}
		return result;
	}

	TypeMapper& m_typeMapper;
	std::shared_ptr<awst::Expression> m_blob;
	awst::SourceLocation const& m_loc;
};
}

bool canDecodeEvmAbi(std::vector<Type const*> const& components)
{
	std::set<int64_t> visiting;
	for (auto const* component: components)
		if (!canDecode(component, visiting))
			return false;
	return true;
}

std::shared_ptr<awst::Expression> decodeEvmAbi(
	TypeMapper& typeMapper,
	std::shared_ptr<awst::Expression> blob,
	std::vector<Type const*> const& components,
	awst::WType const* targetType,
	awst::SourceLocation const& loc,
	Statements& out)
{
	return Decoder(typeMapper, std::move(blob), loc).tuple(
		components, targetType, out);
}

} // namespace puyasol::builder::abi
