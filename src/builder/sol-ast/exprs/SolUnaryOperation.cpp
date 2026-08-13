/// @file SolUnaryOperation.cpp — unary operation translation.

#include "Logger.h"
#include "builder/sol-types/SolcConstFold.h"
#include "awst/NameGen.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-ast/exprs/SolUnaryOperation.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/SlotHandleAccess.h"
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

namespace
{
// Promote a value to a signed 256-bit biguint: a uint64 value is widened via
// itob then reinterpreted as biguint; any other type is passed through. Shared
// by handleNegate and makeNewValue's signed branch (identical two-step).
std::shared_ptr<awst::Expression> promoteToSignedBiguint(
	std::shared_ptr<awst::Expression> value, awst::SourceLocation const& loc)
{
	if (value->wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeItob(std::move(value), loc);
		value = awst::makeAsBiguint(std::move(itob), loc);
	}
	return value;
}
} // namespace

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
	// For constant expressions like `-2`, operand type is RationalNumberType;
	// fall through to check result type for signed intN.
	auto intInfo = SolIntType::fromSol(m_unaryOp.subExpression().annotation().type);
	if (!intInfo || !intInfo->isSigned)
	{
		// constant `-2`: operand is RationalNumberType, not intN — use the result type.
		if (auto resultInfo = SolIntType::fromSol(m_unaryOp.annotation().type);
			resultInfo && resultInfo->isSigned)
			intInfo = resultInfo;
	}

	if (intInfo && intInfo->isSigned)
	{
		unsigned bits = intInfo->bits;
		// -x = (2^N - x) mod 2^N; overflow: x == 2^(N-1) i.e. INT_MIN
		auto [pow2NStr, halfNStr] = intInfo->pow2NAndHalf();

		auto makeBiguintConst = [&](std::string const& val) {
			auto c = awst::makeIntegerConstant(val, m_loc, awst::WType::biguintType());
			return c;
		};

		auto operand = promoteToSignedBiguint(std::move(_operand), m_loc);

		if (bits < 256)
		{
			auto mask = awst::makeBigUIntBinOp(std::move(operand), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);
			operand = std::move(mask);
		}

		if (!m_scope.isUnchecked())
		{
			auto cmp = awst::makeNumericCompare(operand, awst::NumericComparison::Ne, makeBiguintConst(halfNStr), m_loc);

			m_ctx.queuePreStmt(awst::makeAssert(std::move(cmp), m_loc, "signed negation overflow"), m_loc);
		}

		// -x = (2^N - x) mod 2^N
		std::shared_ptr<awst::Expression> negated;
		if (bits == 256)
		{
			std::string tmpName = "__neg_tmp_" + std::to_string(m_unaryOp.id());

			auto tmpVar = awst::makeVarExpression(tmpName, awst::WType::biguintType(), m_loc);

			auto initStmt = awst::makeAssignmentStatement(tmpVar, makeBiguintConst("0"), m_loc);
			m_ctx.prePendingStatements.push_back(std::move(initStmt));

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

		if (bits <= 64)
		{
			return awst::makeBiguintToUInt64(std::move(negated), m_loc);
		}
		return negated;
	}

	// biguint constant negation: two's complement 2^256 - value
	if (isBigUInt(_operand->wtype))
	{
		if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(_operand.get()))
		{
			solidity::u256 val(intConst->value);
			// Allow val == 2^255 (int256.min valid); strict < missed it, AVM b- crashes on 0-2^255.
			if (val > 0 && val <= (solidity::u256(1) << 255))
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

		// Non-constant unsigned negation: 0 - x
		auto zero = awst::makeZero(m_loc, awst::WType::biguintType());
		auto e = awst::makeBigUIntBinOp(std::move(zero), awst::BigUIntBinaryOperator::Sub, std::move(_operand), m_loc);
		return e;
	}
	// uint64 constant negation: produce biguint two's-comp. Can't narrow: if the
	// context is int256 storage, uint64(2^64-1) would sign-extend wrong.
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
		unsigned maskBits = 64;
		if (auto it = SolIntType::fromSol(m_unaryOp.subExpression().annotation().type))
			maskBits = it->bits;

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
	// biguint-backed intN (64<N<256): ~x = x XOR (2^N-1) masked to bit width.
	// Raw BitInvert inverts wrong count: ~uint128(5) must be 2^128-6, not ~0x05=0xFA
	// (V4 LPFee removeOverrideFlagAndValidate was silently a no-op).
	if (expr->wtype == awst::WType::biguintType())
	{
		unsigned maskBits = 0;
		if (auto it = SolIntType::fromSol(m_unaryOp.subExpression().annotation().type))
			maskBits = it->bits;
		if (maskBits > 0 && maskBits < 256)
		{
			solidity::u256 mask = (solidity::u256(1) << maskBits) - 1;
			std::ostringstream oss; oss << mask;
			auto maskConst = awst::makeIntegerConstant(
				oss.str(), m_loc, awst::WType::biguintType());
			return awst::makeBigUIntBinOp(
				std::move(expr), awst::BigUIntBinaryOperator::BitXor,
				std::move(maskConst), m_loc);
		}
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

	// Transient state var: read/compute/write via TransientStorage
	// (AssignmentStatement with read expr as target isn't an lvalue).
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

	// NB: raw cast, NOT SolIntType::fromSol — this path historically does not unwrap
	// UDVTs here; a UDVT operand is treated as non-int (isSigned=false). Hoisted once
	// and reused for signedBits/unsignedBits below.
	auto const* opInt = dynamic_cast<IntegerType const*>(
		m_unaryOp.subExpression().annotation().type);
	bool isSigned = opInt && opInt->isSigned();

	if (dynamic_cast<awst::BoxValueExpression const*>(_operand.get()))
		_operand = builder::StorageMapper::makeStateGetWithDefault(_operand, _operand->wtype, m_loc);
	else if (auto const* idx = dynamic_cast<awst::IndexExpression const*>(_operand.get()))
	{
		if (dynamic_cast<awst::BoxValueExpression const*>(idx->base.get()))
		{
			auto* nativeType = m_ctx.typeMapper.map(m_unaryOp.subExpression().annotation().type);
			if (_operand->wtype != nativeType)
				_operand = awst::makeARC4Decode(_operand, nativeType, m_loc);
		}
	}

	static const std::string pow256Minus1 =
		"115792089237316195423570985008687907853269984665640564039457584007913129639935";

	unsigned signedBits = (opInt && isSigned) ? opInt->numBits() : 0;
	unsigned unsignedBits = (opInt && !isSigned) ? opInt->numBits() : 0;

	// Checked unsigned `++` overflow guard. The signed path (below) and `x += 1` both check, but the
	// unsigned inc just computes base+1: native uint64 add reverts on its own, but a sub-word
	// (uint8..uint56) add yields e.g. 256 that later masks to 0, and a biguint (uint65..uint256) add
	// yields the exact 2^N with no auto-revert — both silently wrapped. Assert result <= 2^bits-1 via a
	// self-contained comma so it composes in both the prefix value and the postfix write. (Dec underflow
	// is already caught: uint64 `-` and biguint `b-` revert on a negative result.) Found by the differential fuzzer.
	auto guardUIncOverflow = [&](std::shared_ptr<awst::Expression> bin, unsigned bits)
		-> std::shared_ptr<awst::Expression>
	{
		auto* wt = bin->wtype;
		std::string tmpName = "__uinc_" + std::to_string(awst::NameGen::next("SolUnaryOperation.s_uincCounter"));
		solidity::u256 maxV = (bits >= 256) ? (solidity::u256(0) - 1)
		                                     : ((solidity::u256(1) << bits) - 1);
		std::ostringstream oss; oss << maxV;
		auto bind = awst::makeAssignmentExpression(
			awst::makeVarExpression(tmpName, wt, m_loc), std::move(bin), m_loc, wt);
		auto cmp = awst::makeNumericCompare(
			awst::makeVarExpression(tmpName, wt, m_loc), awst::NumericComparison::Lte,
			awst::makeIntegerConstant(oss.str(), m_loc, wt), m_loc);
		auto comma = awst::makeCommaExpression(wt, m_loc);
		comma->expressions.push_back(std::move(bind));
		comma->expressions.push_back(awst::makeAssert(std::move(cmp), m_loc, "overflow"));
		comma->expressions.push_back(awst::makeVarExpression(tmpName, wt, m_loc));
		return comma;
	};

	auto makeNewValue = [&](std::shared_ptr<awst::Expression> base)
		-> std::shared_ptr<awst::Expression>
	{
		if (isSigned && signedBits > 0)
		{
			auto [pow2NStr2, halfNStr2] = builder::TypeCoercion::pow2NAndHalf(signedBits);

			auto makeBConst = [&](std::string const& v) {
				auto c = awst::makeIntegerConstant(v, m_loc, awst::WType::biguintType());
				return c;
			};

			auto val = promoteToSignedBiguint(std::move(base), m_loc);

			if (signedBits < 256)
			{
				auto mask = awst::makeBigUIntBinOp(std::move(val), awst::BigUIntBinaryOperator::Mod, makeBConst(pow2NStr2), m_loc);
				val = std::move(mask);
			}

			// Overflow check: inc at MAX (half-1), dec at MIN (half)
			if (!m_scope.isUnchecked())
			{
				std::string limitStr;
				if (isInc)
				{
					solidity::u256 maxVal = (solidity::u256(1) << (signedBits - 1)) - 1;
					std::ostringstream oss; oss << maxVal; limitStr = oss.str();
				}
				else
					limitStr = halfNStr2; // MIN = half

				auto cmp = awst::makeNumericCompare(val, awst::NumericComparison::Ne, makeBConst(limitStr), m_loc);

				m_ctx.queuePreStmt(awst::makeAssert(std::move(cmp), m_loc, "signed inc/dec overflow"), m_loc);
			}

			std::shared_ptr<awst::Expression> added;
			if (isInc)
			{
				auto add = awst::makeBigUIntBinOp(std::move(val), awst::BigUIntBinaryOperator::Add, makeBConst("1"), m_loc);
				added = std::move(add);
			}
			else
			{
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

			if (signedBits <= 64)
			{
				return awst::makeBiguintToUInt64(std::move(mod), m_loc);
			}
			return mod;
		}
		else if (!isSigned && m_scope.isUnchecked() && unsignedBits > 0)
		{
			// Unsigned UNCHECKED inc/dec must WRAP mod 2^N (EVM), but the native uint64 +/- opcodes and
			// the biguint b- opcode REVERT at the boundary (uint64 max+1 overflows, 0-1 underflows).
			// Compute in biguint: inc = v+1; dec = v + (2^N-1) [add max, not subtract 1, to dodge
			// underflow]; then mod 2^N. Narrow back to uint64 for sub-word/uint64 backings.
			auto makeBConst = [&](std::string const& v) {
				return awst::makeIntegerConstant(v, m_loc, awst::WType::biguintType());
			};
			static const std::string pow2_256Str =
				"115792089237316195423570985008687907853269984665640564039457584007913129639936";
			bool nativeBack = !isBigUInt(base->wtype);
			auto val = promoteToSignedBiguint(std::move(base), m_loc);
			std::shared_ptr<awst::Expression> added;
			if (isInc)
				added = awst::makeBigUIntBinOp(std::move(val), awst::BigUIntBinaryOperator::Add, makeBConst("1"), m_loc);
			else
			{
				std::string maxStr = (unsignedBits >= 256)
					? pow256Minus1
					: ((solidity::u256(1) << unsignedBits) - 1).str();
				added = awst::makeBigUIntBinOp(std::move(val), awst::BigUIntBinaryOperator::Add, makeBConst(maxStr), m_loc);
			}
			std::string pow2NStr = (unsignedBits >= 256)
				? pow2_256Str
				: (solidity::u256(1) << unsignedBits).str();
			auto mod = awst::makeBigUIntBinOp(std::move(added), awst::BigUIntBinaryOperator::Mod, makeBConst(pow2NStr), m_loc);
			if (nativeBack)
				return awst::makeBiguintToUInt64(std::move(mod), m_loc);
			return mod;
		}
		else if (isBigUInt(base->wtype))
		{
			auto one = awst::makeOne(m_loc, awst::WType::biguintType());
			auto bin = awst::makeBigUIntBinOp(std::move(base), isInc ? awst::BigUIntBinaryOperator::Add : awst::BigUIntBinaryOperator::Sub, std::move(one), m_loc);
			// biguint (uint65..uint256) add never auto-reverts — guard checked inc at 2^N.
			if (isInc && !m_scope.isUnchecked() && unsignedBits > 0)
				return guardUIncOverflow(std::move(bin), unsignedBits);
			return bin;
		}
		else
		{
			auto one = awst::makeOne(m_loc);
			auto bin = awst::makeUInt64BinOp(std::move(base), isInc ? awst::UInt64BinaryOperator::Add : awst::UInt64BinaryOperator::Sub, std::move(one), m_loc);
			// Sub-word (uint8..uint56) add yields up to 2^bits that masks to 0 — guard checked inc.
			// uint64 (bits==64) is left to the native `+` opcode, which reverts on overflow.
			if (isInc && !m_scope.isUnchecked() && unsignedBits > 0 && unsignedBits < 64)
				return guardUIncOverflow(std::move(bin), unsignedBits);
			return bin;
		}
	};

	// Derive write target from the ALREADY-BUILT operand — rebuilding re-runs
	// side-effecting indexes (verified: `arr[i++]++` gave i==2 on rebuild).
	// Unwrap StateGet (read wrapper) so assignment lands on BoxValue/AppState;
	// peel ARC4Decode so write hits encoded slot (paired with ARC4Encode below).
	auto makeWriteTarget = [&](std::shared_ptr<awst::Expression> target)
		-> std::shared_ptr<awst::Expression>
	{
		target = awst::unwrapStateGet(std::move(target));
		if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
			target = decodeExpr->value;
		return target;
	};

	// ARC4-typed write target + native new value: wrap with ARC4Encode (pairs makeWriteTarget).
	auto maybeEncode = [&](std::shared_ptr<awst::Expression> writeTarget,
		std::shared_ptr<awst::Expression> newValue) -> std::shared_ptr<awst::Expression>
	{
		auto const* targetType = writeTarget->wtype;
		if (auto const* arc4 = dynamic_cast<awst::ARC4UIntN const*>(targetType))
			if (newValue->wtype != arc4)
				return awst::makeARC4Encode(std::move(newValue), arc4, m_loc);
		return newValue;
	};

	// Boxed STRUCT FIELD inc/dec: a bare `field := v` assignment is rejected by puya once the struct is
	// boxed (any 2nd function keeps the struct in a box). Rebuild the struct copy-on-write
	// (box := struct-with-field-replaced) like SolAssignment's compound path.
	auto structFieldTypeOf = [&](std::shared_ptr<awst::Expression> const& writeTarget)
		-> awst::ARC4Struct const*
	{
		auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(writeTarget.get());
		if (!fieldExpr) return nullptr;
		auto const* structType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
		if (!structType)
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(fieldExpr->base.get()))
				structType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
		return structType;
	};
	auto buildStructFieldCowWrite = [&](std::shared_ptr<awst::Expression> const& writeTarget,
		awst::ARC4Struct const* structType,
		std::shared_ptr<awst::Expression> nativeFieldValue) -> std::shared_ptr<awst::Statement>
	{
		auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(writeTarget.get());
		awst::WType const* arc4FieldType = nullptr;
		for (auto const& [fn, ft]: structType->fields())
			if (fn == fieldExpr->name) { arc4FieldType = ft; break; }
		std::shared_ptr<awst::Expression> encoded = std::move(nativeFieldValue);
		if (arc4FieldType && encoded->wtype != arc4FieldType)
			encoded = awst::makeARC4Encode(std::move(encoded), arc4FieldType, m_loc);
		auto fbase = awst::unwrapStateGet(fieldExpr->base);
		// Read the OTHER fields with-default so a fresh (non-existent) top-level box yields defaults
		// instead of reverting (matches compound). rebuildArc4StructChainCOW only wraps the read base for
		// NESTED structs; for a top-level struct (fbase is a bare BoxValue) we must wrap it here. The
		// write target stays the bare box (unwrapped).
		auto readBase = fbase;
		if (dynamic_cast<awst::BoxValueExpression const*>(fbase.get()))
			readBase = builder::StorageMapper::makeStateGetWithDefault(fbase, fbase->wtype, m_loc);
		auto newStruct = awst::makeStructWithReplacedField(structType, readBase, fieldExpr->name, std::move(encoded), m_loc);
		auto cow = eb::AssignmentHelper::rebuildArc4StructChainCOW(m_ctx, fbase, std::move(newStruct), m_loc);
		auto target = awst::makeWritableTarget(std::move(cow.assignTarget));
		return awst::makeAssignmentStatement(std::move(target), std::move(cow.assignValue), m_loc);
	};

	if (isPrefix)
	{
		auto writeTarget = makeWriteTarget(_operand);
		if (auto const* structType = structFieldTypeOf(writeTarget))
		{
			std::string pName = "__sfpre_" + std::to_string(awst::NameGen::next("SolUnaryOperation.sfPreCounter"));
			auto nv = makeNewValue(_operand);
			auto* nvType = nv->wtype;
			auto nvVar = awst::makeVarExpression(pName, nvType, m_loc);
			m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(nvVar, std::move(nv), m_loc));
			m_ctx.prePendingStatements.push_back(buildStructFieldCowWrite(writeTarget, structType,
				awst::makeVarExpression(pName, nvType, m_loc)));
			return awst::makeVarExpression(pName, nvType, m_loc);
		}
		auto newValue = maybeEncode(writeTarget, makeNewValue(_operand));
		return awst::makeAssignmentExpression(
			std::move(writeTarget), std::move(newValue), m_loc, _operand->wtype);
	}
	else
	{
		// Post-inc: save old value, emit write as prePending (not pending) so
		// `a++ + a` reads updated `a`; pending only fires at statement end.
		std::string tempName = "__postinc_" + std::to_string(awst::NameGen::next("SolUnaryOperation.postIncCounter"));

		auto tempVar = awst::makeVarExpression(tempName, _operand->wtype, m_loc);

		auto saveStmt = awst::makeAssignmentStatement(tempVar, _operand, m_loc);
		m_ctx.prePendingStatements.push_back(std::move(saveStmt));

		auto writeTarget = makeWriteTarget(_operand);
		if (auto const* structType = structFieldTypeOf(writeTarget))
		{
			m_ctx.prePendingStatements.push_back(buildStructFieldCowWrite(writeTarget, structType,
				makeNewValue(tempVar)));
			return tempVar;
		}
		auto newValue = maybeEncode(writeTarget, makeNewValue(tempVar));

		auto incrStmt = awst::makeAssignmentStatement(std::move(writeTarget), std::move(newValue), m_loc);
		m_ctx.prePendingStatements.push_back(std::move(incrStmt));

		return tempVar;
	}
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleDelete(
	std::shared_ptr<awst::Expression> _operand)
{
	// Transient state var delete: write zero via TransientStorage (read expr not an lvalue).
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

	// Use already-built operand: rebuilding re-runs side effects (verified: `delete m[f()]` ran f() twice).
	auto target = _operand;

	// Clear function pointer tracking on delete.
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_unaryOp.subExpression()))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
			m_scope.eraseFuncPtrTarget(varDecl->id());
	}

	if (auto const* boxExpr = dynamic_cast<awst::BoxValueExpression const*>(target.get()))
	{
		// Top-level dynamic box (eager box_create in __postInit): emit `a = default`
		// not box_del — subsequent reads assert box exists. Mappings use box_del (lazy).
		if (builder::StorageMapper::isTopLevelDynamicBox(boxExpr))
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

	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		target = decodeExpr->value;

	// ARC4Struct field: COW with zeroed field.
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

			auto newStruct = awst::makeStructWithReplacedField(
				arc4StructType, readBase, fieldName, std::move(zeroVal), m_loc);

			auto writeTarget = base;
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
				writeTarget = sg->field;

			m_ctx.queuePending(awst::makeAssignmentStatement(std::move(writeTarget), std::move(newStruct), m_loc));
			return _operand;
		}
	}

	auto defaultVal = builder::StorageMapper::makeDefaultValue(target->wtype, m_loc);
	target = awst::unwrapStateGet(std::move(target));

	// Slot-based storage delete: clear biguint-slot range.
	if (dynamic_cast<awst::BigUIntBinaryOperation const*>(target.get())
		&& target->wtype == awst::WType::biguintType())
	{
		auto const* subExprType = m_unaryOp.subExpression().annotation().type;
		auto const* arrType = subExprType ? dynamic_cast<ArrayType const*>(subExprType) : nullptr;
		unsigned slotCount = 1;
		if (arrType && !arrType->isDynamicallySized())
		{
			// EVM clears the array's SLOT footprint — packed arrays span fewer
			// slots than elements, multislot-element arrays span more.
			auto slots = arrType->storageSize();
			if (slots > 4096)
			{
				Logger::instance().error(
					"slot-handle delete of " + slots.str()
					+ " slots exceeds the unroll cap (4096)", m_loc);
				slots = 1;
			}
			slotCount = static_cast<unsigned>(slots);
		}

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

std::shared_ptr<awst::Expression> SolUnaryOperation::handleEvmStorageIncDecDelete()
{
	auto const& sub = m_unaryOp.subExpression();
	auto const* solType = sub.annotation().type;
	// delete on a fixed array / struct: zero the whole slot footprint.
	if (m_unaryOp.getOperator() == Token::Delete && solType)
	{
		auto const* dat = dynamic_cast<ArrayType const*>(solType);
		bool fixedArr = dat && !dat->isDynamicallySized()
			&& !dat->isByteArrayOrString();
		bool isStruct = dynamic_cast<StructType const*>(solType) != nullptr;
		if (fixedArr || isStruct)
		{
			auto slots = solType->storageSize();
			if (slots > 256)
			{
				Logger::instance().error(
					"--evm-storage-layout: delete of " + slots.str()
					+ " slots exceeds the unroll cap (256)", m_loc);
				return nullptr;
			}
			EvmSlotLowering low(m_ctx, m_scope, m_loc);
			auto addr = low.resolve(sub);
			if (!addr)
				return nullptr;
			unsigned n = static_cast<unsigned>(slots);
			// The zero-writes are separate statements — pin the base slot to a
			// NAMED temp (cross-statement idiom; SingleEvaluation is unsound).
			std::string nm = "__evm_del_"
				+ std::to_string(awst::NameGen::next("SolUnaryOperation.evmDel"));
			m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(nm, awst::WType::biguintType(), m_loc),
				addr->slot, m_loc));
			std::vector<std::shared_ptr<awst::Statement>> writes;
			for (unsigned j = 0; j < n; ++j)
			{
				auto slotJ = j == 0
					? std::shared_ptr<awst::Expression>(
						awst::makeVarExpression(nm, awst::WType::biguintType(), m_loc))
					: awst::makeBigUIntBinOp(
						awst::makeVarExpression(nm, awst::WType::biguintType(), m_loc),
						awst::BigUIntBinaryOperator::Add,
						awst::makeIntegerConstant(j, m_loc,
							awst::WType::biguintType()), m_loc);
				writes.push_back(builder::SlotHandleAccess::writeSlot(
					std::move(slotJ),
					awst::makeIntegerConstant("0", m_loc,
						awst::WType::biguintType()), m_loc));
			}
			for (auto& st: writes)
				m_ctx.queuePending(std::move(st));
			return awst::makeZero(m_loc, awst::WType::biguintType());
		}
	}
	if (m_unaryOp.getOperator() == Token::Delete
		&& EvmSlotLowering::isBytesLike(solType))
	{
		EvmSlotLowering low(m_ctx, m_scope, m_loc);
		auto addr = low.resolve(sub);
		if (!addr)
			return nullptr;
		std::vector<std::shared_ptr<awst::Statement>> writes;
		low.writeBytesValue(*addr,
			awst::makeBytesConstant({}, m_loc), writes);
		for (auto& st: writes)
			m_ctx.queuePending(std::move(st));
		return awst::makeZero(m_loc, awst::WType::biguintType());
	}
	if (m_unaryOp.getOperator() == Token::Delete
		&& solType && !solType->isValueType())
	{
		EvmSlotLowering low(m_ctx, m_scope, m_loc);
		auto addr = low.resolve(sub);
		if (!addr)
			return nullptr;
		addr->solType = solType;
		addr->wtype = m_ctx.typeMapper.map(solType);
		std::vector<std::shared_ptr<awst::Statement>> writes;
		if (low.clearAggregate(*addr, solType, writes))
		{
			for (auto& st: writes)
				m_ctx.queuePending(std::move(st));
			return awst::makeZero(m_loc, awst::WType::biguintType());
		}
		return nullptr;
	}
	if (!solType || !solType->isValueType())
	{
		Logger::instance().error(
			"--evm-storage-layout: delete/++/-- on aggregate storage not yet "
			"supported", m_loc);
		return nullptr;
	}
	EvmSlotLowering low(m_ctx, m_scope, m_loc);
	auto addr = low.resolve(sub);
	if (!addr)
		return nullptr;

	// The slot (and any runtime byte offset) is referenced by the value read
	// AND the queued write — separate statements, so pin to temps (the
	// cross-statement idiom; SingleEvaluation would be unsound here).
	auto pinToTemp = [&](std::shared_ptr<awst::Expression>& _e, char const* _tag) {
		if (!_e || dynamic_cast<awst::VarExpression const*>(_e.get())
			|| dynamic_cast<awst::IntegerConstant const*>(_e.get()))
			return;
		std::string nm = std::string("__evm_") + _tag + "_"
			+ std::to_string(awst::NameGen::next("SolUnaryOperation.evmPin"));
		m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(nm, _e->wtype, m_loc), _e, m_loc));
		_e = awst::makeVarExpression(nm, _e->wtype, m_loc);
	};
	pinToTemp(addr->slot, "slot");
	pinToTemp(addr->byteOffset, "off");

	Token op = m_unaryOp.getOperator();
	std::vector<std::shared_ptr<awst::Statement>> writes;
	if (op == Token::Delete)
	{
		std::shared_ptr<awst::Expression> zero;
		if (addr->wtype == awst::WType::accountType())
			zero = awst::makeAddressConstant(
				"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ", m_loc);
		else if (addr->wtype == awst::WType::biguintType())
			zero = awst::makeZero(m_loc, awst::WType::biguintType());
		else if (addr->wtype == awst::WType::boolType())
			zero = awst::makeBoolConstant(false, m_loc, awst::WType::boolType());
		else if (addr->wtype == awst::WType::uint64Type())
			zero = awst::makeZero(m_loc);
		else if (auto const* bw = dynamic_cast<awst::BytesWType const*>(addr->wtype);
			bw && bw->length().has_value())
			zero = awst::makeBytesConstant(
				std::vector<uint8_t>(static_cast<size_t>(*bw->length()), 0), m_loc,
				awst::BytesEncoding::Base16, addr->wtype);
		else
			zero = builder::StorageMapper::makeDefaultValue(addr->wtype, m_loc);
		low.writeValue(*addr, std::move(zero), writes);
		for (auto& st: writes)
			m_ctx.queuePending(std::move(st));
		return awst::makeZero(m_loc, awst::WType::biguintType());
	}

	bool isInc = (op == Token::Inc);
	bool isPrefix = m_unaryOp.isPrefixOperation();
	auto current = low.readValue(*addr);
	// Postfix returns the OLD value: pin it BEFORE the write. Returning a
	// fresh read with the write queued post-statement looked equivalent, but
	// `return st.a++` hoists pending statements ahead of the return, so the
	// "old" read observed the incremented value.
	std::shared_ptr<awst::Expression> oldPin;
	if (!isPrefix)
	{
		std::string onm = "__evm_old_"
			+ std::to_string(awst::NameGen::next("SolUnaryOperation.evmOld"));
		m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(onm, current->wtype, m_loc), current, m_loc));
		oldPin = awst::makeVarExpression(onm, current->wtype, m_loc);
		current = awst::makeVarExpression(onm, current->wtype, m_loc);
	}
	auto one = awst::makeIntegerConstant("1", m_loc,
		addr->wtype == awst::WType::biguintType()
			? awst::WType::biguintType() : awst::WType::uint64Type());
	auto newValue = eb::AssignmentHelper::tryComputeCompoundValue(
		m_ctx, isInc ? Token::AssignAdd : Token::AssignSub,
		solType, current, one, m_loc);
	if (!newValue)
		newValue = m_ctx.buildBinaryOp(
			isInc ? Token::Add : Token::Sub, current, std::move(one),
			current->wtype, m_loc);
	if (newValue && addr->wtype && newValue->wtype != addr->wtype
		&& (newValue->wtype == awst::WType::uint64Type()
			|| newValue->wtype == awst::WType::biguintType())
		&& (addr->wtype == awst::WType::uint64Type()
			|| addr->wtype == awst::WType::biguintType()))
		newValue = builder::TypeCoercion::implicitNumericCast(
			std::move(newValue), addr->wtype, m_loc);
	if (!newValue)
		return nullptr;
	low.writeValue(*addr, std::move(newValue), writes);
	// Both forms write via prePending (after the postfix old-pin above, so
	// ordering inside prePending is pin -> write). Prefix returns a fresh
	// read (sees the new value); postfix returns the pinned old value.
	for (auto& st: writes)
		m_ctx.prePendingStatements.push_back(std::move(st));
	if (isPrefix)
		return low.readValue(*addr);
	return oldPin;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::toAwst()
{
	// The canonical constant path (fable-review item 1): solc folded the WHOLE
	// expression (non-fractional rational annotation, e.g. `-2`, `~5`) → emit
	// its value directly; never fold built AWST downstream. Runtime-typed
	// expressions (incl. AWST-constant operands like a lowered type(intN).min)
	// take the full checked paths below.
	if (auto folded = builder::SolcConstFold::foldAnnotated(m_unaryOp, m_ctx.typeMapper, m_loc))
		return folded;
	// intN-typed constant expression (e.g. `-M` over a constant variable) —
	// foldTyped's in-range guard keeps `-intN.min` on the checked path.
	if (auto folded = builder::SolcConstFold::foldTyped(m_unaryOp, m_loc))
		return folded;

	// --evm-storage-layout: ++/--/delete on storage state refs must not build
	// the operand (the sub-expression is the lvalue, not a value).
	{
		Token op = m_unaryOp.getOperator();
		if ((op == Token::Inc || op == Token::Dec || op == Token::Delete)
			&& m_ctx.typeMapper.profile().evmStorageLayout
			&& EvmSlotLowering::isStorageStateRef(m_unaryOp.subExpression()))
			return handleEvmStorageIncDecDelete();
	}

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
			// Checked negate references the operand in the overflow assert AND
			// the negation (verified: -g() ran g 3×); biguint ~ references it
			// in len + extract. Pin once — the same wrapped node also feeds the
			// handleNot/handleNegate/handleBitNot fallbacks below. Inc/Dec/
			// Delete never enter this block (their operand must stay an
			// lvalue-shaped tree).
			operand = awst::makeEvalOnce(std::move(operand), m_loc);
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
