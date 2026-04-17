#include "builder/ExpressionBuilder.h"
#include "builder/sol-ast/SolExpressionDispatch.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

LibraryFunctionIdMap const ExpressionBuilder::s_emptyLibraryFunctionIds{};

std::shared_ptr<awst::Expression> ExpressionBuilder::implicitNumericCast(
	std::shared_ptr<awst::Expression> _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc
)
{
	return TypeCoercion::implicitNumericCast(std::move(_expr), _targetType, _loc);
}

static OverloadedNamesSet const s_emptyOverloads;

FreeFunctionIdMap const ExpressionBuilder::s_emptyFreeFunctionIds;

void ExpressionBuilder::trackConstantLocal(int64_t _declId, unsigned long long _value)
{
	m_constantLocals[_declId] = _value;
}

unsigned long long ExpressionBuilder::getConstantLocal(solidity::frontend::Declaration const* _decl) const
{
	if (!_decl)
		return 0;
	auto it = m_constantLocals.find(_decl->id());
	return it != m_constantLocals.end() ? it->second : 0;
}

void ExpressionBuilder::trackFuncPtrTarget(int64_t _declId, solidity::frontend::FunctionDefinition const* _func)
{
	m_funcPtrTargets[_declId] = _func;
}

solidity::frontend::FunctionDefinition const* ExpressionBuilder::getFuncPtrTarget(int64_t _declId) const
{
	auto it = m_funcPtrTargets.find(_declId);
	return it != m_funcPtrTargets.end() ? it->second : nullptr;
}

ExpressionBuilder::ExpressionBuilder(
	TypeMapper& _typeMapper,
	StorageMapper& _storageMapper,
	std::string const& _sourceFile,
	std::string const& _contractName,
	LibraryFunctionIdMap const& _libraryFunctionIds,
	OverloadedNamesSet const& _overloadedNames,
	FreeFunctionIdMap const& _freeFunctionById
)
	: m_typeMapper(_typeMapper),
	  m_storageMapper(_storageMapper),
	  m_sourceFile(_sourceFile),
	  m_contractName(_contractName),
	  m_libraryFunctionIds(_libraryFunctionIds),
	  m_overloadedNames(_overloadedNames.empty() ? s_emptyOverloads : _overloadedNames),
	  m_freeFunctionById(_freeFunctionById.empty() ? s_emptyFreeFunctionIds : _freeFunctionById)
{
	Logger::instance().debug("[TRACE] ExpressionBuilder m_freeFunctionById.size()=" + std::to_string(m_freeFunctionById.size()) + " paramSize=" + std::to_string(_freeFunctionById.size()) + " addr=" + std::to_string((uintptr_t)&m_freeFunctionById));
	// Factory is created lazily in visit(FunctionCall) since it needs a BuilderContext
}

std::string ExpressionBuilder::resolveMethodName(
	solidity::frontend::FunctionDefinition const& _func
)
{
	std::string name = _func.name();
	if (m_overloadedNames.count(name))
	{
		name += "(";
		bool first = true;
		for (auto const& p: _func.parameters())
		{
			if (!first) name += ",";
			auto const* solType = p->type();
			if (dynamic_cast<solidity::frontend::BoolType const*>(solType))
				name += "b";
			else if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solType))
				name += (intType->isSigned() ? "i" : "u") + std::to_string(intType->numBits());
			else if (dynamic_cast<solidity::frontend::AddressType const*>(solType))
				name += "addr";
			else if (auto const* fixedBytes = dynamic_cast<solidity::frontend::FixedBytesType const*>(solType))
				name += "b" + std::to_string(fixedBytes->numBytes());
			else
				name += std::to_string(p->id());
			first = false;
		}
		name += ")";
	}
	return name;
}

std::shared_ptr<awst::Expression> ExpressionBuilder::build(
	solidity::frontend::Expression const& _expr
)
{
	m_builderCtxPool.clear();
	auto ctx = makeBuilderContext();
	return sol_ast::buildExpression(ctx, _expr);
}

std::vector<std::shared_ptr<awst::Statement>> ExpressionBuilder::takePendingStatements()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.swap(m_pendingStatements);
	return result;
}

std::vector<std::shared_ptr<awst::Statement>> ExpressionBuilder::takePrePendingStatements()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.swap(m_prePendingStatements);
	return result;
}

void ExpressionBuilder::addParamRemap(int64_t _declId, std::string const& _uniqueName, awst::WType const* _type)
{
	m_paramRemaps[_declId] = {_uniqueName, _type};
}

void ExpressionBuilder::removeParamRemap(int64_t _declId)
{
	m_paramRemaps.erase(_declId);
}

void ExpressionBuilder::addSuperTarget(int64_t _funcId, std::string const& _name)
{
	m_superTargetNames[_funcId] = _name;
}

void ExpressionBuilder::clearSuperTargets()
{
	m_superTargetNames.clear();
}

void ExpressionBuilder::addStorageAlias(int64_t _declId, std::shared_ptr<awst::Expression> _expr)
{
	m_storageAliases[_declId] = std::move(_expr);
}

void ExpressionBuilder::removeStorageAlias(int64_t _declId)
{
	m_storageAliases.erase(_declId);
}

