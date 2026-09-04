#include "builder/contract/EvmMemoryCodec.h"
#include "builder/AwstShorthand.h"

#include "Logger.h"
#include "awst/NameGen.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/sol-types/TypeMapper.h"
// StructDefinition members are walked by value here; TypeMapper.h now only
// forward-declares the solc types.
#include <libsolidity/ast/AST.h>
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>

namespace puyasol::builder
{
using namespace puyasol::builder::shorthand;
using namespace solidity::frontend;

namespace
{
using Statements = std::vector<std::shared_ptr<awst::Statement>>;


bool isReference(Type const* type)
{
	type = codec::underlyingType(type);
	return dynamic_cast<ArrayType const*>(type)
		|| dynamic_cast<StructType const*>(type);
}

std::shared_ptr<awst::Expression> checkedUint64Word(
	TypeMapper& mapper, std::shared_ptr<awst::Expression> offset,
	awst::SourceLocation const& loc, Statements& out)
{
	auto once = awst::makeEvalOnce(std::move(offset), loc);
	out.push_back(AssemblyBuilder::memBoundsAssert(
		mapper.profile().scratchLayout, once, loc));
	auto value = awst::makeEvalOnce(AssemblyBuilder::readMemWordDirect(
		mapper.profile().scratchLayout, once, loc), loc);
	out.push_back(awst::makeExpressionStatement(
		awst::makeAssert(
			awst::makeNumericCompare(
				awst::makeAsBiguint(awst::makeExtract(value, 0, 24, loc), loc),
				awst::NumericComparison::Eq,
				awst::makeIntegerConstant("0", loc,
					awst::WType::biguintType()), loc),
			loc, "EVM memory word exceeds uint64"), loc));
	return awst::makeWord32ToUInt64(value, loc);
}

class MemoryReader
{
public:
	MemoryReader(TypeMapper& mapper, awst::SourceLocation const& loc):
		m_mapper(mapper), m_loc(loc), m_scratch(mapper.profile().scratchLayout)
	{
	}

	std::shared_ptr<awst::Expression> read(
		Type const* type, std::shared_ptr<awst::Expression> offset, Statements& out)
	{
		type = codec::underlyingType(type);
		if (codec::isWordType(type))
			return codec::valueFromEvmWord(
				m_mapper, type, word(std::move(offset), out), m_loc, out,
				// A memory read is not a trust boundary: inline assembly may
				// legally have dirtied the high bytes, and the EVM masks them.
				codec::PaddingPolicy::Clean);
		if (auto const* array = dynamic_cast<ArrayType const*>(type))
		{
			if (array->isByteArrayOrString())
				return bytesValue(array, std::move(offset), out);
			return arrayValue(array, std::move(offset), out);
		}
		if (auto const* structure = dynamic_cast<StructType const*>(type))
			return structValue(structure, std::move(offset), out);
		Logger::instance().error(
			"EVM memory lowering: unsupported materialisation type '"
			+ (type ? type->toString(true) : std::string("?")) + "'", m_loc);
		return nullptr;
	}

private:
	std::shared_ptr<awst::Expression> word(
		std::shared_ptr<awst::Expression> offset, Statements& out)
	{
		auto once = awst::makeEvalOnce(std::move(offset), m_loc);
		out.push_back(AssemblyBuilder::memBoundsAssert(m_scratch, once, m_loc));
		// valueFromEvmWord may inspect the same word several times (width,
		// sign, address and enum cleanup). Keep the complete scratch read — not
		// just its offset — single-evaluation. Without this boundary one typed
		// field read duplicates the multi-slot loads/straddle tree at every
		// inspection site, causing both repeated runtime work and exponential
		// program-size growth in struct-heavy event and ABI codecs.
		return awst::makeEvalOnce(
			AssemblyBuilder::readMemWordDirect(m_scratch, once, m_loc), m_loc);
	}

	std::shared_ptr<awst::Expression> pointer(
		std::shared_ptr<awst::Expression> offset, Statements& out)
	{
		return checkedUint64Word(m_mapper, std::move(offset), m_loc, out);
	}

	std::shared_ptr<awst::Expression> child(
		Type const* type, std::shared_ptr<awst::Expression> slot, Statements& out)
	{
		if (isReference(type))
			return read(type, pointer(std::move(slot), out), out);
		return read(type, std::move(slot), out);
	}

