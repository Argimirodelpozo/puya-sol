#pragma once

#include "awst/Node.h"
#include "builder/sol-ast/Context.h"

#include <libsolidity/parsing/Token.h>

#include <memory>

namespace puyasol::builder::eb
{
class ContractContext;

/// Build an AWST binary-op from already-resolved operands (fallback when sol-eb
/// type-builder dispatch fails). Chooses uint64/biguint/bytes based on types;
/// emits side-effect statements (e.g. exp loop) into the active pre-effect frame.
std::shared_ptr<awst::Expression> buildBinaryOp(
	ContractContext& _ctx,
	sol_ast::Context& _scope,
	solidity::frontend::Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::WType const* _resultType,
	awst::SourceLocation const& _loc
);

} // namespace puyasol::builder::eb
