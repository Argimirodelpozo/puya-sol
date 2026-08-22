#include "builder/abi/EvmAbiEncode.h"

#include "awst/NameGen.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/sol-types/TypeMapper.h"

#include <set>
#include <string>
// solc AST nodes used completely (dynamic_cast / member access); the hub
// headers only forward-declare them now.
#include <libsolidity/ast/AST.h>

namespace puyasol::builder::abi
{
using namespace solidity::frontend;

namespace
{
using Statements = std::vector<std::shared_ptr<awst::Statement>>;

std::shared_ptr<awst::Expression> u64(
	uint64_t value, awst::SourceLocation const& loc)
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

std::shared_ptr<awst::Expression> multiply(
	std::shared_ptr<awst::Expression> left,
	std::shared_ptr<awst::Expression> right,
	awst::SourceLocation const& loc)
{
	return awst::makeUInt64BinOp(std::move(left),
		awst::UInt64BinaryOperator::Mult, std::move(right), loc);
}

std::shared_ptr<awst::Expression> word(
	std::shared_ptr<awst::Expression> value,
	awst::SourceLocation const& loc)
{
	return awst::makeLeftPadToN(awst::makeItob(std::move(value), loc), 32, loc);
}

std::shared_ptr<awst::Expression> concat(
	std::vector<std::shared_ptr<awst::Expression>> parts,
	awst::SourceLocation const& loc)
{
	if (parts.empty())
		return awst::makeBytesConstant({}, loc);
	auto result = std::move(parts.front());
	for (size_t i = 1; i < parts.size(); ++i)
		result = awst::makeConcat(std::move(result), std::move(parts[i]), loc);
	return result;
}

bool canEncode(Type const* type, std::set<int64_t>& visiting)
{
	type = codec::underlyingType(type);
	if (!type)
		return false;
	if (codec::isWordType(type))
		return true;
	if (auto const* array = dynamic_cast<ArrayType const*>(type))
		return array->isByteArrayOrString()
			|| canEncode(array->baseType(), visiting);
	if (auto const* structure = dynamic_cast<StructType const*>(type))
	{
		auto id = structure->structDefinition().id();
		if (!visiting.insert(id).second || structure->containsNestedMapping())
			return false;
		for (auto const& member: structure->structDefinition().members())
			if (!member || !canEncode(member->type(), visiting))
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
			if (!canEncode(component, visiting))
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

awst::WType const* structFieldType(
	awst::WType const* structure, std::string const& name)
{
	if (auto const* arc4 = dynamic_cast<awst::ARC4Struct const*>(structure))
		for (auto const& [fieldName, fieldType]: arc4->fields())
			if (fieldName == name)
				return fieldType;
	return nullptr;
}

class Encoder
{
public:
	Encoder(TypeMapper& typeMapper, awst::SourceLocation const& loc):
		m_typeMapper(typeMapper), m_loc(loc)
	{
	}

	std::shared_ptr<awst::Expression> sequence(
		std::vector<Type const*> const& types,
		std::vector<std::shared_ptr<awst::Expression>> values,
		Statements& out)
	{
		uint64_t headSize = 0;
		for (auto const* type: types)
			headSize += codec::underlyingType(type)->calldataHeadSize();

		std::vector<std::shared_ptr<awst::Expression>> heads;
		std::vector<std::shared_ptr<awst::Expression>> tails;
		auto tailOffset = u64(headSize, m_loc);
		for (size_t i = 0; i < types.size(); ++i)
		{
			auto const* type = codec::underlyingType(types[i]);
			auto value = i < values.size() ? std::move(values[i])
				: awst::makeBytesConstant({}, m_loc);
			if (!type->isDynamicallyEncoded())
			{
				heads.push_back(inlineValue(type, std::move(value), out));
				continue;
			}
			auto tail = awst::makeEvalOnce(
				payload(type, std::move(value), out), m_loc);
			heads.push_back(word(tailOffset, m_loc));
			tailOffset = add(std::move(tailOffset), awst::makeLen(tail, m_loc), m_loc);
			tails.push_back(std::move(tail));
		}
		auto head = concat(std::move(heads), m_loc);
		if (tails.empty())
			return head;
		return awst::makeConcat(
			std::move(head), concat(std::move(tails), m_loc), m_loc);
	}

private:
	std::shared_ptr<awst::Expression> inlineValue(
		Type const* type, std::shared_ptr<awst::Expression> value,
		Statements& out)
	{
		if (codec::isWordType(type))
			return codec::valueToEvmWord(
				m_typeMapper, type, std::move(value), m_loc);
		if (auto const* array = dynamic_cast<ArrayType const*>(type))
			return arrayValue(array, std::move(value), out, false);
		if (auto const* structure = dynamic_cast<StructType const*>(type))
			return structValue(structure, std::move(value), out);
		if (auto const* tuple = dynamic_cast<TupleType const*>(type))
			return tupleValue(tuple, std::move(value), out);
		return awst::makeBytesConstant({}, m_loc);
	}

	std::shared_ptr<awst::Expression> payload(
		Type const* type, std::shared_ptr<awst::Expression> value,
		Statements& out)
	{
		if (auto const* array = dynamic_cast<ArrayType const*>(type))
		{
			if (array->isByteArrayOrString())
			{
				auto bytes = awst::makeEvalOnce(codec::valueFromArc4(
					m_typeMapper, type, std::move(value), m_loc), m_loc);
				if (bytes->wtype != awst::WType::bytesType())
					bytes = awst::makeAsBytes(std::move(bytes), m_loc);
				return awst::makeConcat(
					word(awst::makeLen(bytes, m_loc), m_loc),
					awst::makeRightPadTo32Multiple(bytes, m_loc), m_loc);
			}
			return arrayValue(
				array, std::move(value), out, array->isDynamicallySized());
		}
		if (auto const* structure = dynamic_cast<StructType const*>(type))
			return structValue(structure, std::move(value), out);
		if (auto const* tuple = dynamic_cast<TupleType const*>(type))
			return tupleValue(tuple, std::move(value), out);
		return inlineValue(type, std::move(value), out);
	}

	std::shared_ptr<awst::Expression> arrayValue(
		ArrayType const* array, std::shared_ptr<awst::Expression> value,
		Statements& out, bool includeLength)
	{
		auto arrayValue = awst::makeEvalOnce(std::move(value), m_loc);
		auto const* elemType = codec::underlyingType(array->baseType());
		auto const* elemW = arrayElementType(arrayValue->wtype);
		auto count = array->isDynamicallySized()
			? std::static_pointer_cast<awst::Expression>(
				awst::makeArrayLength(arrayValue, awst::WType::uint64Type(), m_loc))
			: u64(static_cast<uint64_t>(array->length()), m_loc);
		count = awst::makeEvalOnce(std::move(count), m_loc);

		int id = awst::NameGen::next("EvmAbiEncode.array");
		std::string suffix = std::to_string(id);
		auto bytesVar = [&](std::string const& stem) {
			return awst::makeVarExpression(
				"__evmabi_" + stem + "_" + suffix,
				awst::WType::bytesType(), m_loc);
		};
		auto uintVar = [&](std::string const& stem) {
			return awst::makeVarExpression(
				"__evmabi_" + stem + "_" + suffix,
				awst::WType::uint64Type(), m_loc);
		};
		out.push_back(awst::makeAssignmentStatement(
			bytesVar("heads"), awst::makeBytesConstant({}, m_loc), m_loc));
		out.push_back(awst::makeAssignmentStatement(
			bytesVar("tails"), awst::makeBytesConstant({}, m_loc), m_loc));
		out.push_back(awst::makeAssignmentStatement(uintVar("n"), count, m_loc));
		out.push_back(awst::makeAssignmentStatement(uintVar("i"), u64(0, m_loc), m_loc));
		out.push_back(awst::makeAssignmentStatement(
			uintVar("tailoff"), multiply(uintVar("n"),
				u64(elemType->calldataHeadSize(), m_loc), m_loc), m_loc));

		auto body = awst::makeBlock(m_loc);
		auto element = awst::makeIndexExpression(
			arrayValue, uintVar("i"), elemW, m_loc);
		if (elemType->isDynamicallyEncoded())
		{
			auto encoded = awst::makeEvalOnce(
				payload(elemType, std::move(element), body->body), m_loc);
			body->body.push_back(awst::makeAssignmentStatement(
				bytesVar("heads"), awst::makeConcat(bytesVar("heads"),
					word(uintVar("tailoff"), m_loc), m_loc), m_loc));
			body->body.push_back(awst::makeAssignmentStatement(
				bytesVar("tails"), awst::makeConcat(bytesVar("tails"), encoded, m_loc), m_loc));
			body->body.push_back(awst::makeAssignmentStatement(
				uintVar("tailoff"), add(uintVar("tailoff"),
					awst::makeLen(encoded, m_loc), m_loc), m_loc));
		}
		else
		{
			body->body.push_back(awst::makeAssignmentStatement(
				bytesVar("heads"), awst::makeConcat(bytesVar("heads"),
					inlineValue(elemType, std::move(element), body->body), m_loc), m_loc));
		}
		body->body.push_back(awst::makeAssignmentStatement(
			uintVar("i"), add(uintVar("i"), u64(1, m_loc), m_loc), m_loc));
		out.push_back(awst::makeWhileLoop(
			awst::makeNumericCompare(uintVar("i"), awst::NumericComparison::Lt,
				uintVar("n"), m_loc), std::move(body), m_loc));

		auto encoded = awst::makeConcat(bytesVar("heads"), bytesVar("tails"), m_loc);
		if (!includeLength)
			return encoded;
		return awst::makeConcat(word(uintVar("n"), m_loc), std::move(encoded), m_loc);
	}

	std::shared_ptr<awst::Expression> structValue(
		StructType const* structure, std::shared_ptr<awst::Expression> value,
		Statements& out)
	{
		auto base = awst::makeEvalOnce(std::move(value), m_loc);
		std::vector<Type const*> types;
		std::vector<std::shared_ptr<awst::Expression>> values;
		for (auto const& member: structure->structDefinition().members())
		{
			types.push_back(member->type());
			auto const* fieldW = structFieldType(base->wtype, member->name());
			if (!fieldW)
				fieldW = m_typeMapper.map(member->type());
			values.push_back(awst::makeFieldExpression(
				base, member->name(), fieldW, m_loc));
		}
		return sequence(types, std::move(values), out);
	}

	std::shared_ptr<awst::Expression> tupleValue(
		TupleType const* tuple, std::shared_ptr<awst::Expression> value,
		Statements& out)
	{
		auto base = awst::makeEvalOnce(std::move(value), m_loc);
		auto const* tupleW = dynamic_cast<awst::WTuple const*>(base->wtype);
		std::vector<Type const*> types;
		std::vector<std::shared_ptr<awst::Expression>> values;
		for (size_t i = 0; i < tuple->components().size(); ++i)
		{
			auto const* component = tuple->components()[i];
			types.push_back(component);
			auto const* itemW = tupleW && i < tupleW->types().size()
				? tupleW->types()[i] : m_typeMapper.map(component);
			values.push_back(awst::makeTupleItem(
				base, static_cast<int>(i), itemW, m_loc));
		}
		return sequence(types, std::move(values), out);
	}

	TypeMapper& m_typeMapper;
	awst::SourceLocation const& m_loc;
};
}

bool canEncodeEvmAbi(std::vector<Type const*> const& components)
{
	std::set<int64_t> visiting;
	for (auto const* component: components)
		if (!canEncode(component, visiting))
			return false;
	return true;
}

std::shared_ptr<awst::Expression> encodeEvmAbi(
	TypeMapper& typeMapper,
	std::vector<Type const*> const& components,
	std::vector<std::shared_ptr<awst::Expression>> values,
	awst::SourceLocation const& loc,
	Statements& out)
{
	return Encoder(typeMapper, loc).sequence(components, std::move(values), out);
}

} // namespace puyasol::builder::abi
