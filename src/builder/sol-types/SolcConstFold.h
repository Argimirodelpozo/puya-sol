#pragma once

/// @file SolcConstFold.h
/// THE canonical constant-folding entry points (fable-review.md item 1):
/// builders never fold constants themselves — they ask solc, and lower only
/// the non-constant residue.
///
/// solc hands us compile-time constants two ways:
///  (a) the ANNOTATION — the TypeChecker folds constant expressions and types
///      them RationalNumberType; the annotation IS the fold. Expression
///      builders call foldAnnotated() FIRST (before lowering operands).
///  (b) the EVALUATOR — a constant VariableDeclaration is typed at its
///      DECLARED type (not rational), so its value needs
///      ConstantEvaluator::tryEvaluate plus the literal edge cases the
///      evaluator won't fold (bool, hex address/bytesN literals, string
///      packing): constantVarEvmWord().
///
/// RULE (never violate): a fold happens only when solc evaluated the WHOLE
/// expression — never fold "around" a runtime operand. A fold that swallows a
/// runtime-reverting subexpression is a soundness bug; see the
/// `-type(intN).min` fast-path bug (guard test_const_negate_typemin) and the
/// missing-revert-under-fold class (test_dce_reverting_subexpr*).

#include "awst/Node.h"

#include <libsolutil/Numeric.h>

#include <memory>
#include <optional>

namespace solidity::frontend
{
class Expression;
class VariableDeclaration;
}

namespace puyasol::builder
{
class TypeMapper;

class SolcConstFold
{
public:
	/// If solc folded `_expr` to a NON-FRACTIONAL rational (the expression's
	/// annotation type is RationalNumberType), emit its value as a typed AWST
	/// IntegerConstant (uint64/biguint per the mapped type, negative values in
	/// 256-bit two's complement per RationalNumberType::literalValue). Returns
	/// nullptr when the expression is not a solc-folded integer constant —
	/// callers then lower the expression normally.
	static std::shared_ptr<awst::Expression> foldAnnotated(
		solidity::frontend::Expression const& _expr,
		TypeMapper& _typeMapper,
		awst::SourceLocation const& _loc);

	/// Case (b): the expression is typed at a DECLARED integer type (not
	/// rational) yet is compile-time constant — arithmetic over constant
	/// variables (`-M`, `A / B`, `A % B << 2`). Folds ONLY when every
	/// integer-typed node in the subtree evaluates IN RANGE of its OWN
	/// annotated type: an out-of-range intermediate means the runtime lowering
	/// would revert (checked) or wrap (unchecked), so rational evaluation
	/// diverges — the fold is rejected and the normal lowering runs. That is
	/// what keeps `int8 constant M = type(int8).min; -M` REVERTING (128 is out
	/// of int8 range → no fold → checked path) and keeps
	/// `unchecked { (P+P)/P }` computing on the WRAPPED intermediate.
	/// Node whitelist: literals, constant-variable identifiers, parens,
	/// unary -/~, binary + - * / % ** << >> & | ^, and rational-typed leaves
	/// (`type(T).min`). Conversions, calls and ternaries deliberately do NOT
	/// fold (conversion truncation has its own semantics). Ops the evaluator
	/// can't compute simply fail to fold — the guard only ever REJECTS.
	static std::shared_ptr<awst::Expression> foldTyped(
		solidity::frontend::Expression const& _expr,
		awst::SourceLocation const& _loc);

	/// A constant VariableDeclaration's value as the 32-byte EVM word inline
	/// assembly observes: bytesN values left-aligned per the DECLARED type,
	/// string literals packed left-aligned, bool as 0/1. Backed by solc's
	/// ConstantEvaluator (which itself recurses through chained constants and
	/// constant arithmetic); the literal fast-paths cover what it won't fold.
	/// nullopt = not a compile-time-resolvable constant.
	static std::optional<solidity::u256> constantVarEvmWord(
		solidity::frontend::VariableDeclaration const& _varDecl);

	/// fable-review.md item 2 — solc's TypeChecker-computed effect fact.
	/// True iff solc marked the expression PURE: no state read/write, no
	/// environment dependence. Evaluating a pure expression TWICE is
	/// unobservable, so this licenses skipping SingleEvaluation/EvalOnce
	/// wrapping (DUPLICATION).
	///
	/// ⚠️ It does NOT license ELISION or reordering across control flow:
	/// pure ≠ cannot revert (`a / b` is pure yet reverts at b==0). Operand
	/// evaluation must never become conditional on anything Solidity doesn't
	/// make it conditional on — see test_dce_reverting_subexpr* for the class
	/// of bug that rule prevents. There is deliberately no `canElide()` here.
	static bool isEffectFree(solidity::frontend::Expression const& _expr);
};

} // namespace puyasol::builder
