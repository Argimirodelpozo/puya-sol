#include "builder/ast/calls/SolStructConstruction.h"
#include "builder/ast/exprs/SolIndexAccess.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/ConversionPlan.h"
#include "builder/eb/AssignmentHelper.h"
#include "builder/codec/EvmMemoryCodec.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/yul/AssemblyBuilder.h"
#include "awst/NameGen.h"

#include <libsolidity/ast/Types.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolStructConstruction::toAwst()
{
	using namespace solidity::frontend;
	auto const* structure = dynamic_cast<StructType const*>(solType());
	auto const* representation = dynamic_cast<awst::ARC4Struct const*>(wtype());
	assert(structure && representation);
	auto const args = m_call.sortedArguments();
	auto const* constructor = structure->constructorType();
	assert(args.size() == constructor->parameterTypes().size());
	bool const pointer = m_ctx.typeMapper.profile().scratchMemoryModel
		&& std::exchange(m_ctx.memoryReferenceWanted, false);
	int const id = pointer ? awst::NameGen::next("SolStructConstruction.memory") : 0;
	std::string const name = "__struct_memory_" + std::to_string(id);
	auto base = [&] { return awst::makeVarExpression(name, awst::WType::uint64Type(), m_loc); };
	auto allocate = [&] {
		for (auto& statement: AssemblyBuilder::emitMemoryAlloc(m_ctx.typeMapper.profile().scratchLayout,
			awst::makeIntegerConstant(std::max(uint64_t{32}, static_cast<uint64_t>(structure->memoryDataSize())), m_loc),
			name, id, m_loc)) m_ctx.preEffects().push_back(std::move(statement));
	};
	auto store = [&](size_t i, std::shared_ptr<awst::Expression> value) {
		auto offset = awst::makeUInt64BinOp(base(), awst::UInt64BinaryOperator::Add,
			awst::makeIntegerConstant(static_cast<uint64_t>(
				structure->memoryOffsetOfMember(constructor->parameterNames()[i])), m_loc), m_loc);
		AssemblyBuilder::writeMemWordDirect(m_ctx.typeMapper, std::move(offset),
			std::move(value), m_loc, m_ctx.preEffects(), 0);
	};
	// Legacy solc allocates before evaluating arguments, writing each field as
	// it goes. IR visits all arguments before allocating and writing the head.
	if (pointer && !m_ctx.viaIRSequencing) allocate();
	auto result = awst::makeNewStruct(representation, m_loc);
	auto values = CallOperands::build(m_ctx, m_call, m_loc,
		[&](solidity::frontend::Expression const& source, size_t i) {
		auto const* memberType = constructor->parameterTypes()[i];
		auto const* referenceType = dynamic_cast<ReferenceType const*>(memberType);
		std::shared_ptr<awst::Expression> value;
		if (pointer && referenceType && source.annotation().type->dataStoredIn(DataLocation::Memory))
			if (auto reference = SolIndexAccess::resolveBlobReference(m_ctx, m_scope, source, m_loc))
				value = m_ctx.emitSequencedOperand(std::move(reference->effects),
					std::move(reference->value), true, m_loc);
		if (!value)
		{
			value = ConversionPlan{source.annotation().type, memberType,
				m_ctx.typeMapper.map(memberType), ConversionPlan::Context::Initialization}
				.emit(buildExpr(source), m_loc, &m_ctx.preEffects());
			if (pointer && referenceType)
			{
				int const childId = awst::NameGen::next("SolStructConstruction.child");
				std::string const child = "__struct_child_" + std::to_string(childId);
				if (!spillEvmMemoryValue(m_ctx.typeMapper, memberType, m_ctx.typeMapper.map(memberType),
					std::move(value), child, childId, m_loc, m_ctx.preEffects()))
					throw std::runtime_error("Cannot allocate scratch struct reference member");
				value = awst::makeVarExpression(child, awst::WType::uint64Type(), m_loc);
			}
		}
		if (!pointer)
			return eb::AssignmentHelper::arc4EncodeForType(
				m_ctx, std::move(value), representation->fields().at(i).second, m_loc);
		value = referenceType ? awst::makeLeftPadToN(awst::makeItob(std::move(value), m_loc), 32, m_loc)
			: codec::valueToEvmWord(m_ctx.typeMapper, memberType, std::move(value), m_loc);
		if (!m_ctx.viaIRSequencing)
		{
			value = m_ctx.emitSequencedOperand({}, std::move(value), true, m_loc);
			store(i, value);
		}
		return value;
	});
	if (pointer)
	{
		if (m_ctx.viaIRSequencing)
		{
			allocate();
			for (size_t i = 0; i < values.size(); ++i) store(i, std::move(values[i]));
		}
		return base();
	}
	for (size_t i = 0; i < values.size(); ++i)
		result->values[constructor->parameterNames()[i]] = std::move(values[i]);
	return result;
}

} // namespace puyasol::builder::sol_ast