	std::shared_ptr<awst::Expression> bytesValue(
		ArrayType const* array, std::shared_ptr<awst::Expression> offset,
		Statements& out)
	{
		auto base = awst::makeEvalOnce(std::move(offset), m_loc);
		auto count = awst::makeEvalOnce(
			checkedUint64Word(m_mapper, base, m_loc, out), m_loc);
		out.push_back(awst::makeExpressionStatement(
			awst::makeAssert(
				awst::makeNumericCompare(count, awst::NumericComparison::Lte,
					u64(AssemblyBuilder::SLOT_SIZE, m_loc), m_loc),
				m_loc, "EVM memory byte value exceeds AVM stack limit"), m_loc));
		auto data = AssemblyBuilder::readMemStackRange(
			m_scratch, add(base, u64(32, m_loc), m_loc), count, m_loc);
		if (array->isString())
			return awst::makeReinterpretCast(
				std::move(data), awst::WType::stringType(), m_loc);
		return data;
	}

	std::shared_ptr<awst::Expression> arrayValue(
		ArrayType const* array, std::shared_ptr<awst::Expression> offset,
		Statements& out)
	{
		auto const* arrayW = m_mapper.map(array);
		auto const* elemW = arrayElementType(arrayW);
		auto const* elemType = array->baseType();
		auto base = awst::makeEvalOnce(std::move(offset), m_loc);
		if (!array->isDynamicallySized())
		{
			auto result = awst::makeNewArray(arrayW, m_loc);
			uint64_t count = static_cast<uint64_t>(array->length());
			for (uint64_t i = 0; i < count; ++i)
			{
				auto value = child(elemType,
					add(base, u64(i * array->memoryStride(), m_loc), m_loc), out);
				if (!value)
					return nullptr;
				result->values.push_back(codec::valueToArc4(
					m_mapper, elemType, std::move(value), elemW, m_loc));
			}
			return result;
		}

		auto count = awst::makeEvalOnce(
			checkedUint64Word(m_mapper, base, m_loc, out), m_loc);
		out.push_back(awst::makeExpressionStatement(
			awst::makeAssert(
				awst::makeNumericCompare(count, awst::NumericComparison::Lte,
					u64(65535, m_loc), m_loc), m_loc,
				"EVM memory array exceeds ARC4 uint16 length"), m_loc));

		int id = awst::NameGen::next("EvmMemoryCodec.readArray");
		std::string suffix = std::to_string(id);
		auto arrVar = [&]() { return awst::makeVarExpression(
			"__evmmem_arr_" + suffix, arrayW, m_loc); };
		auto idxVar = [&]() { return awst::makeVarExpression(
			"__evmmem_i_" + suffix, awst::WType::uint64Type(), m_loc); };
		auto countVar = [&]() { return awst::makeVarExpression(
			"__evmmem_n_" + suffix, awst::WType::uint64Type(), m_loc); };
		auto baseVar = [&]() { return awst::makeVarExpression(
			"__evmmem_base_" + suffix, awst::WType::uint64Type(), m_loc); };
		out.push_back(awst::makeAssignmentStatement(
			arrVar(), awst::makeNewArray(arrayW, m_loc), m_loc));
		out.push_back(awst::makeAssignmentStatement(countVar(), count, m_loc));
		out.push_back(awst::makeAssignmentStatement(
			baseVar(), add(base, u64(32, m_loc), m_loc), m_loc));
		out.push_back(awst::makeAssignmentStatement(idxVar(), u64(0, m_loc), m_loc));

		auto body = awst::makeBlock(m_loc);
		auto slot = add(baseVar(), awst::makeUInt64BinOp(
			idxVar(), awst::UInt64BinaryOperator::Mult,
			u64(array->memoryStride(), m_loc), m_loc), m_loc);
		auto value = child(elemType, std::move(slot), body->body);
		if (!value)
			return nullptr;
		value = codec::valueToArc4(
			m_mapper, elemType, std::move(value), elemW, m_loc);
		body->body.push_back(awst::makeExpressionStatement(
			awst::makeArrayPushOne(arrVar(), std::move(value), arrayW, m_loc), m_loc));
		body->body.push_back(awst::makeAssignmentStatement(
			idxVar(), add(idxVar(), u64(1, m_loc), m_loc), m_loc));
		out.push_back(awst::makeWhileLoop(
			awst::makeNumericCompare(idxVar(), awst::NumericComparison::Lt,
				countVar(), m_loc), std::move(body), m_loc));
		return arrVar();
	}

