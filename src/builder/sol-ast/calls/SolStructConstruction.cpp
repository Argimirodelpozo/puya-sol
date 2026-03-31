#include "builder/sol-ast/calls/SolStructConstruction.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolStructConstruction::toAwst()
{
	auto* solType = m_call.annotation().type;
	auto* wtype = m_ctx.typeMapper.map(solType);

	auto const& names = m_call.names();
	auto const& args = m_call.arguments();

	std::map<std::string, std::shared_ptr<awst::Expression>> fieldValues;

	auto const* tupleType = dynamic_cast<awst::WTuple const*>(wtype);
	auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(wtype);

	if (!names.empty())
	{
		// Named arguments: MyStruct({field1: val1, field2: val2})
		for (size_t i = 0; i < names.size(); ++i)
		{
			auto val = buildExpr(*args[i]);
			if (tupleType && i < tupleType->types().size())
				val = TypeCoercion::implicitNumericCast(std::move(val), tupleType->types()[i], m_loc);
			else if (arc4StructType)
			{
				for (auto const& [fname, ftype]: arc4StructType->fields())
					if (fname == *names[i])
					{
						val = TypeCoercion::implicitNumericCast(std::move(val), ftype, m_loc);
						break;
					}
			}
			fieldValues[*names[i]] = std::move(val);
		}
	}
	else
	{
		// Positional arguments: MyStruct(val1, val2)
		if (auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(solType))
		{
			auto const& members = structType->structDefinition().members();
			for (size_t i = 0; i < args.size() && i < members.size(); ++i)
			{
				auto val = buildExpr(*args[i]);
				if (tupleType && i < tupleType->types().size())
					val = TypeCoercion::implicitNumericCast(std::move(val), tupleType->types()[i], m_loc);
				else if (arc4StructType && i < arc4StructType->fields().size())
					val = TypeCoercion::implicitNumericCast(std::move(val), arc4StructType->fields()[i].second, m_loc);
				fieldValues[members[i]->name()] = std::move(val);
			}
		}
	}

	// ARC4Struct: wrap mismatched fields in ARC4Encode
	if (arc4StructType)
	{
		for (auto const& [fname, ftype]: arc4StructType->fields())
		{
			auto it = fieldValues.find(fname);
			if (it != fieldValues.end() && it->second->wtype != ftype)
			{
				auto encode = std::make_shared<awst::ARC4Encode>();
				encode->sourceLocation = m_loc;
				encode->wtype = ftype;
				encode->value = std::move(it->second);
				it->second = std::move(encode);
			}
		}
		auto newStruct = std::make_shared<awst::NewStruct>();
		newStruct->sourceLocation = m_loc;
		newStruct->wtype = wtype;
		newStruct->values = std::move(fieldValues);
		return newStruct;
	}

	auto structExpr = std::make_shared<awst::NamedTupleExpression>();
	structExpr->sourceLocation = m_loc;
	structExpr->wtype = wtype;
	structExpr->values = std::move(fieldValues);
	return structExpr;
}

} // namespace puyasol::builder::sol_ast
