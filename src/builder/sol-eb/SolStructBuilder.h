#pragma once

#include "builder/sol-eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

/// Instance builder for Solidity struct types.
///
/// Structs are encoded as either ARC4Struct or WTuple on AVM.
/// Handles:
///   - compare: Eq/Ne (compare encoded bytes for ARC4Struct)
///   - member_access: field access with ARC4 decode if needed
class SolStructBuilder: public InstanceBuilder
{
public:
	SolStructBuilder(
		BuilderContext& _ctx,
		solidity::frontend::StructType const* _structType,
		std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)), m_structType(_structType)
	{
	}

	solidity::frontend::Type const* solType() const override { return m_structType; }

	std::unique_ptr<InstanceBuilder> compare(
		InstanceBuilder& _other, BuilderComparisonOp _op,
		awst::SourceLocation const& _loc) override;

private:
	solidity::frontend::StructType const* m_structType;
};

} // namespace puyasol::builder::eb