	std::shared_ptr<awst::Expression> structValue(
		StructType const* structure, std::shared_ptr<awst::Expression> offset,
		Statements& out)
	{
		if (structure->containsNestedMapping())
		{
			Logger::instance().error(
				"EVM memory lowering: a memory struct cannot contain a mapping", m_loc);
			return nullptr;
		}
		auto const* structW = dynamic_cast<awst::ARC4Struct const*>(
			m_mapper.map(structure));
		if (!structW)
			return nullptr;
		auto base = awst::makeEvalOnce(std::move(offset), m_loc);
		auto result = awst::makeNewStruct(structW, m_loc);
		for (auto const& member: structure->structDefinition().members())
		{
			awst::WType const* fieldW = awst::structFieldType(structW, member->name());
			auto value = child(member->type(), add(base,
				u64(structure->memoryOffsetOfMember(member->name()).str(), m_loc),
				m_loc), out);
			if (!value)
				return nullptr;
			result->values[member->name()] = codec::valueToArc4(
				m_mapper, member->type(), std::move(value), fieldW, m_loc);
		}
		return result;
	}

	TypeMapper& m_mapper;
	awst::SourceLocation const& m_loc;
	ScratchLayout const& m_scratch;
};

class MemoryWriter
{
public:
	MemoryWriter(TypeMapper& mapper, awst::SourceLocation const& loc):
		m_mapper(mapper), m_loc(loc), m_scratch(mapper.profile().scratchLayout)
	{
	}

	std::shared_ptr<awst::Expression> write(
		Type const* type, std::shared_ptr<awst::Expression> value, Statements& out)
	{
		type = codec::underlyingType(type);
		if (codec::isWordType(type))
		{
			auto base = allocate(u64(32, m_loc), out);
			writeWord(base, codec::valueToEvmWord(
				m_mapper, type, std::move(value), m_loc), out);
			return base;
		}
		if (auto const* array = dynamic_cast<ArrayType const*>(type))
		{
			if (array->isByteArrayOrString())
				return bytesValue(array, std::move(value), out);
			return arrayValue(array, std::move(value), out);
		}
		if (auto const* structure = dynamic_cast<StructType const*>(type))
			return structValue(structure, std::move(value), out);
		Logger::instance().error(
			"EVM memory lowering: unsupported spill type '"
			+ (type ? type->toString(true) : std::string("?")) + "'", m_loc);
		return nullptr;
	}

	bool writeAt(Type const* type, std::shared_ptr<awst::Expression> value,
		std::shared_ptr<awst::Expression> offset, Statements& out)
	{
		type = codec::underlyingType(type);
		if (codec::isWordType(type))
		{
			writeWord(std::move(offset), codec::valueToEvmWord(
				m_mapper, type, std::move(value), m_loc), out);
			return true;
		}
		if (auto const* array = dynamic_cast<ArrayType const*>(type))
		{
			if (array->isByteArrayOrString() || array->isDynamicallySized())
				return false;
			return fixedArrayAt(array, std::move(value), std::move(offset), out);
		}
		if (auto const* structure = dynamic_cast<StructType const*>(type))
			return structAt(structure, std::move(value), std::move(offset), out);
		return false;
	}

private:
	std::shared_ptr<awst::Expression> allocate(
		std::shared_ptr<awst::Expression> size, Statements& out)
	{
		int id = awst::NameGen::next("EvmMemoryCodec.alloc");
		std::string name = "__evmmem_off_" + std::to_string(id);
		for (auto& statement: AssemblyBuilder::emitMemoryAlloc(
			m_scratch, std::move(size), name, id, m_loc))
			out.push_back(std::move(statement));
		return awst::makeVarExpression(name, awst::WType::uint64Type(), m_loc);
	}

	void writeWord(std::shared_ptr<awst::Expression> offset,
		std::shared_ptr<awst::Expression> value, Statements& out)
	{
		AssemblyBuilder::writeMemWordDirect(
			m_scratch, std::move(offset), std::move(value), m_loc, out);
	}

