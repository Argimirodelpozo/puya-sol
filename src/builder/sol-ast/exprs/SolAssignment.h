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

	// ── toAwst pipeline phases (post-buildExpr) ─────────────────────────
	//
	// Each named phase represents a distinct step in the
	// shape-handler-then-generic-finalization pipeline. The orchestrator
	// `toAwst` calls them in order; each `try*` may claim the assignment
	// (return non-nullopt) or fall through; each `apply*` mutates `value`
	// (or `target`) in place and returns the next value to thread.

	/// `arr.push() = v` is a shape Solidity allows but we can't model with
	/// references. Detect it before buildExpr(LHS) and stash the RHS as
	/// `pendingArrayPushValue` so SolArrayMethod folds it into the
	/// ArrayExtend. Returns the resulting ArrayExtend if the pattern
	/// applies, else std::nullopt.
	std::optional<std::shared_ptr<awst::Expression>> tryHandlePushAssignRewrite(
		solidity::frontend::Token _op);

	/// EVM panics (0x21) on assigning out-of-range enum values. Pre-emit
	/// an assert if LHS is an enum type. Returns the (possibly coerced) value.
	std::shared_ptr<awst::Expression> applyEnumRangeCheck(
		std::shared_ptr<awst::Expression> _value,
		solidity::frontend::Token _op);

	/// `slot = arr` where slot is a biguint (256-bit slot offset) and
	/// `arr` is a static-sized array literal — expand to per-element
	/// `__storage_write(slot+j, arr[j])`.
	std::optional<std::shared_ptr<awst::Expression>> trySlotBasedArrayWrite(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> const& _target,
		std::shared_ptr<awst::Expression> const& _value);

	/// `slot = v` where slot is a computed biguint slot — emit
	/// `__storage_write(btoi(slot), v)` (with read-modify-write for
	/// compound assigns). Returns the void-equivalent result.
	std::optional<std::shared_ptr<awst::Expression>> trySlotBasedScalarWrite(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> const& _target,
		std::shared_ptr<awst::Expression>& _value);

	/// `(a, b) = expr` tuple destructuring — delegates to
	/// `handleTupleAssignment`. Returns nullopt if target isn't a tuple.
	std::optional<std::shared_ptr<awst::Expression>> tryTupleAssignment(
		std::shared_ptr<awst::Expression>& _target,
		std::shared_ptr<awst::Expression>& _value);

	/// `b[i] = v` where `b` is bytes — delegates to
	/// `handleBytesElementAssignment`. Returns nullopt if target isn't an
	/// IndexExpression on bytes.
	std::optional<std::shared_ptr<awst::Expression>> tryBytesElemAssignment(
		std::shared_ptr<awst::Expression> const& _target,
		std::shared_ptr<awst::Expression>& _value);

	/// `s.f = v` for ARC4 struct field, or named-WTuple field — delegates
	/// to `handleStructFieldAssignment` / inline WTuple-named-field path.
	std::optional<std::shared_ptr<awst::Expression>> tryStructOrNamedTupleFieldAssignment(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> const& _target,
		std::shared_ptr<awst::Expression>& _value);

	/// Compound assignment (`+=`, `-=`, …): read current value of target,
	/// apply binary op, return the computed new value. For simple Assign
	/// just passes value through.
	std::shared_ptr<awst::Expression> applyCompoundAssignment(
		solidity::frontend::Token _op,
		std::shared_ptr<awst::Expression> const& _target,
		std::shared_ptr<awst::Expression> _value);

	/// Type coercion at the assignment boundary: int→bytes[N], string→bytes,
	/// reinterpret-cast string↔bytes, numeric narrowing.
	std::shared_ptr<awst::Expression> applyAssignmentTypeCoercion(
		std::shared_ptr<awst::Expression> _value,
		std::shared_ptr<awst::Expression> const& _target);

	/// If value's wtype differs from target and target is ARC4-typed, emit
	/// the appropriate ARC4 encode (with widening / narrowing handling).
	std::shared_ptr<awst::Expression> applyArc4EncodeIfNeeded(
		std::shared_ptr<awst::Expression> _value,
		std::shared_ptr<awst::Expression> const& _target);

	/// For static-array-of-dynamic-elem and per-entry-mapping boxes, emit
	/// a guarded box_put or box_create as a pending pre-statement so the
	/// per-entry box exists before the subsequent box_replace.
	void maybePrePopulateBox(
		std::shared_ptr<awst::Expression> const& _target);
};

} // namespace puyasol::builder::sol_ast
