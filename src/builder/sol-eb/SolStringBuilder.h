#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

/// Instance builder for Solidity string type.
///
/// Handles:
///   - compare: Eq/Ne → BytesComparisonExpression
///   - bool_eval: len(s) != 0
class SolStringBuilder: public InstanceBuilder
{
public:
	SolStringBuilder(
		BuilderContext& _ctx,
		solidity::frontend::Type const* _solType,
		std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)), m_solType(_solType)
	{
	}

	solidity::frontend::Type const* solType() const override { return m_solType; }

	std::unique_ptr<InstanceBuilder> compare(
		InstanceBuilder& _other, BuilderComparisonOp _op,
		awst::SourceLocation const& _loc) override;

	std::unique_ptr<InstanceBuilder> bool_eval(
		awst::SourceLocation const& _loc, bool _negate = false) override;

private:
	solidity::frontend::Type const* m_solType;
};

/// Instance builder for Solidity dynamic bytes type.
///
/// Handles:
///   - compare: Eq/Ne → BytesComparisonExpression
///   - bool_eval: len(b) != 0
class SolDynamicBytesBuilder: public InstanceBuilder
{
public:
	SolDynamicBytesBuilder(
		BuilderContext& _ctx,
		solidity::frontend::Type const* _solType,
		std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)), m_solType(_solType)
	{
	}

	solidity::frontend::Type const* solType() const override { return m_solType; }

	std::unique_ptr<InstanceBuilder> compare(
		InstanceBuilder& _other, BuilderComparisonOp _op,
		awst::SourceLocation const& _loc) override;

	std::unique_ptr<InstanceBuilder> bool_eval(
		awst::SourceLocation const& _loc, bool _negate = false) override;

private:
	solidity::frontend::Type const* m_solType;
};

} // namespace puyasol::builder::eb
