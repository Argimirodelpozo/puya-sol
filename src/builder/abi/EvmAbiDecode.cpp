#include "builder/abi/EvmAbiDecode.h"
#include "builder/AwstShorthand.h"

#include "Logger.h"
#include "awst/NameGen.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/BuildArtifacts.h"

#include <stdexcept>
// solc AST nodes used completely (dynamic_cast / member access); the hub
// headers only forward-declare them now.
#include <libsolidity/ast/AST.h>

namespace puyasol::builder::abi
{
using namespace puyasol::builder::shorthand;
using namespace solidity::frontend;

namespace
{
using Statements = std::vector<std::shared_ptr<awst::Statement>>;

class Decoder
{
public:
	Decoder(TypeMapper& typeMapper, std::shared_ptr<awst::Expression> blob,
		awst::SourceLocation const& loc, bool routerBody = false):
		m_typeMapper(typeMapper), m_routerBody(routerBody),
		m_blob(routerBody ? std::move(blob) : awst::makeEvalOnce(std::move(blob), loc)), m_loc(loc)
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
		// Shared fetch subroutine (bounds assert lives in its body).
		if (m_routerBody)
		{
			auto call = awst::makeSubroutineCall(
				awst::InstanceMethodTarget{"__evm_decw"},
				awst::WType::bytesType(), m_loc);
			awst::pushCallArg(call->args, std::move(position));
			return call;
		}
		auto pos = awst::makeEvalOnce(std::move(position), m_loc);
		bounds(pos, u64(32, m_loc), out);
		return awst::makeExtract3(m_blob, pos, u64(32, m_loc), m_loc);
	}

	std::shared_ptr<awst::Expression> smallWord(
		std::shared_ptr<awst::Expression> position, Statements& out,
		char const* what)
	{
		// Shared fetch+assert+narrow subroutine. The helper's assert message is
		// generic; `what` only differentiates the inline diagnostics.
		if (m_routerBody)
		{
			auto call = awst::makeSubroutineCall(
				awst::InstanceMethodTarget{"__evm_deco"},
				awst::WType::uint64Type(), m_loc);
			awst::pushCallArg(call->args, std::move(position));
			return call;
		}
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
		return payload(type, add(std::move(base), offset, m_loc), out);
	}

	std::shared_ptr<awst::Expression> inlineValue(
		Type const* type,
		std::shared_ptr<awst::Expression> position,
		Statements& out)
	{
		if (m_routerBody
			&& (dynamic_cast<AddressType const*>(type)
				|| dynamic_cast<ContractType const*>(type)))
		{
			// Shared fetch+padding-assert subroutine for the address leaf.
			auto call = awst::makeSubroutineCall(
				awst::InstanceMethodTarget{"__evm_arga"},
				awst::WType::accountType(), m_loc);
			awst::pushCallArg(call->args, std::move(position));
			return call;
		}
		if (codec::isWordType(type))
			return codec::valueFromEvmWord(
				m_typeMapper, type, word(std::move(position), out), m_loc, out,
				// ABI input IS a trust boundary (solc: validator_revert_t_uintN).
				codec::PaddingPolicy::Validate);
		if (auto const* array = dynamic_cast<ArrayType const*>(type))
			return arrayValue(array, std::move(position), out);
		if (auto const* structure = dynamic_cast<StructType const*>(type))
			return structValue(structure, std::move(position), out);
		if (auto const* tupleType = dynamic_cast<TupleType const*>(type))
			return tupleValue(tupleType, std::move(position), out);
		throw std::logic_error("Unsupported ABI decoder leaf");
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
			return arrayValue(array, std::move(start), out);
		}
		if (auto const* structure = dynamic_cast<StructType const*>(type))
			return structValue(structure, std::move(start), out);
		if (auto const* tupleType = dynamic_cast<TupleType const*>(type))
			return tupleValue(tupleType, std::move(start), out);
		return inlineValue(type, std::move(start), out);
	}

