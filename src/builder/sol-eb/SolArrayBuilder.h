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

	/// rvalue read: sign-extends a decoded signed sub-256 element (see index()).
	std::shared_ptr<awst::Expression> resolve() override;
	/// lvalue (assignment target): the *bare* decoded element. Must NOT sign-
	/// extend — the sign-extension wraps the value in a CommaExpression, which
	/// is not a valid assignment target.
	std::shared_ptr<awst::Expression> resolve_lvalue() override;

private:
	solidity::frontend::ArrayType const* m_arrayType;

	/// Set by index() when this builder wraps a decoded signed sub-256 element
	/// (e.g. int128). resolve() then sign-extends it to canonical 256-bit on
	/// read; resolve_lvalue() leaves it bare. Null for every other case.
	solidity::frontend::Type const* m_signExtendElem = nullptr;
	awst::SourceLocation m_signExtendLoc{};

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
		ContractContext& _ctx,
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
