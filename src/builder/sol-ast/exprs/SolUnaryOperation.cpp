/// @file SolUnaryOperation.cpp
/// Migrated from UnaryOperationBuilder.cpp.

#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-ast/exprs/SolUnaryOperation.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>
#include <libsolutil/Numeric.h>
#include <sstream>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

SolUnaryOperation::SolUnaryOperation(
	eb::ContractContext& _ctx, UnaryOperation const& _node)
	: SolExpression(_ctx, _node), m_unaryOp(_node)
{
}

bool SolUnaryOperation::isBigUInt(awst::WType const* _type) const
{
	return _type == awst::WType::biguintType();
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleNot(
	std::shared_ptr<awst::Expression> _operand)
{
	auto e = awst::makeNot(std::move(_operand), m_loc);
	return e;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleNegate(
	std::shared_ptr<awst::Expression> _operand)
{
	// Check if the operand or result type is a signed integer.
	// For constant expressions like (-2), the operand type is RationalNumberType
	// but the result type is signed IntegerType.
	auto const* solType = m_unaryOp.subExpression().annotation().type;
	if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(solType))
		solType = &udvt->underlyingType();
	auto const* intType = dynamic_cast<IntegerType const*>(solType);
	if (!intType || !intType->isSigned())
	{
		// Try the result type (for constant expressions)
		auto const* resultType = m_unaryOp.annotation().type;
		if (auto const* udvt2 = dynamic_cast<UserDefinedValueType const*>(resultType))
			resultType = &udvt2->underlyingType();
		auto const* resultIntType = dynamic_cast<IntegerType const*>(resultType);
		if (resultIntType && resultIntType->isSigned())
			intType = resultIntType;
	}

	if (intType && intType->isSigned())
	{
		unsigned bits = intType->numBits();
		// Signed negation: -x = (2^N - x) mod 2^N
		// Overflow check: -INT_MIN overflows (x == 2^(N-1))
		std::string pow2NStr, halfNStr;
		if (bits == 256)
		{
			pow2NStr = kPow2_256;
			halfNStr = "57896044618658097711785492504343953926634992332820282019728792003956564819968";
		}
		else
		{
			solidity::u256 pow2N = solidity::u256(1) << bits;
			solidity::u256 halfN = solidity::u256(1) << (bits - 1);
			std::ostringstream oss1, oss2;
			oss1 << pow2N; pow2NStr = oss1.str();
			oss2 << halfN; halfNStr = oss2.str();
		}

		auto makeBiguintConst = [&](std::string const& val) {
			auto c = awst::makeIntegerConstant(val, m_loc, awst::WType::biguintType());
			return c;
		};

		// Ensure operand is biguint
		auto operand = std::move(_operand);
		if (operand->wtype == awst::WType::uint64Type())
		{
			auto itob = awst::makeItob(std::move(operand), m_loc);
			operand = awst::makeAsBiguint(std::move(itob), m_loc);
		}

		// Mask to N bits first (uint64 two's complement may be wider)
		if (bits < 256)
		{
			auto mask = awst::makeBigUIntBinOp(std::move(operand), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);
			operand = std::move(mask);
		}

		// Checked: assert(x != INT_MIN) i.e. x != 2^(N-1)
		if (!m_scope.isUnchecked())
		{
			auto cmp = awst::makeNumericCompare(operand, awst::NumericComparison::Ne, makeBiguintConst(halfNStr), m_loc);

			m_ctx.queuePreStmt(awst::makeAssert(std::move(cmp), m_loc, "signed negation overflow"), m_loc);
		}

		// -x = (2^N - x) mod 2^N
		// For 256-bit: 2^256 - 0 would overflow, so use if/else via temp variable
		std::shared_ptr<awst::Expression> negated;
		if (bits == 256)
		{
			// Emit: __neg_tmp = 0; if (x != 0) { __neg_tmp = (2^256 - x) % 2^256; }
			// Then use __neg_tmp as the result.
			std::string tmpName = "__neg_tmp_" + std::to_string(m_unaryOp.id());

			// __neg_tmp = 0
			auto tmpVar = awst::makeVarExpression(tmpName, awst::WType::biguintType(), m_loc);

			auto initStmt = awst::makeAssignmentStatement(tmpVar, makeBiguintConst("0"), m_loc);
			m_ctx.prePendingStatements.push_back(std::move(initStmt));

			// if (x != 0) { __neg_tmp = (2^256 - x) % 2^256; }
			auto isNonZero = awst::makeNumericCompare(operand, awst::NumericComparison::Ne, makeBiguintConst("0"), m_loc);

			auto sub = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, operand, m_loc);

			auto mod = awst::makeBigUIntBinOp(std::move(sub), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);

			auto assignTmp = awst::makeAssignmentStatement(tmpVar, std::move(mod), m_loc);

			auto ifBody = awst::makeBlock(m_loc);
			ifBody->body.push_back(std::move(assignTmp));

			m_ctx.prePendingStatements.push_back(awst::makeIfElse(
				std::move(isNonZero), std::move(ifBody), nullptr, m_loc));

			negated = tmpVar;
		}
		else
		{
			auto sub = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, std::move(operand), m_loc);

			auto mod = awst::makeBigUIntBinOp(std::move(sub), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);

			negated = std::move(mod);
		}

		// Convert back to uint64 for ≤64-bit types
		if (bits <= 64)
		{
			return awst::makeBiguintToUInt64(std::move(negated), m_loc);
		}
		return negated;
	}

	// For biguint constant negation (e.g., (-2) where type is RationalNumberType),
	// produce two's complement directly: 2^256 - value
	if (isBigUInt(_operand->wtype))
	{
		if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(_operand.get()))
		{
			solidity::u256 val(intConst->value);
			// Allow val == 2^255 (the largest positive value whose negation
			// is a valid signed int256: -2^255 = type(int256).min). The strict
			// less-than missed this, so `-2**255` fell through to runtime
			// `0 - 2^255`, which crashes with "byte math would have negative
			// result" since AVM `b-` rejects negative results.
			if (val > 0 && val <= (solidity::u256(1) << 255))
			{
				// 2^256 - val (two's complement negation)
				static const std::string pow256Str =
					kPow2_256;
				solidity::u256 pow256(pow256Str);
				solidity::u256 negVal = pow256 - val;
				std::ostringstream oss;
				oss << negVal;
				auto result = awst::makeIntegerConstant(oss.str(), m_loc, awst::WType::biguintType());
				return result;
			}
		}

		// Non-constant unsigned negation: 0 - x
		auto zero = awst::makeZero(m_loc, awst::WType::biguintType());
		auto e = awst::makeBigUIntBinOp(std::move(zero), awst::BigUIntBinaryOperator::Sub, std::move(_operand), m_loc);
		return e;
	}
	// For uint64 constant negation, produce two's complement biguint.
	// We can't safely narrow to uint64 here — the surrounding context
	// might be int256 (biguint storage) and a uint64 result would
	// sign-extend wrong (uint64(2^64-1) → biguint(2^64-1) instead of
	// biguint(2^256-1)). The wide-form constant survives narrowing
	// to uint64 slots correctly via `extract3 + btoi`, while uint64
	// constants don't survive widening to biguint slots.
	if (_operand->wtype == awst::WType::uint64Type())
	{
		if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(_operand.get()))
		{
			solidity::u256 val(intConst->value);
			if (val > 0)
			{
				static const std::string pow256Str =
					kPow2_256;
				solidity::u256 pow256(pow256Str);
				solidity::u256 negVal = pow256 - val;
				std::ostringstream oss;
				oss << negVal;
				auto result = awst::makeIntegerConstant(oss.str(), m_loc, awst::WType::biguintType());
				return result;
			}
		}
	}
	auto zero2 = awst::makeZero(m_loc, _operand->wtype);
	if (_operand->wtype == awst::WType::uint64Type())
	{
		auto e = awst::makeUInt64BinOp(std::move(zero2), awst::UInt64BinaryOperator::Sub, std::move(_operand), m_loc);
		return e;
	}
	auto e = awst::makeBigUIntBinOp(std::move(zero2), awst::BigUIntBinaryOperator::Sub, std::move(_operand), m_loc);
	return e;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleBitNot(
	std::shared_ptr<awst::Expression> _operand)
{
	auto* resultType = _operand->wtype;
	if (_operand->wtype == awst::WType::uint64Type())
	{
		// Use the correct bit-width mask from the Solidity type
		unsigned maskBits = 64;
		auto const* solType = m_unaryOp.subExpression().annotation().type;
		if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(solType))
			solType = &udvt->underlyingType();
		if (auto const* intType = dynamic_cast<IntegerType const*>(solType))
			maskBits = intType->numBits();

		solidity::u256 mask = (maskBits >= 64)
			? solidity::u256("18446744073709551615")
			: (solidity::u256(1) << maskBits) - 1;
		std::ostringstream oss;
		oss << mask;

		auto maxVal = awst::makeIntegerConstant(oss.str(), m_loc);
		auto xorOp = awst::makeUInt64BinOp(std::move(_operand), awst::UInt64BinaryOperator::BitXor, std::move(maxVal), m_loc);
		return xorOp;
	}
	auto expr = std::move(_operand);
	if (expr->wtype == awst::WType::biguintType())
	{
		auto cast = awst::makeAsBytes(std::move(expr), m_loc);
		expr = std::move(cast);
	}
	auto const* exprWtype = expr->wtype;
	auto e = awst::makeBitInvert(std::move(expr), exprWtype, m_loc);
	if (resultType == awst::WType::biguintType())
	{
		auto castBack = awst::makeReinterpretCast(std::move(e), resultType, m_loc);
		return castBack;
	}
	return e;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleIncDec(
	std::shared_ptr<awst::Expression> _operand)
{
	bool isPrefix = m_unaryOp.isPrefixOperation();
	bool isInc = (m_unaryOp.getOperator() == Token::Inc);

	// Transient state variable inc/dec: read/compute/write via TransientStorage.
	// The normal path builds an AssignmentStatement with the read expr as
	// target, which isn't an lvalue for packed transient slots.
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_unaryOp.subExpression()))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			if (varDecl->isStateVariable()
				&& varDecl->referenceLocation() == VariableDeclaration::Location::Transient
				&& m_ctx.transientStorage
				&& m_ctx.transientStorage->isTransient(*varDecl))
			{
				auto const* info = m_ctx.transientStorage->getVarInfoById(varDecl->id());
				auto* wt = info ? info->wtype : _operand->wtype;
				auto one = (wt == awst::WType::biguintType())
					? awst::makeOne(m_loc, awst::WType::biguintType())
					: awst::makeOne(m_loc);
				std::shared_ptr<awst::Expression> newValue;
				if (wt == awst::WType::biguintType())
					newValue = awst::makeBigUIntBinOp(_operand,
						isInc ? awst::BigUIntBinaryOperator::Add : awst::BigUIntBinaryOperator::Sub,
						std::move(one), m_loc);
				else
					newValue = awst::makeUInt64BinOp(_operand,
						isInc ? awst::UInt64BinaryOperator::Add : awst::UInt64BinaryOperator::Sub,
						std::move(one), m_loc);
				auto writeStmt = m_ctx.transientStorage->buildWrite(
					varDecl->name(), newValue, m_loc);
				if (!writeStmt)
					return _operand;
				if (isPrefix)
				{
					m_ctx.prePendingStatements.push_back(std::move(writeStmt));
					// Re-read for the expression value (post-update)
					return m_ctx.transientStorage->buildRead(varDecl->name(), wt, m_loc);
				}
				else
				{
					m_ctx.pendingStatements.push_back(std::move(writeStmt));
					return _operand;
				}
			}
		}
	}

	bool isSigned = false;
	if (auto const* intType = dynamic_cast<IntegerType const*>(
			m_unaryOp.subExpression().annotation().type))
		isSigned = intType->isSigned();

	// Unwrap BoxValueExpression for reads
	if (dynamic_cast<awst::BoxValueExpression const*>(_operand.get()))
		_operand = builder::StorageMapper::makeStateGetWithDefault(_operand, _operand->wtype, m_loc);

	static const std::string pow256 =
		kPow2_256;
	static const std::string pow256Minus1 =
		"115792089237316195423570985008687907853269984665640564039457584007913129639935";

	// Get signed bit width for overflow checks
	unsigned signedBits = 0;
	if (isSigned)
	{
		if (auto const* it = dynamic_cast<IntegerType const*>(
				m_unaryOp.subExpression().annotation().type))
			signedBits = it->numBits();
	}

	auto makeNewValue = [&](std::shared_ptr<awst::Expression> base)
		-> std::shared_ptr<awst::Expression>
	{
		if (isSigned && signedBits > 0)
		{
			// Signed inc/dec with two's complement wrapping + overflow check
			std::string pow2NStr2, halfNStr2;
			if (signedBits == 256)
			{
				pow2NStr2 = pow256;
				halfNStr2 = "57896044618658097711785492504343953926634992332820282019728792003956564819968";
			}
			else
			{
				solidity::u256 p = solidity::u256(1) << signedBits;
				solidity::u256 h = solidity::u256(1) << (signedBits - 1);
				std::ostringstream o1, o2;
				o1 << p; pow2NStr2 = o1.str();
				o2 << h; halfNStr2 = o2.str();
			}

			auto makeBConst = [&](std::string const& v) {
				auto c = awst::makeIntegerConstant(v, m_loc, awst::WType::biguintType());
				return c;
			};

			// Ensure biguint
			auto val = std::move(base);
			if (val->wtype == awst::WType::uint64Type())
			{
				auto itob = awst::makeItob(std::move(val), m_loc);
				val = awst::makeAsBiguint(std::move(itob), m_loc);
			}

			// Mask to N bits
			if (signedBits < 256)
			{
				auto mask = awst::makeBigUIntBinOp(std::move(val), awst::BigUIntBinaryOperator::Mod, makeBConst(pow2NStr2), m_loc);
				val = std::move(mask);
			}

			// Checked overflow: inc overflows at MAX (half-1), dec underflows at MIN (half)
			if (!m_scope.isUnchecked())
			{
				std::string limitStr;
				if (isInc)
				{
					// MAX = half - 1
					solidity::u256 maxVal = (solidity::u256(1) << (signedBits - 1)) - 1;
					std::ostringstream oss; oss << maxVal; limitStr = oss.str();
				}
				else
					limitStr = halfNStr2; // MIN = half

				auto cmp = awst::makeNumericCompare(val, awst::NumericComparison::Ne, makeBConst(limitStr), m_loc);

				m_ctx.queuePreStmt(awst::makeAssert(std::move(cmp), m_loc, "signed inc/dec overflow"), m_loc);
			}

			// Compute: inc → (val + 1) mod 2^N, dec → (val + 2^N - 1) mod 2^N
			std::shared_ptr<awst::Expression> added;
			if (isInc)
			{
				auto add = awst::makeBigUIntBinOp(std::move(val), awst::BigUIntBinaryOperator::Add, makeBConst("1"), m_loc);
				added = std::move(add);
			}
			else
			{
				// val + (2^N - 1) to avoid negative
				solidity::u256 decOffset = (signedBits == 256)
					? solidity::u256(0) // special: use string directly
					: (solidity::u256(1) << signedBits) - 1;
				std::string decOffsetStr;
				if (signedBits == 256)
					decOffsetStr = pow256Minus1;
				else
				{
					std::ostringstream oss; oss << decOffset; decOffsetStr = oss.str();
				}
				auto add = awst::makeBigUIntBinOp(std::move(val), awst::BigUIntBinaryOperator::Add, makeBConst(decOffsetStr), m_loc);
				added = std::move(add);
			}

			auto mod = awst::makeBigUIntBinOp(std::move(added), awst::BigUIntBinaryOperator::Mod, makeBConst(pow2NStr2), m_loc);

			// Convert back to uint64 for ≤64-bit types
			if (signedBits <= 64)
			{
				return awst::makeBiguintToUInt64(std::move(mod), m_loc);
			}
			return mod;
		}
		else if (isBigUInt(base->wtype))
		{
			auto one = awst::makeOne(m_loc, awst::WType::biguintType());
			auto bin = awst::makeBigUIntBinOp(std::move(base), isInc ? awst::BigUIntBinaryOperator::Add : awst::BigUIntBinaryOperator::Sub, std::move(one), m_loc);
			return bin;
		}
		else
		{
			auto one = awst::makeOne(m_loc);
			auto bin = awst::makeUInt64BinOp(std::move(base), isInc ? awst::UInt64BinaryOperator::Add : awst::UInt64BinaryOperator::Sub, std::move(one), m_loc);
			return bin;
		}
	};

	// Re-read the subexpression for the assignment target. State vars come
	// back wrapped in StateGet, which is a read — unwrap so the assignment
	// lands on the writable BoxValueExpression / AppStateExpression. Also
	// peel ARC4Decode (native-typed read of an ARC4-encoded field) so the
	// write hits the encoded slot — paired with ARC4Encode on the new
	// value below.
	auto makeWriteTarget = [&]() -> std::shared_ptr<awst::Expression>
	{
		auto target = buildExpr(m_unaryOp.subExpression());
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(target.get()))
			target = sg->field;
		if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
			target = decodeExpr->value;
		return target;
	};

	// If the unwrapped write target is ARC4-typed but the new value is
	// native (biguint/uint64), wrap with ARC4Encode. Pairs with the
	// ARC4Decode unwrap in makeWriteTarget.
	auto maybeEncode = [&](std::shared_ptr<awst::Expression> writeTarget,
		std::shared_ptr<awst::Expression> newValue) -> std::shared_ptr<awst::Expression>
	{
		auto const* targetType = writeTarget->wtype;
		if (auto const* arc4 = dynamic_cast<awst::ARC4UIntN const*>(targetType))
			if (newValue->wtype != arc4)
				return awst::makeARC4Encode(std::move(newValue), arc4, m_loc);
		return newValue;
	};

	if (isPrefix)
	{
		auto writeTarget = makeWriteTarget();
		auto newValue = maybeEncode(writeTarget, makeNewValue(_operand));
		return awst::makeAssignmentExpression(
			std::move(writeTarget), std::move(newValue), m_loc, _operand->wtype);
	}
	else
	{
		// Post-increment/decrement: capture the old value in a temp var,
		// emit the variable update as a *pre-pending* statement so it
		// happens before any sibling reads of the same variable, then
		// return the saved old value.
		//
		// Without this, `return a++ + a` evaluates `a` (2nd operand)
		// against the *old* value of `a` because the post-inc assignment
		// sat in pendingStatements — which only fires at the end of the
		// enclosing statement.
		static int postIncCounter = 0;
		std::string tempName = "__postinc_" + std::to_string(postIncCounter++);

		auto tempVar = awst::makeVarExpression(tempName, _operand->wtype, m_loc);

		// Save old value: temp = a
		auto saveStmt = awst::makeAssignmentStatement(tempVar, _operand, m_loc);
		m_ctx.prePendingStatements.push_back(std::move(saveStmt));

		// Compute new value from the saved temp (not re-reading a)
		auto writeTarget = makeWriteTarget();
		auto newValue = maybeEncode(writeTarget, makeNewValue(tempVar));

		// a = temp + 1 (for inc) or temp - 1 (for dec)
		auto incrStmt = awst::makeAssignmentStatement(std::move(writeTarget), std::move(newValue), m_loc);
		m_ctx.prePendingStatements.push_back(std::move(incrStmt));

		return tempVar;
	}
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleDelete(
	std::shared_ptr<awst::Expression> _operand)
{
	// Transient state variable delete: write zero via TransientStorage.
	// Intercept before building the expression so we skip the non-lvalue read.
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_unaryOp.subExpression()))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			if (varDecl->isStateVariable()
				&& varDecl->referenceLocation() == VariableDeclaration::Location::Transient
				&& m_ctx.transientStorage
				&& m_ctx.transientStorage->isTransient(*varDecl))
			{
				auto const* info = m_ctx.transientStorage->getVarInfoById(varDecl->id());
				auto* wt = info ? info->wtype : m_ctx.typeMapper.map(varDecl->type());
				auto zero = builder::StorageMapper::makeDefaultValue(wt, m_loc);
				if (auto stmt = m_ctx.transientStorage->buildWrite(
						varDecl->name(), std::move(zero), m_loc))
					m_ctx.pendingStatements.push_back(std::move(stmt));
				m_scope.eraseFuncPtrTarget(varDecl->id());
				return _operand;
			}
		}
	}

	auto target = buildExpr(m_unaryOp.subExpression());

	// Clear function pointer tracking on delete (e.g., delete y where y is a func ptr)
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_unaryOp.subExpression()))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
			m_scope.eraseFuncPtrTarget(varDecl->id());
	}

	if (auto const* boxExpr = dynamic_cast<awst::BoxValueExpression const*>(target.get()))
	{
		// For top-level dynamic state vars (BoxValueExpression whose key
		// is a literal BytesConstant, of type ARC4DynamicArray /
		// ReferenceArray / dynamic bytes), do NOT emit `box_del`:
		// ApprovalProgramBuilder eagerly box_create's these in __postInit
		// and StorageMapper::makeStateGetWithDefault now reads them via
		// bare BoxValueExpression (asserts box exists). Box deletion
		// would leave subsequent reads asserting — Solidity's `delete a;`
		// expects empty/zero, not "doesn't exist". Emit `a = default`
		// instead (box_put with empty encoding — `0x0000` length header
		// for ARC4 dyn array, `0x` for raw dynamic bytes) so the box
		// stays alive. Mapping values (key = concat) keep the box_del
		// path — their boxes are lazy and re-created on next write.
		auto kind = boxExpr->wtype ? boxExpr->wtype->kind() : awst::WTypeKind::Basic;
		bool dynamicSized =
			kind == awst::WTypeKind::ARC4DynamicArray
			|| kind == awst::WTypeKind::ReferenceArray
			|| (kind == awst::WTypeKind::Bytes
				&& boxExpr->wtype
				&& !dynamic_cast<awst::BytesWType const*>(boxExpr->wtype)->length().has_value());
		bool topLevel = boxExpr->key
			&& std::dynamic_pointer_cast<awst::BytesConstant>(boxExpr->key);
		if (dynamicSized && topLevel)
		{
			auto def = builder::StorageMapper::makeDefaultValue(boxExpr->wtype, m_loc);
			auto put = awst::makeAssignmentExpression(target, std::move(def), m_loc, boxExpr->wtype);
			m_ctx.queueStmt(std::move(put), m_loc);
			return _operand;
		}
		auto stateDelete = awst::makeStateDelete(target, m_loc);
		m_ctx.queueStmt(std::move(stateDelete), m_loc);
		return _operand;
	}

	// Unwrap ARC4Decode
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		target = decodeExpr->value;

	// ARC4Struct field deletion — copy-on-write with zeroed field
	if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(target.get()))
	{
		auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
		if (arc4StructType)
		{
			auto base = fieldExpr->base;
			std::string fieldName = fieldExpr->name;

			auto readBase = base;
			if (dynamic_cast<awst::BoxValueExpression const*>(base.get()))
				readBase = builder::StorageMapper::makeStateGetWithDefault(base, base->wtype, m_loc);

			awst::WType const* arc4FieldType = nullptr;
			for (auto const& [fname, ftype]: arc4StructType->fields())
				if (fname == fieldName) { arc4FieldType = ftype; break; }

			auto zeroVal = builder::StorageMapper::makeDefaultValue(
				arc4FieldType ? arc4FieldType : fieldExpr->wtype, m_loc);

			auto newStruct = awst::makeNewStruct(arc4StructType, m_loc);
			for (auto const& [fname, ftype]: arc4StructType->fields())
			{
				if (fname == fieldName)
					newStruct->values[fname] = std::move(zeroVal);
				else
				{
					auto field = awst::makeFieldExpression(readBase, fname, ftype, m_loc);
					newStruct->values[fname] = std::move(field);
				}
			}

			auto writeTarget = base;
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
				writeTarget = sg->field;

			m_ctx.queuePending(awst::makeAssignmentStatement(std::move(writeTarget), std::move(newStruct), m_loc));
			return _operand;
		}
	}

	// Default: assign zero value
	auto defaultVal = builder::StorageMapper::makeDefaultValue(target->wtype, m_loc);
	if (auto const* sg = dynamic_cast<awst::StateGet const*>(target.get()))
		target = sg->field;

	// Slot-based storage delete: target is a computed biguint slot (e.g. delete _x[0] on multidim storage array)
	if (dynamic_cast<awst::BigUIntBinaryOperation const*>(target.get())
		&& target->wtype == awst::WType::biguintType())
	{
		// Determine number of slots to clear from the Solidity type
		auto const* subExprType = m_unaryOp.subExpression().annotation().type;
		auto const* arrType = subExprType ? dynamic_cast<ArrayType const*>(subExprType) : nullptr;
		unsigned slotCount = 1;
		if (arrType && !arrType->isDynamicallySized())
			slotCount = static_cast<unsigned>(arrType->length());

		for (unsigned j = 0; j < slotCount; ++j)
		{
			auto jConst = awst::makeIntegerConstant(j, m_loc, awst::WType::biguintType());

			auto slotJ = awst::makeBigUIntBinOp(target, awst::BigUIntBinaryOperator::Add, std::move(jConst), m_loc);

			auto btoi = builder::StorageMapper::biguintSlotToBtoi(slotJ, m_loc);

			auto zeroVal = awst::makeZero(m_loc, awst::WType::biguintType());

			auto call = awst::makeSubroutineCall(awst::SubroutineID{"__puyasol___storage_write"}, awst::WType::voidType(), m_loc);
			awst::pushCallArg(call->args, "__slot", std::move(btoi));
			awst::pushCallArg(call->args, "__value", std::move(zeroVal));

			m_ctx.queueStmt(std::move(call), m_loc);
		}
		return _operand;
	}

	m_ctx.queuePending(awst::makeAssignmentStatement(target, std::move(defaultVal), m_loc));
	return _operand;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::toAwst()
{
	auto operand = buildExpr(m_unaryOp.subExpression());

	// Try sol-eb builder dispatch for Not/Sub/BitNot
	{
		eb::BuilderUnaryOp builderOp;
		bool hasUnaryOp = true;
		switch (m_unaryOp.getOperator())
		{
		case Token::Not: builderOp = eb::BuilderUnaryOp::LogicalNot; break;
		case Token::Sub: builderOp = eb::BuilderUnaryOp::Negative; break;
		case Token::BitNot: builderOp = eb::BuilderUnaryOp::BitInvert; break;
		default: hasUnaryOp = false; break;
		}
		if (hasUnaryOp)
		{
			auto* solType = m_unaryOp.subExpression().annotation().type;
			auto builder = m_ctx.builderForInstance(solType, operand);
			if (builder)
			{
				auto result = builder->unary_op(builderOp, m_loc);
				if (result)
					return result->resolve();
			}
		}
	}

	switch (m_unaryOp.getOperator())
	{
	case Token::Not:    return handleNot(std::move(operand));
	case Token::Sub:    return handleNegate(std::move(operand));
	case Token::BitNot: return handleBitNot(std::move(operand));
	case Token::Inc:
	case Token::Dec:    return handleIncDec(std::move(operand));
	case Token::Delete: return handleDelete(std::move(operand));
	default:            return operand;
	}
}

} // namespace puyasol::builder::sol_ast
