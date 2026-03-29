#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

/// Instance builder for Solidity enum types.
///
/// Enums are encoded as uint64 on AVM. Handles:
///   - compare: all 6 operators via NumericComparisonExpression
///   - bool_eval: value != 0
class SolEnumBuilder: public InstanceBuilder
{
public:
	SolEnumBuilder(
		BuilderContext& _ctx,
		solidity::frontend::EnumType const* _enumType,
		std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)), m_enumType(_enumType)
	{
	}

	solidity::frontend::Type const* solType() const override { return m_enumType; }

	std::unique_ptr<InstanceBuilder> compare(
		InstanceBuilder& _other, BuilderComparisonOp _op,
		awst::SourceLocation const& _loc) override;

	std::unique_ptr<InstanceBuilder> bool_eval(
		awst::SourceLocation const& _loc, bool _negate = false) override;

private:
	solidity::frontend::EnumType const* m_enumType;
};

} // namespace puyasol::builder::eb
