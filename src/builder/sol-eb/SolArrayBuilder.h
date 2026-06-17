#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

/// Instance builder for Solidity typed array types (T[], T[N]).
/// NOT for string or bytes (those have their own builders).
///
/// Handles:
///   - index: arr[i] → IndexExpression with ARC4Decode if needed
///   - member_access: .length → ArrayLength or len intrinsic
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

	std::unique_ptr<NodeBuilder> member_access(
		std::string const& _name, awst::SourceLocation const& _loc) override;

	/// rvalue: sign-extends decoded signed sub-256 elements (see index()).
	std::shared_ptr<awst::Expression> resolve() override;
	/// lvalue: bare decoded element (CommaExpression from sign-extend is not an lvalue).
	std::shared_ptr<awst::Expression> resolve_lvalue() override;

private:
	solidity::frontend::ArrayType const* m_arrayType;

	/// Set by index() for signed sub-256 elements (e.g. int128); resolve() sign-extends on read.
	solidity::frontend::Type const* m_signExtendElem = nullptr;
	awst::SourceLocation m_signExtendLoc{};

	/// Get the AWST element type from the base array WType.
	awst::WType const* elementType() const;
};

/// Instance builder for Solidity mapping types (index() not yet wired — lives in old code).
class SolMappingBuilder: public InstanceBuilder
{
public:
	SolMappingBuilder(
		ContractContext& _ctx,
		solidity::frontend::MappingType const* _mappingType,
		std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)), m_mappingType(_mappingType)
	{
	}

	solidity::frontend::Type const* solType() const override { return m_mappingType; }

private:
	solidity::frontend::MappingType const* m_mappingType;
};

} // namespace puyasol::builder::eb