void ExpressionBuilder::addMappingKeyParam(int64_t _declId, std::string const& _paramName)
{
	m_mappingKeyParams[_declId] = _paramName;
}

std::string ExpressionBuilder::getMappingKeyParam(int64_t _declId) const
{
	auto it = m_mappingKeyParams.find(_declId);
	return it != m_mappingKeyParams.end() ? it->second : std::string{};
}

awst::SourceLocation ExpressionBuilder::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc
)
{
	awst::SourceLocation loc;
	loc.file = m_sourceFile;
	loc.line = _solLoc.start >= 0 ? _solLoc.start : 0;
	loc.endLine = _solLoc.end >= 0 ? _solLoc.end : 0;
	return loc;
}

bool ExpressionBuilder::isBigUInt(awst::WType const* _type)
{
	return _type == awst::WType::biguintType();
}


eb::BuilderContext ExpressionBuilder::makeBuilderContext()
{
	return eb::BuilderContext{
		/*.typeMapper =*/ m_typeMapper,
		/*.storageMapper =*/ m_storageMapper,
		/*.transientStorage =*/ m_transientStorage,
		/*.sourceFile =*/ m_sourceFile,
		/*.contractName =*/ m_contractName,
		/*.currentContract =*/ m_currentContract,
		/*.libraryFunctionIds =*/ m_libraryFunctionIds,
		/*.overloadedNames =*/ m_overloadedNames,
		/*.freeFunctionById =*/ m_freeFunctionById,
		/*.pendingStatements =*/ m_pendingStatements,
		/*.prePendingStatements =*/ m_prePendingStatements,
		/*.paramRemaps =*/ m_paramRemaps,
		/*.superTargetNames =*/ m_superTargetNames,
		/*.storageAliases =*/ m_storageAliases,
		/*.slotStorageRefs =*/ m_slotStorageRefs,
		/*.funcPtrTargets =*/ m_funcPtrTargets,
		/*.constantLocals =*/ m_constantLocals,
		/*.varNameToId =*/ m_varNameToId,
		/*.mappingKeyParams =*/ m_mappingKeyParams,
		/*.inConstructor =*/ m_inConstructor,
		/*.inUncheckedBlock =*/ m_inUncheckedBlock,
		/*.pendingArrayPushValue =*/ m_pendingArrayPushValue,
		/*.buildExpr =*/ [this](solidity::frontend::Expression const& _expr) {
			return this->build(_expr);
		},
		/*.buildBinaryOp =*/ [this](solidity::frontend::Token _op,
			std::shared_ptr<awst::Expression> _left,
			std::shared_ptr<awst::Expression> _right,
			awst::WType const* _resultType,
			awst::SourceLocation const& _loc) {
			return this->buildBinaryOp(_op, std::move(_left), std::move(_right), _resultType, _loc);
		},
		/*.builderForInstance =*/ [this](solidity::frontend::Type const* _solType, std::shared_ptr<awst::Expression> _expr) {
			// The returned builder holds BuilderContext by reference, so the context
			// must outlive the builder. We accumulate contexts in a vector that persists
			// for the lifetime of ExpressionBuilder (cleared between build() calls).
			m_builderCtxPool.push_back(std::make_unique<eb::BuilderContext>(makeBuilderContext()));
			return m_registry.tryBuildInstance(*m_builderCtxPool.back(), _solType, std::move(_expr));
		},
	};
}