	template<class Build>
	std::shared_ptr<awst::Expression> aggregate(
		Type const* type, std::shared_ptr<awst::Expression> start,
		Statements& out, Build const& build)
	{
		if (!m_routerBody) return build(std::move(start), out);
		auto const* resultType = m_typeMapper.map(type);
		auto& arts = m_typeMapper.artifacts();
		std::string key = "evm-abi:" + type->identifier();
		auto [entry, fresh] = arts.contract().helpers.try_emplace(key);
		if (fresh)
		{
			entry->second = "__evm_dec_" + std::to_string(awst::NameGen::next("EvmAbiDecode.method"));
			auto method = awst::ContractMethod("", entry->second, resultType,
				{{"__start", awst::WType::uint64Type(), m_loc}}, m_loc);
			auto value = build(u64Var("__start", m_loc), method.body->body);
			method.body->body.push_back(awst::makeReturnStatement(std::move(value), m_loc));
			arts.contract().pendingHelpers.push_back(std::move(method));
		}
		auto call = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{entry->second}, resultType, m_loc);
		awst::pushCallArg(call->args, "__start", std::move(start));
		return call;
	}

	std::shared_ptr<awst::Expression> arrayValue(
		ArrayType const* array, std::shared_ptr<awst::Expression> start, Statements& out)
	{
		return aggregate(array, std::move(start), out, [&](auto position, auto& statements) {
			return arrayElements(array, std::move(position), statements);
		});
	}

	std::shared_ptr<awst::Expression> arrayElements(
		ArrayType const* array, std::shared_ptr<awst::Expression> start, Statements& out)
	{
		auto const* arrayW = m_typeMapper.map(array);
		auto const* elemW = arrayElementType(arrayW);
		auto const* elemType = array->baseType();
		bool const dynamic = array->isDynamicallySized();
		auto base = awst::makeEvalOnce(std::move(start), m_loc);
		auto count = awst::makeEvalOnce(dynamic ? smallWord(base, out, "array length")
			: u64(static_cast<uint64_t>(array->length()), m_loc), m_loc);
		out.push_back(awst::makeExpressionStatement(awst::makeAssert(
			awst::makeNumericCompare(count, awst::NumericComparison::Lte, u64(65535, m_loc), m_loc),
			m_loc, "EVM ABI array exceeds ARC4 uint16 length"), m_loc));
		auto elementsBase = awst::makeEvalOnce(dynamic ? add(base, u64(32, m_loc), m_loc) : base, m_loc);
		auto bodySize = awst::makeUInt64BinOp(count, awst::UInt64BinaryOperator::Mult,
			u64(elemType->calldataHeadSize(), m_loc), m_loc);
		// solc validates the complete head before decoding any elements.
		bounds(elementsBase, bodySize, out);
		if (codec::isByteIdenticalEvmWord(elemType))
		{
			auto bytes = awst::makeExtract3(m_blob, elementsBase, std::move(bodySize), m_loc);
			if (dynamic) bytes = awst::makeConcat(awst::makeUInt16Bytes(count, m_loc), std::move(bytes), m_loc);
			return awst::makeReinterpretCast(std::move(bytes), arrayW, m_loc);
		}

		// Measured loop setup outweighs the saving for one/two fixed elements.
		if (!dynamic && array->length() <= 2)
		{
			auto result = awst::makeNewArray(arrayW, m_loc);
			for (uint64_t i = 0; i < static_cast<uint64_t>(array->length()); ++i)
				result->values.push_back(codec::valueToArc4(m_typeMapper, elemType,
					head(elemType, elementsBase, add(elementsBase,
						u64(i * elemType->calldataHeadSize(), m_loc), m_loc), out), elemW, m_loc));
			return result;
		}

		// Fixed ARC4 element layouts can be allocated once and filled by index.
		// ARC4's indexed setter owns bool packing. Only dynamic tails need the
		// growing accumulator's rebasing; its fixed-array body omits the count.
		auto const size = computeEncodedElementSize(elemW);
		auto const stride = size.fixedBytes();
		bool const preallocate = stride.has_value() || size.kind == EncodedSize::Kind::Packed;
		auto const* accumulator = preallocate || dynamic ? arrayW : m_typeMapper.createType<awst::ARC4DynamicArray>(elemW);
		std::shared_ptr<awst::Expression> initial = awst::makeNewArray(accumulator, m_loc);
		if (preallocate)
		{
			auto bytes = awst::makeBzero(stride
				? awst::makeUInt64BinOp(count, awst::UInt64BinaryOperator::Mult, u64(*stride, m_loc), m_loc)
				: awst::makeUInt64BinOp(add(count, u64(7, m_loc), m_loc),
					awst::UInt64BinaryOperator::FloorDiv, u64(8, m_loc), m_loc), m_loc);
			if (dynamic) bytes = awst::makeConcat(awst::makeUInt16Bytes(count, m_loc), std::move(bytes), m_loc);
			initial = awst::makeReinterpretCast(std::move(bytes), accumulator, m_loc);
		}
		auto suffix = std::to_string(awst::NameGen::next("EvmAbiDecode.array"));
		auto arr = awst::makeVarExpression("__evmabi_arr_" + suffix, accumulator, m_loc);
		auto index = u64Var("__evmabi_i_" + suffix, m_loc);
		auto n = u64Var("__evmabi_n_" + suffix, m_loc);
		auto data = u64Var("__evmabi_base_" + suffix, m_loc);
		out.push_back(awst::makeAssignmentStatement(arr, std::move(initial), m_loc));
		out.push_back(awst::makeAssignmentStatement(n, count, m_loc));
		out.push_back(awst::makeAssignmentStatement(data, elementsBase, m_loc));
		out.push_back(awst::makeAssignmentStatement(index, u64(0, m_loc), m_loc));
		auto body = awst::makeBlock(m_loc);
		auto pos = add(data, awst::makeUInt64BinOp(index, awst::UInt64BinaryOperator::Mult,
			u64(elemType->calldataHeadSize(), m_loc), m_loc), m_loc);
		auto value = codec::valueToArc4(m_typeMapper, elemType,
			head(elemType, data, std::move(pos), body->body), elemW, m_loc);
		if (preallocate)
			body->body.push_back(awst::makeAssignmentStatement(
				awst::makeIndexExpression(arr, index, elemW, m_loc), std::move(value), m_loc));
		else
			body->body.push_back(awst::makeExpressionStatement(
				awst::makeArrayPushOne(arr, std::move(value), accumulator, m_loc), m_loc));
		body->body.push_back(awst::makeAssignmentStatement(index, add(index, u64(1, m_loc), m_loc), m_loc));
		out.push_back(awst::makeWhileLoop(
			awst::makeNumericCompare(index, awst::NumericComparison::Lt, n, m_loc), std::move(body), m_loc));
		if (preallocate || dynamic) return arr;
		auto bytes = awst::makeAsBytes(arr, m_loc);
		return awst::makeReinterpretCast(awst::makeExtract3(bytes, u64(2, m_loc),
			awst::makeUInt64BinOp(awst::makeLen(bytes, m_loc), awst::UInt64BinaryOperator::Sub,
				u64(2, m_loc), m_loc), m_loc), arrayW, m_loc);
	}

	std::shared_ptr<awst::Expression> structValue(
		StructType const* structure, std::shared_ptr<awst::Expression> start, Statements& out)
	{
		auto const* type = dynamic_cast<awst::ARC4Struct const*>(m_typeMapper.map(structure));
		if (!type) throw std::logic_error("ABI struct has no ARC4 representation");
		return aggregate(structure, std::move(start), out, [&](auto position, auto& statements) {
			return structFields(structure, type, std::move(position), statements);
		});
	}

	std::shared_ptr<awst::Expression> structFields(
		StructType const* structure,
		awst::ARC4Struct const* structW,
		std::shared_ptr<awst::Expression> start,
		Statements& out)
	{
		auto base = awst::makeEvalOnce(std::move(start), m_loc);
		auto result = awst::makeNewStruct(structW, m_loc);
		uint64_t cursor = 0;
		for (auto const& member: structure->structDefinition().members())
		{
			awst::WType const* fieldW = awst::structFieldType(structW, member->name());
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
	bool m_routerBody = false;
	std::shared_ptr<awst::Expression> m_blob;
	awst::SourceLocation const& m_loc;
};
}

std::shared_ptr<awst::Expression> decodeEvmAbi(
	TypeMapper& typeMapper, std::shared_ptr<awst::Expression> blob,
	std::vector<Type const*> const& components, awst::WType const* target,
	awst::SourceLocation const& loc, Statements& out)
{
	if (!target) throw std::logic_error("ABI decode has no target type");
	if (!codec::canRoundTripEvmAbi(components))
	{
		Logger::instance().error("type is not representable in canonical Solidity ABI decoding", loc);
		return awst::makeBytesConstant({}, loc);
	}
	return Decoder(typeMapper, std::move(blob), loc).tuple(components, target, out);
}

std::shared_ptr<awst::Expression> decodeEvmCalldata(
	TypeMapper& typeMapper, std::vector<Type const*> const& components,
	awst::WType const* target, awst::SourceLocation const& loc, Statements& out)
{
	if (!target || !codec::canRoundTripEvmAbi(components))
		throw std::logic_error("Unsupported EVM router decode");
	return Decoder(typeMapper, awst::makeAppArg(1, loc), loc, true).tuple(components, target, out);
}

} // namespace puyasol::builder::abi
