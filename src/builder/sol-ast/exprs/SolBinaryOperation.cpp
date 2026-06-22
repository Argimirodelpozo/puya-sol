/// @file SolBinaryOperation.cpp — migrated from BinaryOperationBuilder.cpp.

#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-ast/exprs/SolBinaryOperation.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/sol-eb/BigUIntMathHelpers.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>
#include <libsolutil/Numeric.h>

#include <sstream>

namespace puyasol::builder::sol_ast
{

namespace
{
// Promote a signed-arith operand to biguint. `itob` is uint64-only; account
// and bytes types are already big-endian buffers, so reinterpret through bytes→biguint.
// Same fix as AssemblyBuilder::ensureBiguint (01332f363), but that helper carries
// strict-assembly aggregate-pointer semantics and must stay separate.
// Shared by buildSignedArithmetic and buildSignedDivMod.
std::shared_ptr<awst::Expression> promoteSignedOperandToBiguint(
	std::shared_ptr<awst::Expression> expr, awst::SourceLocation const& loc)
{
	if (expr->wtype == awst::WType::biguintType())
		return expr;
	if (expr->wtype == awst::WType::accountType()
		|| (expr->wtype && expr->wtype->kind() == awst::WTypeKind::Bytes))
	{
		auto bytesExpr = expr->wtype == awst::WType::accountType()
			? awst::makeAsBytes(std::move(expr), loc)
			: std::move(expr);
		return awst::makeAsBiguint(std::move(bytesExpr), loc);
	}
	auto itob = awst::makeItob(std::move(expr), loc);
	return awst::makeAsBiguint(std::move(itob), loc);
}
} // anonymous namespace


using namespace solidity::frontend;
using Token = solidity::frontend::Token;

SolBinaryOperation::SolBinaryOperation(
	eb::ContractContext& _ctx,
	BinaryOperation const& _node)
	: SolExpression(_ctx, _node), m_binOp(_node)
{
}

std::shared_ptr<awst::Expression> SolBinaryOperation::tryUserDefinedOp()
{
	auto const* userFunc = *m_binOp.annotation().userDefinedFunction;
	if (!userFunc) return nullptr;

	std::string subroutineId;
	auto it = m_ctx.freeFunctionById.find(userFunc->id());
	if (it != m_ctx.freeFunctionById.end())
		subroutineId = it->second;
	else
	{
		auto const* libContract = userFunc->annotation().contract;
		if (libContract && libContract->isLibrary())
		{
			std::string qualifiedName = libContract->name() + "." + userFunc->name();
			auto libIt = m_ctx.libraryFunctionIds.find(qualifiedName);
			if (libIt != m_ctx.libraryFunctionIds.end())
				subroutineId = libIt->second;
		}
		if (subroutineId.empty())
			subroutineId = m_ctx.sourceFile + "." + userFunc->name();
	}

	auto left = buildExpr(m_binOp.leftExpression());
	auto right = buildExpr(m_binOp.rightExpression());
	auto* resultType = m_ctx.typeMapper.map(m_binOp.annotation().type);

	auto call = awst::makeSubroutineCall(awst::SubroutineID{subroutineId}, resultType, m_loc);
	awst::pushCallArg(call->args, userFunc->parameters()[0]->name(), std::move(left));
	awst::pushCallArg(call->args, userFunc->parameters()[1]->name(), std::move(right));
	return call;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::tryConstantFold()
{
	if (auto const* ratType = dynamic_cast<RationalNumberType const*>(
			m_binOp.annotation().type))
	{
		if (!ratType->isFractional())
			// Solc folded the whole op to a non-fractional rational; emit its value via
			// the shared helper (promotes uint64->biguint when the magnitude overflows).
			return builder::TypeCoercion::rationalIntConstant(
				ratType->literalValue(nullptr),
				m_ctx.typeMapper.map(m_binOp.annotation().type), m_loc);
	}
	return nullptr;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::trySolEbDispatch(
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right)
{
	auto solOp = m_binOp.getOperator();
	auto* leftSolType = m_binOp.leftExpression().annotation().type;
	auto* rightSolType = m_binOp.rightExpression().annotation().type;

	// Use common type for arithmetic overflow checks (e.g. uint8+uint16 → uint16).
	auto const* commonSolType = m_binOp.annotation().commonType;

	auto leftBuilder = m_ctx.builderForInstance(leftSolType, _left);
	if (!leftBuilder && commonSolType)
		leftBuilder = m_ctx.builderForInstance(commonSolType, _left);
	if (!leftBuilder) return nullptr;

	auto rightBuilder = m_ctx.builderForInstance(rightSolType, _right);
	if (!rightBuilder && commonSolType)
		rightBuilder = m_ctx.builderForInstance(commonSolType, _right);
	if (!rightBuilder && leftSolType)
		rightBuilder = m_ctx.builderForInstance(leftSolType, _right);
	if (!rightBuilder) return nullptr;

	// Try comparison operators
	eb::BuilderComparisonOp cmpOp;
	bool hasCmpOp = true;
	switch (solOp)
	{
	case Token::Equal:              cmpOp = eb::BuilderComparisonOp::Eq; break;
	case Token::NotEqual:           cmpOp = eb::BuilderComparisonOp::Ne; break;
	case Token::LessThan:           cmpOp = eb::BuilderComparisonOp::Lt; break;
	case Token::LessThanOrEqual:    cmpOp = eb::BuilderComparisonOp::Lte; break;
	case Token::GreaterThan:        cmpOp = eb::BuilderComparisonOp::Gt; break;
	case Token::GreaterThanOrEqual: cmpOp = eb::BuilderComparisonOp::Gte; break;
	default: hasCmpOp = false; break;
	}
	if (hasCmpOp)
	{
		// Drive operand conversion off solc's commonType: coerce both integer
		// operands to the comparison's common type (canonicalising), so compare()
		// gets uniform same-width canonical operands — one solc-driven point that
		// replaces the per-operand sign-extension / narrowConstIfNegative inside
		// compare(). Non-integer comparisons (address/bytes) keep their builders.
		auto* cmpL = leftBuilder.get();
		auto* cmpR = rightBuilder.get();
		std::unique_ptr<eb::InstanceBuilder> clHold, crHold;
		if (commonSolType && dynamic_cast<IntegerType const*>(commonSolType))
		{
			auto* commonW = m_ctx.typeMapper.map(commonSolType);
			auto lv = builder::TypeCoercion::coerceToCommonInt(_left, leftSolType, commonW, m_loc);
			auto rv = builder::TypeCoercion::coerceToCommonInt(_right, rightSolType, commonW, m_loc);
			clHold = m_ctx.builderForInstance(commonSolType, lv);
			crHold = m_ctx.builderForInstance(commonSolType, rv);
			if (clHold && crHold) { cmpL = clHold.get(); cmpR = crHold.get(); }
		}
		auto result = cmpL->compare(*cmpR, cmpOp, m_loc);
		if (result) return result->resolve();
	}

	// Try arithmetic/bitwise operators
	eb::BuilderBinaryOp builderOp;
	bool hasBinOp = true;
	switch (solOp)
	{
	case Token::Add: case Token::AssignAdd: builderOp = eb::BuilderBinaryOp::Add; break;
	case Token::Sub: case Token::AssignSub: builderOp = eb::BuilderBinaryOp::Sub; break;
	case Token::Mul: case Token::AssignMul: builderOp = eb::BuilderBinaryOp::Mult; break;
	case Token::Div: case Token::AssignDiv: builderOp = eb::BuilderBinaryOp::FloorDiv; break;
	case Token::Mod: case Token::AssignMod: builderOp = eb::BuilderBinaryOp::Mod; break;
	case Token::Exp: builderOp = eb::BuilderBinaryOp::Pow; break;
	case Token::SHL: case Token::AssignShl: builderOp = eb::BuilderBinaryOp::LShift; break;
	case Token::SHR: case Token::SAR: case Token::AssignShr: case Token::AssignSar:
		builderOp = eb::BuilderBinaryOp::RShift; break;
	case Token::BitOr: case Token::AssignBitOr: builderOp = eb::BuilderBinaryOp::BitOr; break;
	case Token::BitXor: case Token::AssignBitXor: builderOp = eb::BuilderBinaryOp::BitXor; break;
	case Token::BitAnd: case Token::AssignBitAnd: builderOp = eb::BuilderBinaryOp::BitAnd; break;
	default: hasBinOp = false; break;
	}
	if (hasBinOp)
	{
		// Use common-type builder for arithmetic overflow width (e.g. uint8+uint16 → uint16).
		auto* arithBuilder = leftBuilder.get();
		std::unique_ptr<eb::InstanceBuilder> commonBuilder;
		if (commonSolType && commonSolType != leftSolType)
		{
			commonBuilder = m_ctx.builderForInstance(commonSolType, _left);
			if (commonBuilder)
				arithBuilder = commonBuilder.get();
		}
		auto result = arithBuilder->binary_op(*rightBuilder, builderOp, m_loc);
		if (result) return result->resolve();
	}

	return nullptr;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::toAwst()
{
	// 1. User-defined operator overloading
	if (auto result = tryUserDefinedOp())
		return result;

	// 2. Constant folding
	if (auto result = tryConstantFold())
		return result;

	// 3. Build operands
	auto left = buildExpr(m_binOp.leftExpression());
	auto right = buildExpr(m_binOp.rightExpression());
	auto* resultType = m_ctx.typeMapper.map(m_binOp.annotation().type);

	// 4. Signed integer arithmetic (mod 2^N + overflow); must precede sol-eb dispatch.
	auto const* commonType = m_binOp.annotation().commonType;
	if (auto const* intType = dynamic_cast<IntegerType const*>(commonType))
	{
		if (intType->isSigned())
		{
			auto op = m_binOp.getOperator();
			// Signed handlers reference each operand multiple times (sign/overflow/range).
			// makeEvalOnce prevents repeat execution (verified: `a()+b()` ran ~4× each);
			// pure leaves pass through, keeping `x+y`/`x+1` codegen byte-identical.
			left = awst::makeEvalOnce(std::move(left), m_loc);
			right = awst::makeEvalOnce(std::move(right), m_loc);
			if (op == Token::Add || op == Token::AssignAdd
				|| op == Token::Sub || op == Token::AssignSub
				|| op == Token::Mul || op == Token::AssignMul)
			{
				return buildSignedArithmetic(op, std::move(left), std::move(right), intType);
			}
			if (op == Token::Div || op == Token::AssignDiv
				|| op == Token::Mod || op == Token::AssignMod)
			{
				// buildSignedDivMod masks both operands to N (commonType) bits and reads
				// sign from `>= 2^(N-1)`, so each must be canonical at the common width. A
				// narrower signed divisor (int16 -32768) arrives sign-extended only in its
				// own 64-bit slot (2^64-32768) and masks to a huge POSITIVE N-bit value ->
				// wrong abs (int128/int16 div/mod returned 0 / the dividend). Sign-extend
				// each from its own width to canonical commonType first.
				auto* commonW = m_ctx.typeMapper.map(commonType);
				left = builder::TypeCoercion::coerceToCommonInt(
					std::move(left), m_binOp.leftExpression().annotation().type, commonW, m_loc);
				right = builder::TypeCoercion::coerceToCommonInt(
					std::move(right), m_binOp.rightExpression().annotation().type, commonW, m_loc);
				return buildSignedDivMod(op, std::move(left), std::move(right), intType);
			}
			if (op == Token::Exp)
			{
				return buildSignedExp(std::move(left), std::move(right), intType);
			}
		}
	}

	// 5. Sol-eb builder dispatch
	if (auto result = trySolEbDispatch(left, right))
		return result;

	// 6. Fallback to buildBinaryOp
	auto built = m_ctx.buildBinaryOp(
		m_binOp.getOperator(), std::move(left), std::move(right), resultType, m_loc);

	// 7. bytesN shift truncation: `bytesN << k` goes through biguint ×2^k
	// (sol-eb/BinaryOpBuilder.cpp), but Solidity's bytesN semantics are
	// left-aligned in a 32-byte word — `bytes6(0x616263646566) << 24` must yield
	// `0x646566000000`, not the 9-byte biguint. Cast to bytes and take last N bytes.
	auto op = m_binOp.getOperator();
	bool isShift = (op == Token::SHL || op == Token::AssignShl
		|| op == Token::SHR || op == Token::AssignShr
		|| op == Token::SAR || op == Token::AssignSar);
	if (isShift && built)
	{
		if (auto const* fbType = dynamic_cast<FixedBytesType const*>(m_binOp.annotation().type))
		{
			unsigned n = fbType->numBytes();
			auto bytesT = awst::WType::bytesType();

			auto asBytes = awst::makeReinterpretCast(std::move(built), bytesT, m_loc);

			// Pad left to ≥N bytes, then extract last N (concat(bzero(N),b) ensures len≥N).
			auto padded = awst::makeLeftPad(asBytes, n, m_loc);
			static int shCounter = 0;
			std::string varName = "__bytes_shift_" + std::to_string(shCounter++);
			auto var = awst::makeVarExpression(varName, bytesT, m_loc);
			m_ctx.prePendingStatements.push_back(
				awst::makeAssignmentStatement(var, std::move(padded), m_loc));

			auto lenCall = awst::makeLen(var, m_loc);

			auto nConst = awst::makeIntegerConstant(n, m_loc);
			auto start = awst::makeUInt64BinOp(
				std::move(lenCall), awst::UInt64BinaryOperator::Sub, std::move(nConst), m_loc);

			auto extr = awst::makeExtract3(
				var, std::move(start), awst::makeIntegerConstant(n, m_loc),
				m_loc, bytesT);

			return awst::makeReinterpretCast(std::move(extr), resultType, m_loc); // retype to bytes[N]
		}
	}

	return built;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::buildSignedArithmetic(
	Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	IntegerType const* _intType)
{
	// Delegate to the shared sol-eb signed-arithmetic helper (also used by SolIntegerBuilder for the
	// compound `x op= d` path, which previously mis-lowered signed assignment via the unsigned ops).
	auto op = (_op == Token::Sub || _op == Token::AssignSub) ? eb::BuilderBinaryOp::Sub
		: (_op == Token::Mul || _op == Token::AssignMul) ? eb::BuilderBinaryOp::Mult
		: eb::BuilderBinaryOp::Add;
	return eb::buildSignedArithmetic(m_ctx, m_scope.isUnchecked(), op,
		std::move(_left), std::move(_right), _intType->numBits(), m_loc);
}

std::shared_ptr<awst::Expression> SolBinaryOperation::buildSignedExp(
	std::shared_ptr<awst::Expression> _base,
	std::shared_ptr<awst::Expression> _exp,
	IntegerType const* _intType)
{
	unsigned bits = _intType->numBits();

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

	// Ensure base is biguint
	if (_base->wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeItob(std::move(_base), m_loc);
		_base = awst::makeAsBiguint(std::move(itob), m_loc);
	}

	// Mask base to N bits
	if (bits < 256)
	{
		auto mask = awst::makeBigUIntBinOp(std::move(_base), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);
		_base = std::move(mask);
	}

	// isNeg: base >= half
	auto baseNegCmp = awst::makeNumericCompare(_base, awst::NumericComparison::Lt, makeBiguintConst(halfNStr), m_loc);
	auto baseNeg = awst::makeNot(std::move(baseNegCmp), m_loc);

	// abs(base) = baseNeg ? (pow2N - base) : base
	auto negBase = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, _base, m_loc);
	auto absBase = awst::makeConditional(
		baseNeg, std::move(negBase), _base, awst::WType::biguintType(), m_loc);

	// Ensure exp is biguint
	if (_exp->wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeItob(std::move(_exp), m_loc);
		_exp = awst::makeAsBiguint(std::move(itob), m_loc);
	}

	// Compute abs(base) ^ exp using the standard buildBinaryOp (unsigned exp)
	auto* resultType = awst::WType::biguintType();
	auto absResult = m_ctx.buildBinaryOp(
		Token::Exp, std::move(absBase), _exp, resultType, m_loc);

	// Unchecked sub-word: wrap the (possibly overflowing) magnitude mod 2^bits BEFORE the
	// negation below — `pow2N - absResult` underflows the biguint subtraction when the exp
	// overflows the type (e.g. int8 (-128)**3 = 2097152 > 256) and the AVM `b-` panics. The
	// positive branch wraps too. Checked keeps the raw value: the overflow assert below needs
	// it to detect out-of-range (and a passing assert guarantees absResult < pow2N anyway).
	if (m_scope.isUnchecked() && bits < 256)
		absResult = awst::makeBigUIntBinOp(std::move(absResult),
			awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);

	// Check overflow: absResult must fit in signed range
	// absResult < half for positive result, absResult <= half for negative result
	if (!m_scope.isUnchecked())
	{
		// expIsOdd: exp % 2 != 0
		auto two = makeBiguintConst("2");
		auto expMod2 = awst::makeBigUIntBinOp(_exp, awst::BigUIntBinaryOperator::Mod, std::move(two), m_loc);
		auto expIsOdd = awst::makeNumericCompare(std::move(expMod2), awst::NumericComparison::Ne, makeBiguintConst("0"), m_loc);

		// resultNeg = baseNeg && expIsOdd
		auto resultNeg = awst::makeBoolBinOp(baseNeg, awst::BinaryBooleanOperator::And, std::move(expIsOdd), m_loc);

		// If resultNeg: absResult <= half, else: absResult < half
		auto ltHalf = awst::makeNumericCompare(absResult, awst::NumericComparison::Lt, makeBiguintConst(halfNStr), m_loc);

		auto halfLtRes = awst::makeNumericCompare(makeBiguintConst(halfNStr), awst::NumericComparison::Lt, absResult, m_loc);
		auto leHalf = awst::makeNot(std::move(halfLtRes), m_loc);

		auto rangeOk = awst::makeConditional(
			std::move(resultNeg), std::move(leHalf), std::move(ltHalf),
			awst::WType::boolType(), m_loc);

		auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(rangeOk), m_loc, "signed exp overflow"), m_loc);
		m_ctx.prePendingStatements.push_back(std::move(assertStmt));
	}

	// Negate result when base was negative and exp is odd: (pow2N - absResult) mod pow2N
	auto expMod2_2 = awst::makeBigUIntBinOp(_exp, awst::BigUIntBinaryOperator::Mod, makeBiguintConst("2"), m_loc);
	auto expOdd2 = awst::makeNumericCompare(std::move(expMod2_2), awst::NumericComparison::Ne, makeBiguintConst("0"), m_loc);
	auto shouldNeg = awst::makeBoolBinOp(baseNeg, awst::BinaryBooleanOperator::And, std::move(expOdd2), m_loc);

	auto negResult = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, absResult, m_loc);
	auto negMod = awst::makeBigUIntBinOp(std::move(negResult), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);

	// absResult == 0 → don't negate
	auto resZero = awst::makeNumericCompare(absResult, awst::NumericComparison::Eq, makeBiguintConst("0"), m_loc);
	auto notZero = awst::makeNot(std::move(resZero), m_loc);
	auto doNeg = awst::makeBoolBinOp(std::move(shouldNeg), awst::BinaryBooleanOperator::And, std::move(notZero), m_loc);

	auto result = awst::makeConditional(
		std::move(doNeg), std::move(negMod), std::move(absResult),
		awst::WType::biguintType(), m_loc);
	// A sub-word signed value's native WType is uint64 (canonical 64-bit two's-complement); narrow
	// the biguint exp result back so it composes as a SUB-expression with surrounding uint64 ops —
	// `b ^ (a**3)` for int8 else hands a biguint to a UInt64BinaryOperation (puya: "expected
	// uint64"). Whole-return coerces, a subexpr does not. >64-bit (int128/int256) stays biguint.
	// implicitNumericCast (NOT makeBiguintToUInt64): the exp result is sign-extended to 256 bits
	// (32 bytes), and a bare btoi reverts on >8 bytes — implicitNumericCast takes the low 8 bytes
	// (the 64-bit two's-complement), as the signed-shift narrow does.
	if (bits <= 64)
		return builder::TypeCoercion::implicitNumericCast(
			std::move(result), awst::WType::uint64Type(), m_loc);
	return result;
}

std::shared_ptr<awst::Expression> SolBinaryOperation::buildSignedDivMod(
	Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	IntegerType const* _intType)
{
	unsigned bits = _intType->numBits();
	bool isDiv = (_op == Token::Div || _op == Token::AssignDiv);

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

	_left = promoteSignedOperandToBiguint(std::move(_left), m_loc);
	_right = promoteSignedOperandToBiguint(std::move(_right), m_loc);

	// Mask to N bits
	if (bits < 256)
	{
		auto maskOp = [&](std::shared_ptr<awst::Expression> val) {
			auto mod = awst::makeBigUIntBinOp(std::move(val), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);
			return mod;
		};
		_left = maskOp(std::move(_left));
		_right = maskOp(std::move(_right));
	}

	// isNeg: val >= half
	auto isNeg = [&](std::shared_ptr<awst::Expression> const& val)
		-> std::shared_ptr<awst::Expression> {
		auto cmp = awst::makeNumericCompare(val, awst::NumericComparison::Lt, makeBiguintConst(halfNStr), m_loc);
		auto notExpr = awst::makeNot(std::move(cmp), m_loc);
		return notExpr;
	};

	// abs(val) = val < half ? val : (pow2N - val)
	auto absVal = [&](std::shared_ptr<awst::Expression> const& val) {
		auto neg = isNeg(val);
		auto negated = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, val, m_loc);
		return awst::makeConditional(
			std::move(neg), std::move(negated), val,
			awst::WType::biguintType(), m_loc);
	};

