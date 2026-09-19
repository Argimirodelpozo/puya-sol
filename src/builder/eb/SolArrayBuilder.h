#pragma once

#include "builder/eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

/// Instance builder for Solidity typed array types (T[], T[N]).
/// NOT for string or bytes (those have their own builders).
///
/// Handles:
///   - index: arr[i] → IndexExpression with ARC4Decode if needed
///   - compare: not supported for arrays (returns nullptr)
class SolArrayBuilder: public InstanceBuilder
{
public:
	SolArrayBuilder(
		ContractContext& _ctx,
		solidity::frontend::ArrayType const* _arrayType,
		std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)), m_arrayType(_arrayType)
	{
	}

	solidity::frontend::Type const* solType() const override { return m_arrayType; }

	std::unique_ptr<InstanceBuilder> index(
		InstanceBuilder& _idx, awst::SourceLocation const& _loc) override;

	/// rvalue: validates enum values and sign-extends decoded signed elements.
	std::shared_ptr<awst::Expression> resolve() override;
	/// lvalue: bare decoded element (CommaExpression from sign-extend is not an lvalue).
	std::shared_ptr<awst::Expression> resolve_lvalue() override;

private:
	solidity::frontend::ArrayType const* m_arrayType;

	/// Set by index(); resolve_lvalue() retains the unvalidated element location.
	solidity::frontend::Type const* m_elementType = nullptr;
	awst::SourceLocation m_elementLoc{};
};

} // namespace puyasol::builder::eb
