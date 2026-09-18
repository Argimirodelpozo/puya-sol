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

	/// Fold a declared integer expression using solc's checked ConstantEvaluator.
	/// Solc rejects overflowing intermediates and unsupported expressions; normal
	/// lowering then preserves checked reverts or unchecked wrapping. The final
	/// value must also fit the expression's annotated integer type.
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

	/// Constant address through parentheses, constant declarations and lossless
	/// address/integer conversions. Unsupported or potentially trapping expressions
	/// return nullopt; never infer a value from source spelling.
	static std::optional<solidity::u256> constantAddress(
		solidity::frontend::Expression const& _expression);
};

} // namespace puyasol::builder
