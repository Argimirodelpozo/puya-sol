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
	for (size_t i = 0; i < args.size(); ++i)
	{
		// solc owns the argument/member conversion; ARC4 packing is a separate
		// representation step, shared with ordinary field assignments.
		auto const* memberType = constructor->parameterTypes()[i];
		auto value = ConversionPlan{args[i]->annotation().type, memberType,
			m_ctx.typeMapper.map(memberType), ConversionPlan::Context::Initialization}
			.emit(buildExpr(*args[i]), m_loc, &m_ctx.preEffects());
		result->values[constructor->parameterNames()[i]] = eb::AssignmentHelper::arc4EncodeForType(
			m_ctx, std::move(value), representation->fields().at(i).second, m_loc);
	}
	return result;
}

} // namespace puyasol::builder::sol_ast