std::shared_ptr<awst::Expression> ExpressionBuilder::buildBinaryOp(
	solidity::frontend::Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::WType const* _resultType,
	awst::SourceLocation const& _loc
)
{
	using Token = solidity::frontend::Token;

	// Helper to coerce bytes[N] operands to a numeric type when used in numeric context.
	// For bytes[N] where N > 8 — or bytes of unknown length (e.g. keccak256
	// output, which is a 32-byte digest but comes back typed as `bytes`) —
	// promotes to biguint via ReinterpretCast. btoi only handles ≤8 bytes
	// and fails at runtime for anything longer, so unknown-length bytes
	// must take the biguint path.
	// For fixed bytes[N≤8], uses btoi → uint64.
	auto coerceBytesToUint = [&](std::shared_ptr<awst::Expression>& operand) {
		if (operand->wtype && operand->wtype->kind() == awst::WTypeKind::Bytes)
		{
			auto const* bytesWType = dynamic_cast<awst::BytesWType const*>(operand->wtype);
			bool knownSmall =
				bytesWType && bytesWType->length().has_value() && *bytesWType->length() <= 8;
			if (!knownSmall)
			{
				auto cast = awst::makeReinterpretCast(std::move(operand), awst::WType::biguintType(), _loc);
				operand = std::move(cast);
				return;
			}
			auto expr = std::move(operand);
			if (expr->wtype != awst::WType::bytesType())
			{
				auto toBytes = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), _loc);
				expr = std::move(toBytes);
			}
			auto btoi = std::make_shared<awst::IntrinsicCall>();
			btoi->sourceLocation = _loc;
			btoi->wtype = awst::WType::uint64Type();
			btoi->opCode = "btoi";
			btoi->stackArgs.push_back(std::move(expr));
			operand = std::move(btoi);
		}
	};

	// Auto-coerce bytes[N] operands to uint64 when the other operand is numeric
	bool leftIsBytes = _left->wtype && _left->wtype->kind() == awst::WTypeKind::Bytes;
	bool rightIsBytes = _right->wtype && _right->wtype->kind() == awst::WTypeKind::Bytes;
	bool leftIsNumeric = _left->wtype == awst::WType::uint64Type()
		|| _left->wtype == awst::WType::biguintType();
	bool rightIsNumeric = _right->wtype == awst::WType::uint64Type()
		|| _right->wtype == awst::WType::biguintType();
	if (leftIsBytes && rightIsNumeric)
		coerceBytesToUint(_left);
	if (rightIsBytes && leftIsNumeric)
		coerceBytesToUint(_right);

	// Helper to promote uint64 to biguint
	auto promoteToBigUInt = [&](std::shared_ptr<awst::Expression>& operand) {
		if (operand->wtype == awst::WType::uint64Type())
		{
			// For integer constants, use IntegerConstant(biguint) directly
			// to avoid itob(0) producing 8 zero bytes vs biguint(0) = empty bytes.
			if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(operand.get()))
			{
				auto bigConst = awst::makeIntegerConstant(intConst->value, _loc, awst::WType::biguintType());
				operand = std::move(bigConst);
				return;
			}

			auto itob = std::make_shared<awst::IntrinsicCall>();
			itob->sourceLocation = _loc;
			itob->wtype = awst::WType::bytesType();
			itob->opCode = "itob";
			itob->stackArgs.push_back(std::move(operand));

			auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), _loc);
			operand = std::move(cast);
		}
	};

	// Comparison operations
	switch (_op)
	{
	case Token::Equal:
	case Token::NotEqual:
	case Token::LessThan:
	case Token::LessThanOrEqual:
	case Token::GreaterThan:
	case Token::GreaterThanOrEqual:
	{
		// For bytes-backed types (account, bytes, bytes[N], string), use BytesComparisonExpression
		bool isBytesBacked = _left->wtype == awst::WType::accountType()
			|| (_left->wtype && _left->wtype->kind() == awst::WTypeKind::Bytes)
			|| _left->wtype == awst::WType::stringType();

		if (isBytesBacked && (_op == Token::Equal || _op == Token::NotEqual))
		{
			// Coerce both sides to bytes if they differ
			if (_left->wtype != _right->wtype)
			{
				auto castToBytes = [&](std::shared_ptr<awst::Expression>& expr) {
					if (expr->wtype != awst::WType::bytesType())
					{
						auto cast = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), _loc);
						expr = std::move(cast);
					}
				};
				castToBytes(_left);
				castToBytes(_right);
			}
			auto e = std::make_shared<awst::BytesComparisonExpression>();
			e->sourceLocation = _loc;
			e->wtype = awst::WType::boolType();
			e->lhs = std::move(_left);
			e->rhs = std::move(_right);
			e->op = (_op == Token::Equal) ? awst::EqualityComparison::Eq : awst::EqualityComparison::Ne;
			return e;
		}

		// Bytes ordering comparisons use AVM intrinsics (b<, b>, b<=, b>=)
		if (isBytesBacked)
		{
			std::string opCode;
			switch (_op)
			{
			case Token::LessThan: opCode = "b<"; break;
			case Token::LessThanOrEqual: opCode = "b<="; break;
			case Token::GreaterThan: opCode = "b>"; break;
			case Token::GreaterThanOrEqual: opCode = "b>="; break;
			default: break;
			}
			if (!opCode.empty())
			{
				auto e = std::make_shared<awst::IntrinsicCall>();
				e->sourceLocation = _loc;
				e->wtype = awst::WType::boolType();
				e->opCode = std::move(opCode);
				e->stackArgs.push_back(std::move(_left));
				e->stackArgs.push_back(std::move(_right));
				return e;
			}
		}

		// Promote if mixed uint64/biguint
		if (isBigUInt(_left->wtype) != isBigUInt(_right->wtype))
		{
			promoteToBigUInt(_left);
			promoteToBigUInt(_right);
		}

		auto e = std::make_shared<awst::NumericComparisonExpression>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::boolType();
		e->lhs = std::move(_left);
		e->rhs = std::move(_right);
		switch (_op)
		{
		case Token::Equal: e->op = awst::NumericComparison::Eq; break;
		case Token::NotEqual: e->op = awst::NumericComparison::Ne; break;
		case Token::LessThan: e->op = awst::NumericComparison::Lt; break;
		case Token::LessThanOrEqual: e->op = awst::NumericComparison::Lte; break;
		case Token::GreaterThan: e->op = awst::NumericComparison::Gt; break;
		case Token::GreaterThanOrEqual: e->op = awst::NumericComparison::Gte; break;
		default: break;
		}
		return e;
	}

	// Boolean operations
	case Token::And:
	{
		auto e = std::make_shared<awst::BooleanBinaryOperation>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::boolType();
		e->left = std::move(_left);
		e->op = awst::BinaryBooleanOperator::And;
		e->right = std::move(_right);
		return e;
	}
	case Token::Or:
	{
		auto e = std::make_shared<awst::BooleanBinaryOperation>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::boolType();
		e->left = std::move(_left);
		e->op = awst::BinaryBooleanOperator::Or;
		e->right = std::move(_right);
		return e;
	}

	default:
		break;
	}

	// Bytes bitwise operations (b|, b&, b^) — for bytes[N] types like bytes32/bytes4
	{
		bool leftIsBytesKind = _left->wtype && _left->wtype->kind() == awst::WTypeKind::Bytes;
		bool rightIsBytesKind = _right->wtype && _right->wtype->kind() == awst::WTypeKind::Bytes;
		bool isBitwiseOp = (_op == Token::BitOr || _op == Token::AssignBitOr
			|| _op == Token::BitXor || _op == Token::AssignBitXor
			|| _op == Token::BitAnd || _op == Token::AssignBitAnd);

		if ((leftIsBytesKind || rightIsBytesKind) && isBitwiseOp)
		{
			auto e = std::make_shared<awst::BytesBinaryOperation>();
			e->sourceLocation = _loc;
			e->wtype = awst::WType::bytesType();
			e->left = std::move(_left);
			e->right = std::move(_right);

			switch (_op)
			{
			case Token::BitOr: case Token::AssignBitOr: e->op = awst::BytesBinaryOperator::BitOr; break;
			case Token::BitXor: case Token::AssignBitXor: e->op = awst::BytesBinaryOperator::BitXor; break;
			case Token::BitAnd: case Token::AssignBitAnd: e->op = awst::BytesBinaryOperator::BitAnd; break;
			default: e->op = awst::BytesBinaryOperator::BitOr; break;
			}
			return e;
		}
	}

	// Arithmetic/bitwise operations — choose uint64 vs biguint
	if (isBigUInt(_resultType) || isBigUInt(_left->wtype) || isBigUInt(_right->wtype))
	{
		promoteToBigUInt(_left);

		auto e = std::make_shared<awst::BigUIntBinaryOperation>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::biguintType();

		// BigUInt doesn't have native shift ops — convert x<<n to x*(2^n), x>>n to x/(2^n)
		// Construct 2^n using setbit(bzero(32), 255-n, 1) since AVM has no bexp opcode
		// Note: _right (shift amount) must stay uint64 — do NOT promote it before this block
		if (_op == Token::SHL || _op == Token::AssignShl
			|| _op == Token::SHR || _op == Token::AssignShr
			|| _op == Token::SAR || _op == Token::AssignSar)
		{
			auto shiftAmt = implicitNumericCast(std::move(_right), awst::WType::uint64Type(), _loc);

			// bzero(32) — 256-bit zero buffer
			auto thirtyTwo = awst::makeIntegerConstant("32", _loc);

			auto bzero = std::make_shared<awst::IntrinsicCall>();
			bzero->sourceLocation = _loc;
			bzero->wtype = awst::WType::bytesType();
			bzero->opCode = "bzero";
			bzero->stackArgs.push_back(std::move(thirtyTwo));

			// 255 - n: setbit uses MSB-first ordering, so bit (255-n) = 2^n
			auto twoFiftyFive = awst::makeIntegerConstant("255", _loc);

			auto bitIdx = std::make_shared<awst::UInt64BinaryOperation>();
			bitIdx->sourceLocation = _loc;
			bitIdx->wtype = awst::WType::uint64Type();
			bitIdx->left = std::move(twoFiftyFive);
			bitIdx->right = std::move(shiftAmt);
			bitIdx->op = awst::UInt64BinaryOperator::Sub;

			// setbit(bzero(32), 255-n, 1) → bytes with only bit n set
			auto one = awst::makeIntegerConstant("1", _loc);

			auto setbit = std::make_shared<awst::IntrinsicCall>();
			setbit->sourceLocation = _loc;
			setbit->wtype = awst::WType::bytesType();
			setbit->opCode = "setbit";
			setbit->stackArgs.push_back(std::move(bzero));
			setbit->stackArgs.push_back(std::move(bitIdx));
			setbit->stackArgs.push_back(std::move(one));

			// Cast bytes → biguint
			auto castToBigUInt = awst::makeReinterpretCast(std::move(setbit), awst::WType::biguintType(), _loc);

			e->left = std::move(_left);
			e->right = std::move(castToBigUInt);
			e->op = (_op == Token::SHL || _op == Token::AssignShl)
				? awst::BigUIntBinaryOperator::Mult
				: awst::BigUIntBinaryOperator::FloorDiv;
			return e;
		}

		// For non-shift ops, promote right operand to biguint now
		promoteToBigUInt(_right);

		if (_op == Token::Sub || _op == Token::AssignSub)
		{
			// Checked subtraction: assert a >= b before wrapping
			if (!m_inUncheckedBlock)
			{
				auto cmp = awst::makeNumericCompare(_left, awst::NumericComparison::Gte, _right, _loc);

				auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), _loc, "underflow"), _loc);
				m_prePendingStatements.push_back(std::move(assertStmt));
			}

			// Biguint subtraction needs wrapping mod 2^256 to avoid AVM underflow.
			// Pattern: (a + 2^256 - b) % 2^256
			auto pow256 = awst::makeIntegerConstant(kPow2_256, _loc, awst::WType::biguintType());

			auto addPow = std::make_shared<awst::BigUIntBinaryOperation>();
			addPow->sourceLocation = _loc;
			addPow->wtype = awst::WType::biguintType();
			addPow->left = std::move(_left);
			addPow->op = awst::BigUIntBinaryOperator::Add;
			addPow->right = pow256;

			auto diff = std::make_shared<awst::BigUIntBinaryOperation>();
			diff->sourceLocation = _loc;
			diff->wtype = awst::WType::biguintType();
			diff->left = std::move(addPow);
			diff->op = awst::BigUIntBinaryOperator::Sub;
			diff->right = std::move(_right);

			auto pow256b = awst::makeIntegerConstant(kPow2_256, _loc, awst::WType::biguintType());

			auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
			mod->sourceLocation = _loc;
			mod->wtype = awst::WType::biguintType();
			mod->left = std::move(diff);
			mod->op = awst::BigUIntBinaryOperator::Mod;
			mod->right = std::move(pow256b);
			return mod;
		}


		// BigUInt exponentiation: AVM has no biguint exp opcode, so build a
		// square-and-multiply loop emitted via m_pendingStatements.
		if (_op == Token::Exp)
		{
			static int expCounter = 0;
			int id = expCounter++;
			std::string resultVar = "__biguint_exp_result_" + std::to_string(id);
			std::string baseVar = "__biguint_exp_base_" + std::to_string(id);
			std::string expVar = "__biguint_exp_exp_" + std::to_string(id);

			auto makeVar = [&](std::string const& name) -> std::shared_ptr<awst::VarExpression>
			{
				auto v = awst::makeVarExpression(name, awst::WType::biguintType(), _loc);
				return v;
			};
			auto makeConst = [&](std::string const& value) -> std::shared_ptr<awst::IntegerConstant>
			{
				auto c = awst::makeIntegerConstant(value, _loc, awst::WType::biguintType());
				return c;
			};
			auto makeAssign = [&](
				std::string const& target,
				std::shared_ptr<awst::Expression> value
			) -> std::shared_ptr<awst::AssignmentStatement>
			{
				auto assign = std::make_shared<awst::AssignmentStatement>();
				assign->sourceLocation = _loc;
				assign->target = makeVar(target);
				assign->value = std::move(value);
				return assign;
			};
			auto makeBinOp = [&](
				std::shared_ptr<awst::Expression> lhs,
				awst::BigUIntBinaryOperator op,
				std::shared_ptr<awst::Expression> rhs
			) -> std::shared_ptr<awst::BigUIntBinaryOperation>
			{
				auto bin = std::make_shared<awst::BigUIntBinaryOperation>();
				bin->sourceLocation = _loc;
				bin->wtype = awst::WType::biguintType();
				bin->left = std::move(lhs);
				bin->op = op;
				bin->right = std::move(rhs);
				return bin;
			};

			// Ensure both operands are biguint (they may be uint64)
			auto baseExpr = implicitNumericCast(std::move(_left), awst::WType::biguintType(), _loc);
			auto expExpr = implicitNumericCast(std::move(_right), awst::WType::biguintType(), _loc);

			// __biguint_exp_result = 1
			m_prePendingStatements.push_back(makeAssign(resultVar, makeConst("1")));
			// __biguint_exp_base = base
			m_prePendingStatements.push_back(makeAssign(baseVar, std::move(baseExpr)));
			// __biguint_exp_exp = exp
			m_prePendingStatements.push_back(makeAssign(expVar, std::move(expExpr)));

			// while __biguint_exp_exp > 0:
			auto loop = std::make_shared<awst::WhileLoop>();
			loop->sourceLocation = _loc;
			{
				auto cond = awst::makeNumericCompare(makeVar(expVar), awst::NumericComparison::Gt, makeConst("0"), _loc);
				loop->condition = std::move(cond);
			}

			auto body = std::make_shared<awst::Block>();
			body->sourceLocation = _loc;

			// In unchecked mode, Solidity wraps exponentiation modulo 2^256
			// so that huge exponents (e.g. 2**1113) don't overflow biguint.
			// Take each intermediate result mod 2^256 inside the loop.
			bool const wrapMod = m_inUncheckedBlock;
			auto wrapMod256 = [&](std::shared_ptr<awst::Expression> v)
				-> std::shared_ptr<awst::Expression>
			{
				if (!wrapMod) return v;
				auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
				mod->sourceLocation = _loc;
				mod->wtype = awst::WType::biguintType();
				mod->left = std::move(v);
				mod->op = awst::BigUIntBinaryOperator::Mod;
				mod->right = makeConst(kPow2_256);
				return mod;
			};

			// if exp & 1 != 0: result = result * base
			{
				auto expAnd1 = makeBinOp(makeVar(expVar), awst::BigUIntBinaryOperator::BitAnd, makeConst("1"));
				auto isOdd = awst::makeNumericCompare(std::move(expAnd1), awst::NumericComparison::Ne, makeConst("0"), _loc);

				std::shared_ptr<awst::Expression> product =
					makeBinOp(makeVar(resultVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar));
				product = wrapMod256(std::move(product));

				auto ifBlock = std::make_shared<awst::Block>();
				ifBlock->sourceLocation = _loc;
				ifBlock->body.push_back(makeAssign(resultVar, std::move(product)));

				auto ifStmt = std::make_shared<awst::IfElse>();
				ifStmt->sourceLocation = _loc;
				ifStmt->condition = std::move(isOdd);
				ifStmt->ifBranch = std::move(ifBlock);

				body->body.push_back(std::move(ifStmt));
			}

			// exp = exp / 2
			body->body.push_back(makeAssign(expVar,
				makeBinOp(makeVar(expVar), awst::BigUIntBinaryOperator::FloorDiv, makeConst("2"))));

			// base = base * base (wrapped mod 2^256 in unchecked mode)
			{
				std::shared_ptr<awst::Expression> baseSq =
					makeBinOp(makeVar(baseVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar));
				baseSq = wrapMod256(std::move(baseSq));
				body->body.push_back(makeAssign(baseVar, std::move(baseSq)));
			}

			loop->loopBody = std::move(body);
			m_prePendingStatements.push_back(std::move(loop));

			return makeVar(resultVar);
		}

		e->left = std::move(_left);
		e->right = std::move(_right);

		switch (_op)
		{
		case Token::Add: case Token::AssignAdd: e->op = awst::BigUIntBinaryOperator::Add; break;
		case Token::Mul: case Token::AssignMul: e->op = awst::BigUIntBinaryOperator::Mult; break;
		case Token::Div: case Token::AssignDiv: e->op = awst::BigUIntBinaryOperator::FloorDiv; break;
		case Token::Mod: case Token::AssignMod: e->op = awst::BigUIntBinaryOperator::Mod; break;
		case Token::BitOr: case Token::AssignBitOr: e->op = awst::BigUIntBinaryOperator::BitOr; break;
		case Token::BitXor: case Token::AssignBitXor: e->op = awst::BigUIntBinaryOperator::BitXor; break;
		case Token::BitAnd: case Token::AssignBitAnd: e->op = awst::BigUIntBinaryOperator::BitAnd; break;
		default: e->op = awst::BigUIntBinaryOperator::Add; break;
		}

		// In unchecked blocks, arithmetic must wrap mod 2^256 (EVM semantics).
		// AVM biguint is arbitrary-precision; without truncation, results can
		// exceed 256 bits and break subsequent operations.
		if (m_inUncheckedBlock
			&& (_op == Token::Add || _op == Token::AssignAdd
				|| _op == Token::Sub || _op == Token::AssignSub
				|| _op == Token::Mul || _op == Token::AssignMul))
		{
			auto pow256 = awst::makeIntegerConstant(kPow2_256, _loc, awst::WType::biguintType());

			auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
			mod->sourceLocation = _loc;
			mod->wtype = awst::WType::biguintType();
			mod->left = e;
			mod->op = awst::BigUIntBinaryOperator::Mod;
			mod->right = std::move(pow256);
			return mod;
		}

		return e;
	}
	else
	{
		auto e = std::make_shared<awst::UInt64BinaryOperation>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::uint64Type();
		e->left = std::move(_left);
		e->right = std::move(_right);

		switch (_op)
		{
		case Token::Add: case Token::AssignAdd: e->op = awst::UInt64BinaryOperator::Add; break;
		case Token::Sub: case Token::AssignSub: e->op = awst::UInt64BinaryOperator::Sub; break;
		case Token::Mul: case Token::AssignMul: e->op = awst::UInt64BinaryOperator::Mult; break;
		case Token::Div: case Token::AssignDiv: e->op = awst::UInt64BinaryOperator::FloorDiv; break;
		case Token::Mod: case Token::AssignMod: e->op = awst::UInt64BinaryOperator::Mod; break;
		case Token::Exp:
		{
			// AVM `exp` opcode asserts on 0^0. Solidity defines 0**0 = 1.
			// Wrap: y == 0 ? 1 : x ** y
			e->op = awst::UInt64BinaryOperator::Pow;

			auto zero = awst::makeIntegerConstant("0", _loc);

			auto cond = awst::makeNumericCompare(e->right, awst::NumericComparison::Eq, std::move(zero), _loc);

			auto one = awst::makeIntegerConstant("1", _loc);

			auto ternary = std::make_shared<awst::ConditionalExpression>();
			ternary->sourceLocation = _loc;
			ternary->wtype = awst::WType::uint64Type();
			ternary->condition = std::move(cond);
			ternary->trueExpr = std::move(one);
			ternary->falseExpr = e;
			return ternary;
		}
		case Token::SHL: case Token::AssignShl: e->op = awst::UInt64BinaryOperator::LShift; break;
		case Token::SHR: case Token::AssignShr: case Token::SAR: case Token::AssignSar: e->op = awst::UInt64BinaryOperator::RShift; break;
		case Token::BitOr: case Token::AssignBitOr: e->op = awst::UInt64BinaryOperator::BitOr; break;
		case Token::BitXor: case Token::AssignBitXor: e->op = awst::UInt64BinaryOperator::BitXor; break;
		case Token::BitAnd: case Token::AssignBitAnd: e->op = awst::UInt64BinaryOperator::BitAnd; break;
		default: e->op = awst::UInt64BinaryOperator::Add; break;
		}
		return e;
	}
}


