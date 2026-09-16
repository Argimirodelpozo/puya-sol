#pragma once

#include "builder/eb/NodeBuilder.h"

#include <libsolidity/ast/Types.h>

namespace puyasol::builder::eb
{

/// Instance builder for Solidity struct types.
///
/// Structs are encoded as either ARC4Struct or WTuple on AVM.
class SolStructBuilder: public InstanceBuilder
{
public:
	SolStructBuilder(
		ContractContext& _ctx,
		solidity::frontend::StructType const* _structType,
		std::shared_ptr<awst::Expression> _expr)
		: InstanceBuilder(_ctx, std::move(_expr)), m_structType(_structType)
	{
	}

	solidity::frontend::Type const* solType() const override { return m_structType; }

private:
	solidity::frontend::StructType const* m_structType;
};

} // namespace puyasol::builder::eb
