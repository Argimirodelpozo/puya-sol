#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

/// Assignment expressions: =, +=, -=, *=, /=, etc.
/// Handles tuple decomposition, struct copy-on-write, bytes element assignment,
/// ARC4 encoding for storage targets, and compound assignment operators.
class SolAssignment: public SolExpression
{
public:
	SolAssignment(eb::BuilderContext& _ctx, solidity::frontend::Assignment const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	solidity::frontend::Assignment const& m_assignment;

	std::shared_ptr<awst::Expression> handleTupleAssignment(
		std::shared_ptr<awst::Expression> _target,
		std::shared_ptr<awst::Expression> _value);

	std::shared_ptr<awst::Expression> handleBytesElementAssignment(
		awst::IndexExpression const* _indexExpr,
		std::shared_ptr<awst::Expression> _value);

	std::shared_ptr<awst::Expression> handleStructFieldAssignment(
		awst::FieldExpression const* _fieldExpr,
		std::shared_ptr<awst::Expression> _value,
		std::shared_ptr<awst::Expression> _unwrappedTarget);

	/// Build a TupleExpression with one field replaced.
	std::shared_ptr<awst::Expression> buildTupleWithUpdatedField(
		std::shared_ptr<awst::Expression> _base,
		std::string const& _fieldName,
		std::shared_ptr<awst::Expression> _newValue);
};

} // namespace puyasol::builder::sol_ast