// ── Helpers (merged from HelpersBuilder.cpp) ──

std::shared_ptr<awst::Expression> ExpressionBuilder::buildTupleWithUpdatedField(
	std::shared_ptr<awst::Expression> _base,
	std::string const& _fieldName,
	std::shared_ptr<awst::Expression> _newValue,
	awst::SourceLocation const& _loc
)
{
	auto const* tupleType = dynamic_cast<awst::WTuple const*>(_base->wtype);
	auto const& names = *tupleType->names();
	auto const& types = tupleType->types();

	auto tuple = std::make_shared<awst::TupleExpression>();
	tuple->sourceLocation = _loc;
	tuple->wtype = _base->wtype;

	for (size_t i = 0; i < names.size(); ++i)
	{
		if (names[i] == _fieldName)
		{
			tuple->items.push_back(std::move(_newValue));
		}
		else
		{
			auto field = std::make_shared<awst::FieldExpression>();
			field->sourceLocation = _loc;
			field->base = _base;
			field->name = names[i];
			field->wtype = types[i];
			tuple->items.push_back(std::move(field));
		}
	}

	return tuple;
}

std::optional<ExpressionBuilder::StateVarInfo> ExpressionBuilder::resolveStateVar(
	std::string const& _name
)
{
	(void)_name;
	return std::nullopt;
}

