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
		std::shared_ptr<awst::Expression> _value,
		solidity::frontend::TupleExpression const* _sourceLhs = nullptr);

	std::shared_ptr<awst::Expression> handleBytesElementAssignment(
		awst::IndexExpression const* _indexExpr,
		std::shared_ptr<awst::Expression> _value);

	/// Copy-on-write write-back when the bytes-element target is a struct
	/// field: `s.b[i] = v` where `s.b` is `bytes` (ARC4-encoded as byte[]).
	/// _newBytes is the already-computed raw bytes for the field (replace3
	/// result). Builds a NewStruct chain and emits the struct assignment.
	std::shared_ptr<awst::Expression> buildStructFieldBytesWrite(
		awst::FieldExpression const* _fieldExpr,
		awst::ARC4Struct const* _structType,
		std::shared_ptr<awst::Expression> _newBytes);

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