	std::shared_ptr<awst::Expression> pin(
		std::shared_ptr<awst::Expression> value, Statements& out,
		char const* category)
	{
		int id = awst::NameGen::next("EvmMemoryCodec.pin");
		std::string name = "__evmmem_" + std::string(category) + "_"
			+ std::to_string(id);
		auto const* type = value->wtype;
		out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(name, type, m_loc), std::move(value), m_loc));
		return awst::makeVarExpression(name, type, m_loc);
	}

	std::shared_ptr<awst::Expression> bytesValue(
		ArrayType const* array, std::shared_ptr<awst::Expression> value,
		Statements& out)
	{
		value = codec::valueFromArc4(m_mapper, array, std::move(value), m_loc);
		auto bytes = pin(awst::makeAsBytes(std::move(value), m_loc), out, "bytes");
		int id = awst::NameGen::next("EvmMemoryCodec.bytes");
		std::string name = "__evmmem_off_" + std::to_string(id);
		for (auto& statement: AssemblyBuilder::emitBytesBlobAlloc(
			m_scratch, awst::makeLen(bytes, m_loc), name, id, m_loc))
			out.push_back(std::move(statement));
		AssemblyBuilder::writeMemBytesDirect(m_scratch,
			add(awst::makeVarExpression(name, awst::WType::uint64Type(), m_loc),
				u64(32, m_loc), m_loc),
			bytes, id, m_loc, out);
		return awst::makeVarExpression(name, awst::WType::uint64Type(), m_loc);
	}

	void writeChild(Type const* type,
		std::shared_ptr<awst::Expression> value,
		std::shared_ptr<awst::Expression> slot,
		Statements& out)
	{
		if (isReference(type))
		{
			auto childOffset = write(type, std::move(value), out);
			if (!childOffset)
				return;
			writeWord(std::move(slot),
				awst::makeLeftPadToN(
					awst::makeItob(std::move(childOffset), m_loc), 32, m_loc), out);
			return;
		}
		writeWord(std::move(slot), codec::valueToEvmWord(
			m_mapper, type, std::move(value), m_loc), out);
	}

	std::shared_ptr<awst::Expression> arrayValue(
		ArrayType const* array, std::shared_ptr<awst::Expression> value,
		Statements& out)
	{
		auto const* arrayW = m_mapper.map(array);
		auto const* elemW = arrayElementType(arrayW);
		auto const* elemType = array->baseType();
		auto arrayValue = pin(std::move(value), out, "array");

		if (!array->isDynamicallySized())
		{
			auto base = allocate(u64(array->memoryDataSize().str(), m_loc), out);
			fixedArrayAt(array, arrayValue, base, out);
			return base;
		}

		int id = awst::NameGen::next("EvmMemoryCodec.writeArray");
		std::string suffix = std::to_string(id);
		auto idxVar = [&]() { return awst::makeVarExpression(
			"__evmmem_wi_" + suffix, awst::WType::uint64Type(), m_loc); };
		auto countVar = [&]() { return awst::makeVarExpression(
			"__evmmem_wn_" + suffix, awst::WType::uint64Type(), m_loc); };
		out.push_back(awst::makeAssignmentStatement(
			countVar(), awst::makeArrayLength(
				arrayValue, awst::WType::uint64Type(), m_loc), m_loc));
		auto size = add(u64(32, m_loc), awst::makeUInt64BinOp(
			countVar(), awst::UInt64BinaryOperator::Mult,
			u64(array->memoryStride(), m_loc), m_loc), m_loc);
		auto base = pin(allocate(std::move(size), out), out, "arraybase");
		writeWord(base, awst::makeLeftPadToN(
			awst::makeItob(countVar(), m_loc), 32, m_loc), out);
		out.push_back(awst::makeAssignmentStatement(idxVar(), u64(0, m_loc), m_loc));

		auto body = awst::makeBlock(m_loc);
		auto element = awst::makeIndexExpression(
			arrayValue, idxVar(), elemW, m_loc);
		auto slot = add(add(base, u64(32, m_loc), m_loc),
			awst::makeUInt64BinOp(idxVar(), awst::UInt64BinaryOperator::Mult,
				u64(array->memoryStride(), m_loc), m_loc), m_loc);
		writeChild(elemType, std::move(element), std::move(slot), body->body);
		body->body.push_back(awst::makeAssignmentStatement(
			idxVar(), add(idxVar(), u64(1, m_loc), m_loc), m_loc));
		out.push_back(awst::makeWhileLoop(
			awst::makeNumericCompare(idxVar(), awst::NumericComparison::Lt,
				countVar(), m_loc), std::move(body), m_loc));
		return base;
	}