	// assert(y != 0)
	{
		auto bZero = awst::makeNumericCompare(_right, awst::NumericComparison::Ne, makeBiguintConst("0"), m_loc);

		auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(bZero), m_loc, "division by zero"), m_loc);
		m_ctx.prePendingStatements.push_back(std::move(assertStmt));
	}

	// assert NOT (x == INT_MIN && y == -1) — div overflow; minVal=half, -1=pow2N-1
	if (isDiv && !m_scope.isUnchecked())
	{
		std::ostringstream minusOneOss;
		if (bits == 256)
			minusOneOss << "115792089237316195423570985008687907853269984665640564039457584007913129639935";
		else
		{
			solidity::u256 minusOne = (solidity::u256(1) << bits) - 1;
			minusOneOss << minusOne;
		}

		auto xIsMin = awst::makeNumericCompare(_left, awst::NumericComparison::Eq, makeBiguintConst(halfNStr), m_loc);

		auto yIsNeg1 = awst::makeNumericCompare(_right, awst::NumericComparison::Eq, makeBiguintConst(minusOneOss.str()), m_loc);

		auto bothTrue = awst::makeBoolBinOp(std::move(xIsMin), awst::BinaryBooleanOperator::And, std::move(yIsNeg1), m_loc);

		auto notBoth = awst::makeNot(std::move(bothTrue), m_loc);

		auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(notBoth), m_loc, "signed division overflow"), m_loc);
		m_ctx.prePendingStatements.push_back(std::move(assertStmt));
	}

	auto absA = absVal(_left);
	auto absB = absVal(_right);

	// Unsigned div/mod on absolute values, then sign-correct.
	// div: negate when signs differ; mod: negate when a is negative.
	auto unsignedResult = awst::makeBigUIntBinOp(std::move(absA), isDiv ? awst::BigUIntBinaryOperator::FloorDiv
	                           : awst::BigUIntBinaryOperator::Mod, std::move(absB), m_loc);

	std::shared_ptr<awst::Expression> shouldNegate;
	if (isDiv)
	{
		auto differ = awst::makeNumericCompare(isNeg(_left), awst::NumericComparison::Ne, isNeg(_right), m_loc);
		shouldNegate = std::move(differ);
	}
	else
		shouldNegate = isNeg(_left);

	// (pow2N - result) mod pow2N, but don't negate 0
	auto negResult = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, unsignedResult, m_loc);
	auto negMod = awst::makeBigUIntBinOp(std::move(negResult), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);

	auto resultIsZero = awst::makeNumericCompare(unsignedResult, awst::NumericComparison::Eq, makeBiguintConst("0"), m_loc);
	auto needNeg = awst::makeBoolBinOp(std::move(shouldNegate), awst::BinaryBooleanOperator::And,
		awst::makeNot(std::move(resultIsZero), m_loc), m_loc);

	auto finalResult = awst::makeConditional(
		std::move(needNeg), std::move(negMod), std::move(unsignedResult),
		awst::WType::biguintType(), m_loc);

	if (bits <= 64)
		return awst::makeBiguintToUInt64(std::move(finalResult), m_loc);
	return finalResult;
}

} // namespace puyasol::builder::sol_ast
