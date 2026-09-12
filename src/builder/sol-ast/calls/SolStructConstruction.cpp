#include "builder/sol-ast/calls/SolStructConstruction.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-eb/AssignmentHelper.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolStructConstruction::toAwst()
{
	auto const* structure = dynamic_cast<solidity::frontend::StructType const*>(solType());
	auto const* representation = dynamic_cast<awst::ARC4Struct const*>(wtype());
	assert(structure && representation);
	auto const args = m_call.sortedArguments();
	auto const* constructor = structure->constructorType();
	assert(args.size() == constructor->parameterTypes().size());
	auto result = awst::makeNewStruct(representation, m_loc);
	auto values = CallOperands::build(m_ctx, m_call, m_loc,
		[&](solidity::frontend::Expression const& source, size_t i) {
		// solc owns the argument/member conversion; ARC4 packing is a separate
		// representation step, shared with ordinary field assignments.
		auto const* memberType = constructor->parameterTypes()[i];
		auto value = ConversionPlan{source.annotation().type, memberType,
			m_ctx.typeMapper.map(memberType), ConversionPlan::Context::Initialization}
			.emit(buildExpr(source), m_loc, &m_ctx.preEffects());
		return eb::AssignmentHelper::arc4EncodeForType(
			m_ctx, std::move(value), representation->fields().at(i).second, m_loc);
	});
	for (size_t i = 0; i < values.size(); ++i)
		result->values[constructor->parameterNames()[i]] = std::move(values[i]);
	return result;
}

} // namespace puyasol::builder::sol_ast