	std::shared_ptr<awst::Expression> structValue(
		StructType const* structure, std::shared_ptr<awst::Expression> value,
		Statements& out)
	{
		if (structure->containsNestedMapping())
		{
			Logger::instance().error(
				"EVM memory lowering: a memory struct cannot contain a mapping", m_loc);
			return nullptr;
		}
		auto const* structW = dynamic_cast<awst::ARC4Struct const*>(
			m_mapper.map(structure));
		if (!structW)
			return nullptr;
		auto base = pin(allocate(u64(structure->memoryDataSize().str(), m_loc), out),
			out, "structbase");
		structAt(structure, std::move(value), base, out);
		return base;
	}

	bool fixedArrayAt(ArrayType const* array,
		std::shared_ptr<awst::Expression> value,
		std::shared_ptr<awst::Expression> base, Statements& out)
	{
		auto const* arrayW = m_mapper.map(array);
		auto const* elemW = arrayElementType(arrayW);
		auto const* elemType = array->baseType();
		auto arrayValue = pin(std::move(value), out, "fixedarray");
		uint64_t count = static_cast<uint64_t>(array->length());
		for (uint64_t i = 0; i < count; ++i)
		{
			auto element = awst::makeIndexExpression(
				arrayValue, u64(i, m_loc), elemW, m_loc);
			writeChild(elemType, std::move(element),
				add(base, u64(i * array->memoryStride(), m_loc), m_loc), out);
		}
		return true;
	}

	bool structAt(StructType const* structure,
		std::shared_ptr<awst::Expression> value,
		std::shared_ptr<awst::Expression> base, Statements& out)
	{
		if (structure->containsNestedMapping())
			return false;
		auto const* structW = dynamic_cast<awst::ARC4Struct const*>(
			m_mapper.map(structure));
		if (!structW)
			return false;
		auto structValue = pin(std::move(value), out, "struct");
		for (auto const& member: structure->structDefinition().members())
		{
			awst::WType const* fieldW = awst::structFieldType(structW, member->name());
			auto field = awst::makeFieldExpression(
				structValue, member->name(), fieldW, m_loc);
			writeChild(member->type(), std::move(field), add(base,
				u64(structure->memoryOffsetOfMember(member->name()).str(), m_loc),
				m_loc), out);
		}
		return true;
	}

	TypeMapper& m_mapper;
	awst::SourceLocation const& m_loc;
	ScratchLayout const& m_scratch;
};
}

std::shared_ptr<awst::Expression> readEvmMemoryUint64Word(
	TypeMapper& typeMapper,
	std::shared_ptr<awst::Expression> offset,
	awst::SourceLocation const& loc,
	Statements& out)
{
	return checkedUint64Word(typeMapper, std::move(offset), loc, out);
}

std::shared_ptr<awst::Expression> materializeEvmMemoryValue(
	TypeMapper& typeMapper,
	Type const* solType,
	awst::WType const* wtype,
	std::shared_ptr<awst::Expression> offset,
	awst::SourceLocation const& loc,
	Statements& out)
{
	auto result = MemoryReader(typeMapper, loc).read(solType, std::move(offset), out);
	if (result && wtype && result->wtype != wtype)
		result = codec::valueToArc4(typeMapper, solType, std::move(result), wtype, loc);
	return result;
}

bool spillEvmMemoryValue(
	TypeMapper& typeMapper,
	Type const* solType,
	awst::WType const* wtype,
	std::shared_ptr<awst::Expression> value,
	std::string const& offVar,
	int uniqueId,
	awst::SourceLocation const& loc,
	Statements& out)
{
	(void)wtype;
	(void)uniqueId;
	auto offset = MemoryWriter(typeMapper, loc).write(solType, std::move(value), out);
	if (!offset)
		return false;
	out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(offVar, awst::WType::uint64Type(), loc),
		std::move(offset), loc));
	return true;
}

bool writeEvmMemoryValueAt(
	TypeMapper& typeMapper,
	Type const* solType,
	std::shared_ptr<awst::Expression> value,
	std::shared_ptr<awst::Expression> offset,
	awst::SourceLocation const& loc,
	Statements& out)
{
	return MemoryWriter(typeMapper, loc).writeAt(
		solType, std::move(value), std::move(offset), out);
}

} // namespace puyasol::builder