std::shared_ptr<awst::IntegerConstant> ExpressionBuilder::makeUint64(
	std::string _value, awst::SourceLocation const& _loc
)
{
	auto e = awst::makeIntegerConstant(std::move(_value), _loc);
	return e;
}

std::shared_ptr<awst::Expression> ExpressionBuilder::buildCreateInnerTransaction(
	int _txnType,
	std::map<std::string, std::shared_ptr<awst::Expression>> _fields,
	awst::SourceLocation const& _loc
)
{
	// Set Fee to 0 (auto-pool from outer transaction)
	_fields["Fee"] = makeUint64("0", _loc);

	// Set TypeEnum to the transaction type
	_fields["TypeEnum"] = makeUint64(std::to_string(_txnType), _loc);

	auto* wtype = m_typeMapper.createType<awst::WInnerTransactionFields>(_txnType);

	auto create = std::make_shared<awst::CreateInnerTransaction>();
	create->sourceLocation = _loc;
	create->wtype = wtype;
	create->fields = std::move(_fields);
	return create;
}

std::shared_ptr<awst::Expression> ExpressionBuilder::buildSubmitAndReturn(
	std::shared_ptr<awst::Expression> _createExpr,
	awst::WType const* _solidityReturnType,
	awst::SourceLocation const& _loc
)
{
	// Extract transaction type from the create expression's wtype
	std::optional<int> txnType;
	if (auto const* itf = dynamic_cast<awst::WInnerTransactionFields const*>(_createExpr->wtype))
		txnType = itf->transactionType();

	auto* submitWtype = m_typeMapper.createType<awst::WInnerTransaction>(txnType);

	auto submit = std::make_shared<awst::SubmitInnerTransaction>();
	submit->sourceLocation = _loc;
	submit->wtype = submitWtype;
	submit->itxns.push_back(std::move(_createExpr));

	// For void returns: return submit directly
	if (!_solidityReturnType || _solidityReturnType == awst::WType::voidType())
		return submit;

	// For bool returns on non-app-call txns (transfer, send): return true
	if (_solidityReturnType == awst::WType::boolType()
		&& (!txnType.has_value() || txnType.value() != 6))
	{
		auto stmt = awst::makeExpressionStatement(std::move(submit), _loc);
		m_prePendingStatements.push_back(std::move(stmt));

		return awst::makeBoolConstant(true, _loc);
	}

	// For application calls with return values: extract from LastLog
	if (txnType.has_value() && txnType.value() == 6) // Application call
	{
		// Submit as a pre-pending statement so it executes BEFORE reading the result
		auto submitStmt = awst::makeExpressionStatement(std::move(submit), _loc);
		m_prePendingStatements.push_back(std::move(submitStmt));

		// Read LastLog from most recently submitted inner txn using itxn intrinsic
		auto readLog = std::make_shared<awst::IntrinsicCall>();
		readLog->sourceLocation = _loc;
		readLog->wtype = awst::WType::bytesType();
		readLog->opCode = "itxn";
		readLog->immediates = {std::string("LastLog")};

		// Strip the 4-byte ARC4 return prefix (0x151f7c75)
		auto stripPrefix = std::make_shared<awst::IntrinsicCall>();
		stripPrefix->sourceLocation = _loc;
		stripPrefix->opCode = "extract";
		stripPrefix->immediates = {4, 0}; // offset 4, length 0 = rest of bytes
		stripPrefix->wtype = awst::WType::bytesType();
		stripPrefix->stackArgs.push_back(std::move(readLog));

		// Convert raw ABI bytes to the target Solidity type
		if (_solidityReturnType == awst::WType::biguintType())
		{
			auto cast = awst::makeReinterpretCast(std::move(stripPrefix), awst::WType::biguintType(), _loc);
			return cast;
		}
		else if (_solidityReturnType == awst::WType::uint64Type())
		{
			auto btoi = std::make_shared<awst::IntrinsicCall>();
			btoi->sourceLocation = _loc;
			btoi->opCode = "btoi";
			btoi->wtype = awst::WType::uint64Type();
			btoi->stackArgs.push_back(std::move(stripPrefix));
			return btoi;
		}
		else if (_solidityReturnType == awst::WType::boolType())
		{
			// ARC4 bool is 1 byte: 0x80 = true, 0x00 = false.
			// Extract bit 0 (MSB) using getbit to get 0 or 1, then compare != 0 for bool.
			auto getbit = std::make_shared<awst::IntrinsicCall>();
			getbit->sourceLocation = _loc;
			getbit->opCode = "getbit";
			getbit->wtype = awst::WType::uint64Type();
			getbit->stackArgs.push_back(std::move(stripPrefix));
			getbit->stackArgs.push_back(makeUint64("0", _loc));

			auto cmp = awst::makeNumericCompare(std::move(getbit), awst::NumericComparison::Ne, makeUint64("0", _loc), _loc);
			return cmp;
		}
		else if (_solidityReturnType == awst::WType::accountType())
		{
			auto cast = awst::makeReinterpretCast(std::move(stripPrefix), awst::WType::accountType(), _loc);
			return cast;
		}
		// For tuple/struct returns: decode each field from ARC4-encoded bytes
		if (auto const* tupleType = dynamic_cast<awst::WTuple const*>(_solidityReturnType))
		{
			// Wrap stripPrefix in SingleEvaluation so it's only read once
			auto singleBytes = std::make_shared<awst::SingleEvaluation>();
			singleBytes->sourceLocation = _loc;
			singleBytes->wtype = awst::WType::bytesType();
			singleBytes->source = std::move(stripPrefix);
			singleBytes->id = 0; // unique per method

			auto tuple = std::make_shared<awst::TupleExpression>();
			tuple->sourceLocation = _loc;
			tuple->wtype = _solidityReturnType;

			int offset = 0;
			for (size_t i = 0; i < tupleType->types().size(); ++i)
			{
				auto const* fieldType = tupleType->types()[i];
				int fieldSize = 0;

				if (fieldType == awst::WType::biguintType())
					fieldSize = 32; // ARC4 uint256
				else if (fieldType == awst::WType::uint64Type())
					fieldSize = 8;
				else if (fieldType == awst::WType::boolType())
					fieldSize = 1; // ARC4 bool
				else if (fieldType == awst::WType::accountType())
					fieldSize = 32;
				else if (auto const* bwt = dynamic_cast<awst::BytesWType const*>(fieldType))
				{
					if (bwt->length().has_value())
						fieldSize = static_cast<int>(bwt->length().value());
					else
						fieldSize = 0; // dynamic — not handled
				}

				if (fieldSize == 0)
				{
					// Can't decode dynamic fields, return bytes for the rest
					tuple->items.push_back(singleBytes);
					break;
				}

				// extract3(bytes, offset, fieldSize)
				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = _loc;
				extract->opCode = "extract3";
				extract->wtype = awst::WType::bytesType();
				extract->stackArgs.push_back(singleBytes);
				extract->stackArgs.push_back(makeUint64(std::to_string(offset), _loc));
				extract->stackArgs.push_back(makeUint64(std::to_string(fieldSize), _loc));

				// Cast extracted bytes to the correct type
				std::shared_ptr<awst::Expression> fieldExpr;
				if (fieldType == awst::WType::biguintType())
				{
					auto cast = awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), _loc);
					fieldExpr = std::move(cast);
				}
				else if (fieldType == awst::WType::uint64Type())
				{
					auto btoi = std::make_shared<awst::IntrinsicCall>();
					btoi->sourceLocation = _loc;
					btoi->opCode = "btoi";
					btoi->wtype = awst::WType::uint64Type();
					btoi->stackArgs.push_back(std::move(extract));
					fieldExpr = std::move(btoi);
				}
				else if (fieldType == awst::WType::boolType())
				{
					auto getbit = std::make_shared<awst::IntrinsicCall>();
					getbit->sourceLocation = _loc;
					getbit->opCode = "getbit";
					getbit->wtype = awst::WType::uint64Type();
					getbit->stackArgs.push_back(std::move(extract));
					getbit->stackArgs.push_back(makeUint64("0", _loc));

					auto cmp = awst::makeNumericCompare(std::move(getbit), awst::NumericComparison::Ne, makeUint64("0", _loc), _loc);
					fieldExpr = std::move(cmp);
				}
				else if (fieldType == awst::WType::accountType())
				{
					auto cast = awst::makeReinterpretCast(std::move(extract), awst::WType::accountType(), _loc);
					fieldExpr = std::move(cast);
				}
				else
				{
					// bytes or fixed bytes — cast to target type
					auto cast = awst::makeReinterpretCast(std::move(extract), fieldType, _loc);
					fieldExpr = std::move(cast);
				}

				tuple->items.push_back(std::move(fieldExpr));
				offset += fieldSize;
			}

			return tuple;
		}

		// For ARC4Struct returns: wrap bytes in ReinterpretCast to struct type
		if (dynamic_cast<awst::ARC4Struct const*>(_solidityReturnType))
		{
			auto cast = awst::makeReinterpretCast(std::move(stripPrefix), _solidityReturnType, _loc);
			return cast;
		}

		// For bytes/string: return stripped bytes directly
		return stripPrefix;
	}

	// For non-appl returns: emit submit as pending, return type-appropriate default
	auto stmt = awst::makeExpressionStatement(std::move(submit), _loc);
	m_pendingStatements.push_back(std::move(stmt));

	return StorageMapper::makeDefaultValue(_solidityReturnType, _loc);
}

} // namespace puyasol::builder
