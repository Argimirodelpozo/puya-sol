/// @file SolBinaryOperation.cpp — migrated from BinaryOperationBuilder.cpp.

#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-ast/exprs/SolBinaryOperation.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
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
		{
			auto* resultType = m_ctx.typeMapper.map(m_binOp.annotation().type);
			auto val = ratType->literalValue(nullptr);
			// u256 two's complement; promote to biguint if > uint64 (for sign extension).
			static const solidity::u256 uint64Max("18446744073709551615");
			if (resultType == awst::WType::uint64Type() && val > uint64Max)
				resultType = awst::WType::biguintType();
			auto e = awst::makeIntegerConstant(val.str(), m_loc, resultType);
			return e;
		}
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
		auto result = leftBuilder->compare(*rightBuilder, cmpOp, m_loc);
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
	unsigned bits = _intType->numBits();
	bool isBiguint = (bits > 64);

	// Compute 2^N and 2^(N-1) as strings; u256 overflows for N=256, so special-case.
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
		std::ostringstream pow2NOss, halfNOss;
		pow2NOss << pow2N;
		halfNOss << halfN;
		pow2NStr = pow2NOss.str();
		halfNStr = halfNOss.str();
	}

	auto makeConst = [&](std::string const& val) -> std::shared_ptr<awst::Expression> {
		auto c = awst::makeIntegerConstant(val, m_loc, isBiguint ? awst::WType::biguintType() : awst::WType::uint64Type());
		return c;
	};

	auto makeBiguintConst = [&](std::string const& val) -> std::shared_ptr<awst::Expression> {
		auto c = awst::makeIntegerConstant(val, m_loc, awst::WType::biguintType());
		return c;
	};

	// Promote to biguint for mod 2^N wrapping.
	_left = promoteSignedOperandToBiguint(std::move(_left), m_loc);
	_right = promoteSignedOperandToBiguint(std::move(_right), m_loc);

	// Mask to N bits: uint64 two's-complement may exceed 2^N (e.g. int8(-2)=uint64(2^64-2)).
	if (bits < 256)
	{
		auto maskOp = [&](std::shared_ptr<awst::Expression> val)
			-> std::shared_ptr<awst::Expression> {
			auto mod = awst::makeBigUIntBinOp(std::move(val), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);
			return mod;
		};
		_left = maskOp(std::move(_left));
		_right = maskOp(std::move(_right));
	}

	// Step 1: unsigned op in biguint. Sub: (a + 2^N - b) avoids negative biguint.
	std::shared_ptr<awst::Expression> rawResult;
	bool isSub = (_op == Token::Sub || _op == Token::AssignSub);

	if (isSub)
	{
		// a < 2^N, b < 2^N → a + 2^N - b is always non-negative
		auto aPlusPow = awst::makeBigUIntBinOp(_left, awst::BigUIntBinaryOperator::Add, makeBiguintConst(pow2NStr), m_loc);

		auto subB = awst::makeBigUIntBinOp(std::move(aPlusPow), awst::BigUIntBinaryOperator::Sub, _right, m_loc);
		rawResult = std::move(subB);
	}
	else
	{
		auto bigOp = (_op == Token::Mul || _op == Token::AssignMul)
			? awst::BigUIntBinaryOperator::Mult
			: awst::BigUIntBinaryOperator::Add;
		auto binOp = awst::makeBigUIntBinOp(_left, bigOp, _right, m_loc);
		rawResult = std::move(binOp);
	}

	// Step 2: wrap mod 2^N (two's complement)
	{
		auto wrapOp = awst::makeBigUIntBinOp(std::move(rawResult), awst::BigUIntBinaryOperator::Mod, makeBiguintConst(pow2NStr), m_loc);
		rawResult = std::move(wrapOp);
	}

	// Step 3: overflow check (skipped in unchecked blocks).
	// add: overflow iff a_neg == b_neg && a_neg != result_neg
	// sub: overflow iff a_neg != b_neg && a_neg != result_neg
	// mul: range check (result in [-2^(N-1), 2^(N-1)-1])
	if (!m_scope.isUnchecked())
	{

		// isNeg: val >= half (i.e. NOT (val < half))
		auto isNeg = [&](std::shared_ptr<awst::Expression> const& val)
			-> std::shared_ptr<awst::Expression> {
			auto cmp = awst::makeNumericCompare(promoteSignedOperandToBiguint(val, m_loc), awst::NumericComparison::Lt, makeBiguintConst(halfNStr), m_loc);
			auto notExpr = awst::makeNot(std::move(cmp), m_loc);
			return notExpr;
		};

		std::shared_ptr<awst::Expression> overflowCond;

		if (_op == Token::Add || _op == Token::AssignAdd)
		{
			// No overflow when: a_neg != b_neg || a_neg == result_neg
			auto aNeg = isNeg(_left);
			auto bNeg = isNeg(_right);
			auto rNeg = isNeg(rawResult);

			auto diffSigns = awst::makeNumericCompare(aNeg, awst::NumericComparison::Ne, bNeg, m_loc);
			auto sameSignResult = awst::makeNumericCompare(aNeg, awst::NumericComparison::Eq, rNeg, m_loc);
			auto noOverflow = awst::makeBoolBinOp(std::move(diffSigns), awst::BinaryBooleanOperator::Or, std::move(sameSignResult), m_loc);
			overflowCond = std::move(noOverflow);
		}
		else if (_op == Token::Sub || _op == Token::AssignSub)
		{
			// No overflow when: a_neg == b_neg || a_neg == result_neg
			auto aNeg = isNeg(_left);
			auto bNeg = isNeg(_right);
			auto rNeg = isNeg(rawResult);

			auto noOverflow = awst::makeBoolBinOp(
				awst::makeNumericCompare(aNeg, awst::NumericComparison::Eq, bNeg, m_loc),
				awst::BinaryBooleanOperator::Or,
				awst::makeNumericCompare(aNeg, awst::NumericComparison::Eq, rNeg, m_loc),
				m_loc);
			overflowCond = std::move(noOverflow);
		}
		else // mul
		{
			// Exact biguint product; check abs(a)*abs(b) fits in signed range.
			// abs(a) = a < half ? a : pow2N - a
			auto absVal = [&](std::shared_ptr<awst::Expression> const& val)
				-> std::shared_ptr<awst::Expression> {
				auto neg = isNeg(val); // val >= half
				// pow2N - val
				auto negated = awst::makeBigUIntBinOp(makeBiguintConst(pow2NStr), awst::BigUIntBinaryOperator::Sub, val, m_loc);
				// neg ? (pow2N - val) : val
				return awst::makeConditional(
					std::move(neg), std::move(negated), val,
					awst::WType::biguintType(), m_loc);
			};

			auto absA = absVal(_left);
			auto absB = absVal(_right);

			auto absProduct = awst::makeBigUIntBinOp(std::move(absA), awst::BigUIntBinaryOperator::Mult, std::move(absB), m_loc);

			// Same sign → result positive → absProduct < half
			// Diff sign → result negative → absProduct <= half (handles -1*MIN_INT)
			auto aNeg2 = isNeg(_left);
			auto bNeg2 = isNeg(_right);
			auto sameSign = awst::makeNumericCompare(aNeg2, awst::NumericComparison::Eq, bNeg2, m_loc);

			auto ltHalf = awst::makeNumericCompare(absProduct, awst::NumericComparison::Lt, makeBiguintConst(halfNStr), m_loc);
			// absProduct <= half = NOT (half < absProduct)
			auto leHalf = awst::makeNot(awst::makeNumericCompare(makeBiguintConst(halfNStr), awst::NumericComparison::Lt, absProduct, m_loc), m_loc);

			auto rangeCheck = awst::makeConditional(
				std::move(sameSign), std::move(ltHalf), std::move(leHalf),
				awst::WType::boolType(), m_loc);

			// b==0 → no overflow
			auto bZero = awst::makeNumericCompare(_right, awst::NumericComparison::Eq, makeBiguintConst("0"), m_loc);

			auto noOverflow = awst::makeBoolBinOp(std::move(bZero), awst::BinaryBooleanOperator::Or, std::move(rangeCheck), m_loc);

			overflowCond = std::move(noOverflow);
		}

		if (overflowCond)
		{
			auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(overflowCond), m_loc, "signed arithmetic overflow"), m_loc);
			m_ctx.prePendingStatements.push_back(std::move(assertStmt));
		}
	}

	// Step 4: truncate to uint64 for ≤64-bit types.
	if (!isBiguint)
		return awst::makeBiguintToUInt64(std::move(rawResult), m_loc);

	// Step 5: canonicalise sub-256 signed result to 256-bit two's complement.
	// mod 2^N leaves a negative int<N> in N-bit form (sign bit at N-1, bit 255 clear).
	// The pipeline treats canonical signed as 256-bit (XOR with 2^255 in SolIntegerBuilder),
	// so N-bit form reads as POSITIVE. Without this, V4 getAmount0/1Delta(int128) `liquidity<0`
	// takes the wrong branch and uint128(liquidity) becomes ~2^128 (verified garbage).
	// Idempotent with signExtendToUint256 at assignment/return/cast sites; no-op for
	// non-negative results and int256.
	if (bits < 256)
		return TypeCoercion::signExtendToUint256(std::move(rawResult), bits, m_loc);

	return rawResult;
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

	return awst::makeConditional(
		std::move(doNeg), std::move(negMod), std::move(absResult),
		awst::WType::biguintType(), m_loc);
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
