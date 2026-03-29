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
		BuilderContext& _ctx,
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

private:
	solidity::frontend::ArrayType const* m_arrayType;

	/// Get the AWST element type from the base array WType.
	awst::WType const* elementType() const;
};

/// Instance builder for Solidity mapping types.
///
/// Handles:
///   - index: mapping[key] → BoxValueExpression with key hashing
///   - Not wired into visitor yet — mapping index is complex (box storage, nested mappings)
class SolMappingBuilder: public InstanceBuilder
{
public:
	SolMappingBuilder(
		BuilderContext& _ctx,
		solidity::frontend::MappingType const* _mappingType,
		std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)), m_mappingType(_mappingType)
	{
	}

	solidity::frontend::Type const* solType() const override { return m_mappingType; }

	// index() not yet implemented — mapping access is deeply intertwined
	// with box storage semantics and stays in old IndexAccessBuilder for now.

private:
	solidity::frontend::MappingType const* m_mappingType;
};

} // namespace puyasol::builder::eb
