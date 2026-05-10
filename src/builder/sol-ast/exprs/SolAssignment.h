#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/AST.h>
#include <optional>

namespace puyasol::builder::sol_ast
{

/// Assignment expressions: =, +=, -=, *=, /=, etc.
/// Handles tuple decomposition, struct copy-on-write, bytes element assignment,
/// ARC4 encoding for storage targets, and compound assignment operators.
class SolAssignment: public SolExpression
{
public:
	SolAssignment(eb::ContractContext& _ctx, solidity::frontend::Assignment const& _node);
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

	/// LHS-shape early-out handlers: each returns the assignment-as-expression
	/// result if it owns the shape, or std::nullopt to fall through to the
	/// generic dispatch in `toAwst`.

	/// `tx = v` / `tx += v` where `tx` is a `transient` state variable —
	/// routes through TransientStorage's scratch-blob layout.
	std::optional<std::shared_ptr<awst::Expression>> tryHandleTransientStateWrite();

	/// `m = m2` where `m` is a local storage-pointer (`mapping(K=>V) storage m`).
	/// Updates the compile-time alias for state-var aliases; for runtime-bound
	/// mapping-key params, emits an actual bytes assignment.
	std::optional<std::shared_ptr<awst::Expression>> tryHandleStoragePointerReassign();

	/// `arr[i] = v` where `arr` is a multi-box state-var array (encoded size
	/// exceeds AVM's 32KB box cap). Computes runtime `page = i / elemsPerBox`
	/// and `offset = (i % elemsPerBox) * elemSize`, encodes the rhs as ARC4
	/// element bytes, and emits `box_replace(<name> ++ itob(page), offset, bytes)`.
	std::optional<std::shared_ptr<awst::Expression>> tryHandleMultiBoxArrayWrite();
};

} // namespace puyasol::builder::sol_ast
