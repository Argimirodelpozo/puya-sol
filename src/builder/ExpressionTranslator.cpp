#include "builder/ExpressionTranslator.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <sstream>

namespace
{

/// Compute 2^bits - 1 as a decimal string (for type(uintN).max).
std::string computeMaxUint(unsigned _bits)
{
	// Start with "1", double `_bits` times, then subtract 1
	std::string result = "1";
	for (unsigned i = 0; i < _bits; ++i)
	{
		int carry = 0;
		for (int j = static_cast<int>(result.size()) - 1; j >= 0; --j)
		{
			int digit = (result[static_cast<size_t>(j)] - '0') * 2 + carry;
			result[static_cast<size_t>(j)] = static_cast<char>('0' + digit % 10);
			carry = digit / 10;
		}
		if (carry)
			result = std::string(1, static_cast<char>('0' + carry)) + result;
	}
	// Subtract 1
	for (int j = static_cast<int>(result.size()) - 1; j >= 0; --j)
	{
		if (result[static_cast<size_t>(j)] > '0')
		{
			result[static_cast<size_t>(j)]--;
			break;
		}
		result[static_cast<size_t>(j)] = '9';
	}
	// Remove leading zeros
	size_t start = result.find_first_not_of('0');
	return start == std::string::npos ? "0" : result.substr(start);
}

} // anonymous namespace

namespace puyasol::builder
{

LibraryFunctionIdMap const ExpressionTranslator::s_emptyLibraryFunctionIds{};

std::shared_ptr<awst::Expression> ExpressionTranslator::implicitNumericCast(
	std::shared_ptr<awst::Expression> _expr,
	awst::WType const* _targetType,
	awst::SourceLocation const& _loc
)
{
	if (!_expr || !_targetType || _expr->wtype == _targetType)
		return _expr;

	// uint64 → biguint: itob then reinterpret as biguint
	if (_expr->wtype == awst::WType::uint64Type() && _targetType == awst::WType::biguintType())
	{
		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = _loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(_expr));

		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(itob);
		return cast;
	}

	// biguint → uint64: reinterpret as bytes then btoi
	if (_expr->wtype == awst::WType::biguintType() && _targetType == awst::WType::uint64Type())
	{
		auto toBytes = std::make_shared<awst::ReinterpretCast>();
		toBytes->sourceLocation = _loc;
		toBytes->wtype = awst::WType::bytesType();
		toBytes->expr = std::move(_expr);

		auto btoi = std::make_shared<awst::IntrinsicCall>();
		btoi->sourceLocation = _loc;
		btoi->wtype = awst::WType::uint64Type();
		btoi->opCode = "btoi";
		btoi->stackArgs.push_back(std::move(toBytes));
		return btoi;
	}

	return _expr;
}

static OverloadedNamesSet const s_emptyOverloads;

ExpressionTranslator::ExpressionTranslator(
	TypeMapper& _typeMapper,
	StorageMapper& _storageMapper,
	std::string const& _sourceFile,
	std::string const& _contractName,
	LibraryFunctionIdMap const& _libraryFunctionIds,
	OverloadedNamesSet const& _overloadedNames
)
	: m_typeMapper(_typeMapper),
	  m_storageMapper(_storageMapper),
	  m_sourceFile(_sourceFile),
	  m_contractName(_contractName),
	  m_libraryFunctionIds(_libraryFunctionIds),
	  m_overloadedNames(_overloadedNames.empty() ? s_emptyOverloads : _overloadedNames)
{
}

std::string ExpressionTranslator::resolveMethodName(
	solidity::frontend::FunctionDefinition const& _func
)
{
	std::string name = _func.name();
	if (m_overloadedNames.count(name))
		name += "(" + std::to_string(_func.parameters().size()) + ")";
	return name;
}

std::shared_ptr<awst::Expression> ExpressionTranslator::translate(
	solidity::frontend::Expression const& _expr
)
{
	m_stack.clear();
	_expr.accept(*this);
	if (m_stack.empty())
	{
		// Return a void constant as fallback
		auto vc = std::make_shared<awst::VoidConstant>();
		vc->sourceLocation = makeLoc(_expr.location());
		vc->wtype = awst::WType::voidType();
		return vc;
	}
	return pop();
}

void ExpressionTranslator::push(std::shared_ptr<awst::Expression> _expr)
{
	m_stack.push_back(std::move(_expr));
}

std::shared_ptr<awst::Expression> ExpressionTranslator::pop()
{
	if (m_stack.empty())
	{
		auto vc = std::make_shared<awst::VoidConstant>();
		vc->wtype = awst::WType::voidType();
		return vc;
	}
	auto expr = m_stack.back();
	m_stack.pop_back();
	return expr;
}

std::vector<std::shared_ptr<awst::Statement>> ExpressionTranslator::takePendingStatements()
{
	std::vector<std::shared_ptr<awst::Statement>> result;
	result.swap(m_pendingStatements);
	return result;
}

awst::SourceLocation ExpressionTranslator::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc
)
{
	awst::SourceLocation loc;
	loc.file = m_sourceFile;
	loc.line = _solLoc.start >= 0 ? _solLoc.start : 0;
	loc.endLine = _solLoc.end >= 0 ? _solLoc.end : 0;
	return loc;
}

bool ExpressionTranslator::isBigUInt(awst::WType const* _type)
{
	return _type == awst::WType::biguintType();
}

bool ExpressionTranslator::visit(solidity::frontend::Literal const& _node)
{
	using namespace solidity::frontend;
	auto loc = makeLoc(_node.location());
	auto const* solType = _node.annotation().type;

	switch (_node.token())
	{
	case Token::TrueLiteral:
	{
		auto e = std::make_shared<awst::BoolConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::boolType();
		e->value = true;
		push(e);
		break;
	}
	case Token::FalseLiteral:
	{
		auto e = std::make_shared<awst::BoolConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::boolType();
		e->value = false;
		push(e);
		break;
	}
	case Token::Number:
	{
		auto* mappedType = m_typeMapper.map(solType);
		// IntegerConstant must have uint64 or biguint type
		if (mappedType != awst::WType::uint64Type() && mappedType != awst::WType::biguintType())
			mappedType = awst::WType::biguintType();
		auto e = std::make_shared<awst::IntegerConstant>();
		e->sourceLocation = loc;
		e->wtype = mappedType;
		e->value = _node.value();
		push(e);
		break;
	}
	case Token::StringLiteral:
	{
		auto e = std::make_shared<awst::StringConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::stringType();
		e->value = _node.value();
		push(e);
		break;
	}
	default:
	{
		auto e = std::make_shared<awst::StringConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::stringType();
		e->value = _node.value();
		push(e);
		break;
	}
	}
	return false;
}

bool ExpressionTranslator::visit(solidity::frontend::Identifier const& _node)
{
	auto loc = makeLoc(_node.location());
	std::string name = _node.name();

	// Check if this is a state variable reference
	auto const* decl = _node.annotation().referencedDeclaration;
	if (auto const* varDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(decl))
	{
		if (varDecl->isStateVariable())
		{
			auto* type = m_typeMapper.map(varDecl->type());
			auto kind = StorageMapper::shouldUseBoxStorage(*varDecl)
				? awst::AppStorageKind::Box
				: awst::AppStorageKind::AppGlobal;

			// Dynamic arrays stored as box maps: don't read the array itself,
			// element access is handled in IndexAccess. Skip creating a read.
			if (type && type->kind() == awst::WTypeKind::ReferenceArray
				&& kind == awst::AppStorageKind::Box)
			{
				// Push a placeholder — actual access happens via IndexAccess
				auto placeholder = std::make_shared<awst::VarExpression>();
				placeholder->sourceLocation = loc;
				placeholder->name = name;
				placeholder->wtype = type;
				push(placeholder);
				return false;
			}

			// Constants and immutables are handled as literal values
			if (varDecl->isConstant() || varDecl->immutable())
			{
				// Try to evaluate the constant
				if (varDecl->value())
				{
					push(translate(*varDecl->value()));
					return false;
				}
			}

			push(m_storageMapper.createStateRead(name, type, kind, loc));
			return false;
		}
	}

	// Regular local variable
	auto e = std::make_shared<awst::VarExpression>();
	e->sourceLocation = loc;
	e->name = name;
	if (decl)
	{
		if (auto const* vd = dynamic_cast<solidity::frontend::VariableDeclaration const*>(decl))
			e->wtype = m_typeMapper.map(vd->type());
	}
	if (!e->wtype || e->wtype == awst::WType::voidType())
	{
		auto const* solType = _node.annotation().type;
		if (solType)
			e->wtype = m_typeMapper.map(solType);
	}
	push(e);
	return false;
}

std::shared_ptr<awst::Expression> ExpressionTranslator::buildBinaryOp(
	solidity::frontend::Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::WType const* _resultType,
	awst::SourceLocation const& _loc
)
{
	using Token = solidity::frontend::Token;

	// Helper to coerce bytes[N] operands to uint64 when used in numeric context
	auto coerceBytesToUint = [&](std::shared_ptr<awst::Expression>& operand) {
		if (operand->wtype && operand->wtype->kind() == awst::WTypeKind::Bytes)
		{
			// bytes[N] → bytes → btoi → uint64
			auto expr = std::move(operand);
			if (expr->wtype != awst::WType::bytesType())
			{
				auto toBytes = std::make_shared<awst::ReinterpretCast>();
				toBytes->sourceLocation = _loc;
				toBytes->wtype = awst::WType::bytesType();
				toBytes->expr = std::move(expr);
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
			auto itob = std::make_shared<awst::IntrinsicCall>();
			itob->sourceLocation = _loc;
			itob->wtype = awst::WType::bytesType();
			itob->opCode = "itob";
			itob->stackArgs.push_back(std::move(operand));

			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(itob);
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
			// Coerce both sides to the same wtype (bytes) if they differ
			auto coerceToBytes = [&](std::shared_ptr<awst::Expression>& expr) {
				if (expr->wtype != awst::WType::bytesType()
					&& expr->wtype != awst::WType::accountType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = _loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(expr);
					expr = std::move(cast);
				}
			};
			if (_left->wtype != _right->wtype)
			{
				coerceToBytes(_left);
				coerceToBytes(_right);
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

	// Arithmetic/bitwise operations — choose uint64 vs biguint
	if (isBigUInt(_resultType) || isBigUInt(_left->wtype) || isBigUInt(_right->wtype))
	{
		promoteToBigUInt(_left);
		promoteToBigUInt(_right);

		auto e = std::make_shared<awst::BigUIntBinaryOperation>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::biguintType();
		e->left = std::move(_left);
		e->right = std::move(_right);

		switch (_op)
		{
		case Token::Add: case Token::AssignAdd: e->op = awst::BigUIntBinaryOperator::Add; break;
		case Token::Sub: case Token::AssignSub: e->op = awst::BigUIntBinaryOperator::Sub; break;
		case Token::Mul: case Token::AssignMul: e->op = awst::BigUIntBinaryOperator::Mult; break;
		case Token::Div: case Token::AssignDiv: e->op = awst::BigUIntBinaryOperator::FloorDiv; break;
		case Token::Mod: case Token::AssignMod: e->op = awst::BigUIntBinaryOperator::Mod; break;
		case Token::BitOr: case Token::AssignBitOr: e->op = awst::BigUIntBinaryOperator::BitOr; break;
		case Token::BitXor: case Token::AssignBitXor: e->op = awst::BigUIntBinaryOperator::BitXor; break;
		case Token::BitAnd: case Token::AssignBitAnd: e->op = awst::BigUIntBinaryOperator::BitAnd; break;
		default: e->op = awst::BigUIntBinaryOperator::Add; break;
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
		case Token::Exp: e->op = awst::UInt64BinaryOperator::Pow; break;
		case Token::SHL: case Token::AssignShl: e->op = awst::UInt64BinaryOperator::LShift; break;
		case Token::SHR: case Token::AssignShr: e->op = awst::UInt64BinaryOperator::RShift; break;
		case Token::BitOr: case Token::AssignBitOr: e->op = awst::UInt64BinaryOperator::BitOr; break;
		case Token::BitXor: case Token::AssignBitXor: e->op = awst::UInt64BinaryOperator::BitXor; break;
		case Token::BitAnd: case Token::AssignBitAnd: e->op = awst::UInt64BinaryOperator::BitAnd; break;
		default: e->op = awst::UInt64BinaryOperator::Add; break;
		}
		return e;
	}
}

bool ExpressionTranslator::visit(solidity::frontend::BinaryOperation const& _node)
{
	auto loc = makeLoc(_node.location());
	auto left = translate(_node.leftExpression());
	auto right = translate(_node.rightExpression());
	auto* resultType = m_typeMapper.map(_node.annotation().type);

	push(buildBinaryOp(_node.getOperator(), std::move(left), std::move(right), resultType, loc));
	return false;
}

bool ExpressionTranslator::visit(solidity::frontend::UnaryOperation const& _node)
{
	auto loc = makeLoc(_node.location());
	auto operand = translate(_node.subExpression());

	using Token = solidity::frontend::Token;

	switch (_node.getOperator())
	{
	case Token::Not:
	{
		auto e = std::make_shared<awst::Not>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::boolType();
		e->expr = std::move(operand);
		push(e);
		break;
	}
	case Token::Sub:
	{
		// Unary minus: 0 - operand
		auto* zeroWtype = operand->wtype;
		if (zeroWtype != awst::WType::uint64Type() && zeroWtype != awst::WType::biguintType())
			zeroWtype = awst::WType::uint64Type();
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = loc;
		zero->wtype = zeroWtype;
		zero->value = "0";

		if (isBigUInt(operand->wtype))
		{
			auto e = std::make_shared<awst::BigUIntBinaryOperation>();
			e->sourceLocation = loc;
			e->wtype = awst::WType::biguintType();
			e->left = std::move(zero);
			e->op = awst::BigUIntBinaryOperator::Sub;
			e->right = std::move(operand);
			push(e);
		}
		else
		{
			auto e = std::make_shared<awst::UInt64BinaryOperation>();
			e->sourceLocation = loc;
			e->wtype = awst::WType::uint64Type();
			e->left = std::move(zero);
			e->op = awst::UInt64BinaryOperator::Sub;
			e->right = std::move(operand);
			push(e);
		}
		break;
	}
	case Token::Inc:
	case Token::Dec:
	{
		// i++ / i-- / ++i / --i → assignment expression: i = i + 1 (or i - 1)
		// The result is the new value (pre-increment semantics for simplicity)
		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = loc;
		one->wtype = operand->wtype;
		one->value = "1";

		std::shared_ptr<awst::Expression> newValue;
		auto op = (_node.getOperator() == Token::Inc);
		if (isBigUInt(operand->wtype))
		{
			auto binOp = std::make_shared<awst::BigUIntBinaryOperation>();
			binOp->sourceLocation = loc;
			binOp->wtype = awst::WType::biguintType();
			binOp->left = operand; // shared, will also be used as target
			binOp->op = op ? awst::BigUIntBinaryOperator::Add : awst::BigUIntBinaryOperator::Sub;
			binOp->right = std::move(one);
			newValue = std::move(binOp);
		}
		else
		{
			auto binOp = std::make_shared<awst::UInt64BinaryOperation>();
			binOp->sourceLocation = loc;
			binOp->wtype = awst::WType::uint64Type();
			binOp->left = operand;
			binOp->op = op ? awst::UInt64BinaryOperator::Add : awst::UInt64BinaryOperator::Sub;
			binOp->right = std::move(one);
			newValue = std::move(binOp);
		}

		// Create assignment expression: i = i +/- 1
		auto assignExpr = std::make_shared<awst::AssignmentExpression>();
		assignExpr->sourceLocation = loc;
		assignExpr->wtype = operand->wtype;
		// Re-translate the sub-expression to get a fresh target
		auto target = translate(_node.subExpression());
		assignExpr->target = std::move(target);
		assignExpr->value = std::move(newValue);
		push(assignExpr);
		break;
	}
	default:
	{
		// Fallback: just pass through the operand
		push(std::move(operand));
		break;
	}
	}

	return false;
}

std::shared_ptr<awst::Expression> ExpressionTranslator::translateRequire(
	solidity::frontend::FunctionCall const& _call,
	awst::SourceLocation const& _loc
)
{
	auto const& args = _call.arguments();
	std::shared_ptr<awst::Expression> condition;
	std::optional<std::string> message;

	if (!args.empty())
		condition = translate(*args[0]);

	if (args.size() > 1)
	{
		auto msgExpr = translate(*args[1]);
		if (auto const* sc = dynamic_cast<awst::StringConstant const*>(msgExpr.get()))
			message = sc->value;
		else
			message = "assertion failed";
	}

	return IntrinsicMapper::createAssert(std::move(condition), std::move(message), _loc);
}

bool ExpressionTranslator::visit(solidity::frontend::FunctionCall const& _node)
{
	using namespace solidity::frontend;

	auto loc = makeLoc(_node.location());
	auto const& rawFuncExpr = _node.expression();

	// Unwrap FunctionCallOptions ({value: X, gas: Y}) to get the actual function expression.
	// Extract the "value" option for use in inner transaction construction.
	solidity::frontend::Expression const* funcExprPtr = &rawFuncExpr;
	std::shared_ptr<awst::Expression> callValueAmount;
	if (auto const* callOpts = dynamic_cast<FunctionCallOptions const*>(&rawFuncExpr))
	{
		funcExprPtr = &callOpts->expression();
		auto const& optNames = callOpts->names();
		auto optValues = callOpts->options();
		for (size_t i = 0; i < optNames.size(); ++i)
		{
			if (*optNames[i] == "value" && i < optValues.size())
			{
				callValueAmount = translate(*optValues[i]);
				callValueAmount = implicitNumericCast(
					std::move(callValueAmount), awst::WType::uint64Type(), loc
				);
				break;
			}
		}
	}
	auto const& funcExpr = *funcExprPtr;

	// Handle type conversions
	if (*_node.annotation().kind == FunctionCallKind::TypeConversion)
	{
		if (!_node.arguments().empty())
		{
			auto* targetType = m_typeMapper.map(_node.annotation().type);

			// Special case: address(0) → zero-address constant
			if (targetType == awst::WType::accountType())
			{
				auto const& arg = *_node.arguments()[0];
				if (auto const* lit = dynamic_cast<solidity::frontend::Literal const*>(&arg))
				{
					if (lit->value() == "0")
					{
						// Create 32-byte zero address
						auto e = std::make_shared<awst::AddressConstant>();
						e->sourceLocation = loc;
						e->wtype = awst::WType::accountType();
						e->value = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ";
						push(e);
						return false;
					}
				}
			}

			auto converted = translate(*_node.arguments()[0]);
			// Apply type conversion — try implicit numeric promotion first
			converted = implicitNumericCast(std::move(converted), targetType, loc);
			if (targetType != converted->wtype)
			{
				bool sourceIsBytes = converted->wtype
					&& converted->wtype->kind() == awst::WTypeKind::Bytes;
				bool targetIsUint = targetType == awst::WType::uint64Type();
				bool targetIsBiguint = targetType == awst::WType::biguintType();
				bool sourceIsUint = converted->wtype == awst::WType::uint64Type();
				bool targetIsBytes = targetType
					&& targetType->kind() == awst::WTypeKind::Bytes;

				if (sourceIsBytes && targetIsUint)
				{
					// bytes[N] → uint64: reinterpret to bytes then btoi
					auto expr = std::move(converted);
					if (expr->wtype != awst::WType::bytesType())
					{
						auto toBytes = std::make_shared<awst::ReinterpretCast>();
						toBytes->sourceLocation = loc;
						toBytes->wtype = awst::WType::bytesType();
						toBytes->expr = std::move(expr);
						expr = std::move(toBytes);
					}
					auto btoi = std::make_shared<awst::IntrinsicCall>();
					btoi->sourceLocation = loc;
					btoi->wtype = awst::WType::uint64Type();
					btoi->opCode = "btoi";
					btoi->stackArgs.push_back(std::move(expr));
					push(std::move(btoi));
				}
				else if (sourceIsBytes && targetIsBiguint)
				{
					// bytes[N] → biguint: reinterpret to bytes then to biguint
					auto expr = std::move(converted);
					if (expr->wtype != awst::WType::bytesType())
					{
						auto toBytes = std::make_shared<awst::ReinterpretCast>();
						toBytes->sourceLocation = loc;
						toBytes->wtype = awst::WType::bytesType();
						toBytes->expr = std::move(expr);
						expr = std::move(toBytes);
					}
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::biguintType();
					cast->expr = std::move(expr);
					push(std::move(cast));
				}
				else if (sourceIsUint && targetIsBytes)
				{
					// uint64 → bytes[N]: itob then reinterpret
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = loc;
					itob->wtype = awst::WType::bytesType();
					itob->opCode = "itob";
					itob->stackArgs.push_back(std::move(converted));
					if (targetType != awst::WType::bytesType())
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = targetType;
						cast->expr = std::move(itob);
						push(std::move(cast));
					}
					else
						push(std::move(itob));
				}
				else
				{
					// Same scalar type: safe to ReinterpretCast
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = targetType;
					cast->expr = std::move(converted);
					push(std::move(cast));
				}
			}
			else
				push(std::move(converted));
		}
		return false;
	}

	// Handle struct creation
	if (*_node.annotation().kind == FunctionCallKind::StructConstructorCall)
	{
		auto* solType = _node.annotation().type;
		auto* wtype = m_typeMapper.map(solType);

		auto structExpr = std::make_shared<awst::NamedTupleExpression>();
		structExpr->sourceLocation = loc;
		structExpr->wtype = wtype;

		auto const& names = _node.names();
		auto const& args = _node.arguments();

		auto const* tupleType = dynamic_cast<awst::WTuple const*>(wtype);
		if (!names.empty())
		{
			for (size_t i = 0; i < names.size(); ++i)
			{
				auto val = translate(*args[i]);
				if (tupleType && i < tupleType->types().size())
					val = implicitNumericCast(std::move(val), tupleType->types()[i], loc);
				structExpr->values[*names[i]] = std::move(val);
			}
		}
		else
		{
			// Positional args
			if (auto const* structType = dynamic_cast<StructType const*>(solType))
			{
				auto const& members = structType->structDefinition().members();
				for (size_t i = 0; i < args.size() && i < members.size(); ++i)
				{
					auto val = translate(*args[i]);
					if (tupleType && i < tupleType->types().size())
						val = implicitNumericCast(std::move(val), tupleType->types()[i], loc);
					structExpr->values[members[i]->name()] = std::move(val);
				}
			}
		}

		push(structExpr);
		return false;
	}

	// Handle specific function calls
	if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr))
	{
		std::string memberName = memberAccess->memberName();

		auto const& baseExpr = memberAccess->expression();

		// Handle .call{value: X}("") → payment inner transaction
		if (memberName == "call" && callValueAmount)
		{
			auto receiver = translate(baseExpr);

			std::map<std::string, std::shared_ptr<awst::Expression>> fields;
			fields["Receiver"] = std::move(receiver);
			fields["Amount"] = std::move(callValueAmount);

			auto create = buildCreateInnerTransaction(TxnTypePay, std::move(fields), loc);

			// addr.call{value: X}("") returns (bool, bytes)
			// Emit submit as pending statement, return (true, empty_bytes)
			auto* submitWtype = m_typeMapper.createType<awst::WInnerTransaction>(TxnTypePay);
			auto submit = std::make_shared<awst::SubmitInnerTransaction>();
			submit->sourceLocation = loc;
			submit->wtype = submitWtype;
			submit->itxns.push_back(std::move(create));

			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = loc;
			stmt->expr = std::move(submit);
			m_pendingStatements.push_back(std::move(stmt));

			auto trueLit = std::make_shared<awst::BoolConstant>();
			trueLit->sourceLocation = loc;
			trueLit->wtype = awst::WType::boolType();
			trueLit->value = true;

			auto emptyBytes = std::make_shared<awst::BytesConstant>();
			emptyBytes->sourceLocation = loc;
			emptyBytes->wtype = awst::WType::bytesType();
			emptyBytes->encoding = awst::BytesEncoding::Base16;
			emptyBytes->value = {};

			auto tuple = std::make_shared<awst::TupleExpression>();
			tuple->sourceLocation = loc;
			auto* tupleWtype = m_typeMapper.createType<awst::WTuple>(
				std::vector<awst::WType const*>{awst::WType::boolType(), awst::WType::bytesType()}
			);
			tuple->wtype = tupleWtype;
			tuple->items.push_back(std::move(trueLit));
			tuple->items.push_back(std::move(emptyBytes));

			push(tuple);
			return false;
		}

		// Handle address.transfer / address.send → payment inner transaction
		if (memberName == "transfer" || memberName == "send")
		{
			auto const* baseType = baseExpr.annotation().type;
			if (baseType
				&& baseType->category() == solidity::frontend::Type::Category::Address
				&& _node.arguments().size() == 1)
			{
				auto receiver = translate(baseExpr);
				auto amount = translate(*_node.arguments()[0]);
				amount = implicitNumericCast(
					std::move(amount), awst::WType::uint64Type(), loc
				);

				std::map<std::string, std::shared_ptr<awst::Expression>> fields;
				fields["Receiver"] = std::move(receiver);
				fields["Amount"] = std::move(amount);

				auto create = buildCreateInnerTransaction(
					TxnTypePay, std::move(fields), loc
				);
				// .transfer() returns void (reverts on failure)
				// .send() returns bool (on Algorand, always true if we reach here)
				auto* retType = (memberName == "send")
					? awst::WType::boolType()
					: awst::WType::voidType();
				push(buildSubmitAndReturn(
					std::move(create), retType, loc
				));
				return false;
			}
		}

		// push/pop on arrays
		if (memberName == "push" || memberName == "pop" || memberName == "length")
		{
			// Check if this is a box-stored dynamic array state variable
			bool isBoxArray = false;
			std::string arrayVarName;
			if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpr))
			{
				if (auto const* varDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
					ident->annotation().referencedDeclaration))
				{
					if (varDecl->isStateVariable() && StorageMapper::shouldUseBoxStorage(*varDecl)
						&& dynamic_cast<solidity::frontend::ArrayType const*>(varDecl->type()))
					{
						isBoxArray = true;
						arrayVarName = ident->name();
					}
				}
			}

			if (isBoxArray)
			{
				// Box-mapped dynamic array operations
				if (memberName == "length")
				{
					push(m_storageMapper.createStateRead(
						arrayVarName + "_length", awst::WType::uint64Type(),
						awst::AppStorageKind::AppGlobal, loc
					));
				}
				else if (memberName == "push" && !_node.arguments().empty())
				{
					auto val = translate(*_node.arguments()[0]);

					// Read current length
					auto lenRead = m_storageMapper.createStateRead(
						arrayVarName + "_length", awst::WType::uint64Type(),
						awst::AppStorageKind::AppGlobal, loc
					);

					// Build box key: prefix + itob(length)
					auto prefix = std::make_shared<awst::BytesConstant>();
					prefix->sourceLocation = loc;
					prefix->wtype = awst::WType::boxKeyType();
					prefix->encoding = awst::BytesEncoding::Utf8;
					prefix->value = std::vector<uint8_t>(arrayVarName.begin(), arrayVarName.end());

					auto lenToBytes = std::make_shared<awst::ReinterpretCast>();
					lenToBytes->sourceLocation = loc;
					lenToBytes->wtype = awst::WType::bytesType();
					lenToBytes->expr = lenRead;

					auto boxKey = std::make_shared<awst::BoxPrefixedKeyExpression>();
					boxKey->sourceLocation = loc;
					boxKey->wtype = awst::WType::boxKeyType();
					boxKey->prefix = prefix;
					boxKey->key = std::move(lenToBytes);

					// Write value to box
					auto boxVal = std::make_shared<awst::BoxValueExpression>();
					boxVal->sourceLocation = loc;
					boxVal->wtype = val->wtype;
					boxVal->key = std::move(boxKey);

					auto assign = std::make_shared<awst::AssignmentExpression>();
					assign->sourceLocation = loc;
					assign->wtype = val->wtype;
					assign->target = boxVal;
					assign->value = std::move(val);

					push(assign);

					// Increment length: length = length + 1 (as pending statement)
					auto one = std::make_shared<awst::IntegerConstant>();
					one->sourceLocation = loc;
					one->wtype = awst::WType::uint64Type();
					one->value = "1";

					auto lenRead2 = m_storageMapper.createStateRead(
						arrayVarName + "_length", awst::WType::uint64Type(),
						awst::AppStorageKind::AppGlobal, loc
					);

					auto addOne = std::make_shared<awst::UInt64BinaryOperation>();
					addOne->sourceLocation = loc;
					addOne->wtype = awst::WType::uint64Type();
					addOne->left = std::move(lenRead2);
					addOne->right = std::move(one);
					addOne->op = awst::UInt64BinaryOperator::Add;

					auto lenWrite = m_storageMapper.createStateWrite(
						arrayVarName + "_length", std::move(addOne),
						awst::WType::uint64Type(),
						awst::AppStorageKind::AppGlobal, loc
					);

					auto lenStmt = std::make_shared<awst::ExpressionStatement>();
					lenStmt->sourceLocation = loc;
					lenStmt->expr = std::move(lenWrite);
					m_pendingStatements.push_back(std::move(lenStmt));
				}
				else if (memberName == "pop")
				{
					// Decrement length
					auto one = std::make_shared<awst::IntegerConstant>();
					one->sourceLocation = loc;
					one->wtype = awst::WType::uint64Type();
					one->value = "1";

					auto lenRead = m_storageMapper.createStateRead(
						arrayVarName + "_length", awst::WType::uint64Type(),
						awst::AppStorageKind::AppGlobal, loc
					);

					auto subOne = std::make_shared<awst::UInt64BinaryOperation>();
					subOne->sourceLocation = loc;
					subOne->wtype = awst::WType::uint64Type();
					subOne->left = std::move(lenRead);
					subOne->right = std::move(one);
					subOne->op = awst::UInt64BinaryOperator::Sub;

					push(m_storageMapper.createStateWrite(
						arrayVarName + "_length", std::move(subOne),
						awst::WType::uint64Type(),
						awst::AppStorageKind::AppGlobal, loc
					));
				}
			}
			else
			{
				auto base = translate(baseExpr);
				if (memberName == "length")
				{
					auto e = std::make_shared<awst::ArrayLength>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::uint64Type();
					e->array = std::move(base);
					push(e);
				}
				else if (memberName == "push" && !_node.arguments().empty())
				{
					// array.push(val) — use ArrayExtend (mutates in place, returns void)
					auto val = translate(*_node.arguments()[0]);
					auto* baseWtype = base->wtype;

					// Wrap val in a single-element array
					auto singleArr = std::make_shared<awst::NewArray>();
					singleArr->sourceLocation = loc;
					singleArr->wtype = baseWtype;
					singleArr->values.push_back(std::move(val));

					auto e = std::make_shared<awst::ArrayExtend>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::voidType();
					e->base = std::move(base);
					e->other = std::move(singleArr);

					push(e);
				}
				else if (memberName == "pop")
				{
					auto e = std::make_shared<awst::ArrayPop>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::voidType();
					e->base = std::move(base);
					push(e);
				}
			}
			return false;
		}

		// Handle ERC20 token calls → ASA inner transactions / intrinsics
		if (memberName == "transfer" || memberName == "transferFrom"
			|| memberName == "approve" || memberName == "allowance"
			|| memberName == "balanceOf")
		{
			// Resolve the token's ASA ID from the base expression's state variable
			std::string tokenVarName;
			if (auto const* ident =
					dynamic_cast<solidity::frontend::Identifier const*>(&baseExpr))
				tokenVarName = ident->name();

			auto resolveAssetId = [&]() -> std::shared_ptr<awst::Expression> {
				if (!tokenVarName.empty())
					return m_storageMapper.createStateRead(
						tokenVarName, awst::WType::assetType(),
						awst::AppStorageKind::AppGlobal, loc
					);
				Logger::instance().warning(
					"could not resolve token variable for " + memberName, loc
				);
				return makeUint64("0", loc);
			};

			if (memberName == "transfer" && _node.arguments().size() == 2)
			{
				// token.transfer(to, amount) → axfer inner txn
				auto to = translate(*_node.arguments()[0]);
				auto amount = translate(*_node.arguments()[1]);
				amount = implicitNumericCast(
					std::move(amount), awst::WType::uint64Type(), loc
				);

				std::map<std::string, std::shared_ptr<awst::Expression>> fields;
				fields["AssetReceiver"] = std::move(to);
				fields["AssetAmount"] = std::move(amount);
				fields["XferAsset"] = resolveAssetId();

				auto create = buildCreateInnerTransaction(
					TxnTypeAxfer, std::move(fields), loc
				);
				push(buildSubmitAndReturn(
					std::move(create), awst::WType::boolType(), loc
				));
				return false;
			}
			else if (memberName == "transferFrom" && _node.arguments().size() == 3)
			{
				// token.transferFrom(from, to, amount) → axfer with AssetSender
				auto from = translate(*_node.arguments()[0]);
				auto to = translate(*_node.arguments()[1]);
				auto amount = translate(*_node.arguments()[2]);
				amount = implicitNumericCast(
					std::move(amount), awst::WType::uint64Type(), loc
				);

				std::map<std::string, std::shared_ptr<awst::Expression>> fields;
				fields["AssetSender"] = std::move(from);
				fields["AssetReceiver"] = std::move(to);
				fields["AssetAmount"] = std::move(amount);
				fields["XferAsset"] = resolveAssetId();

				auto create = buildCreateInnerTransaction(
					TxnTypeAxfer, std::move(fields), loc
				);
				push(buildSubmitAndReturn(
					std::move(create), awst::WType::boolType(), loc
				));
				return false;
			}
			else if (memberName == "balanceOf" && _node.arguments().size() == 1)
			{
				// token.balanceOf(addr) → asset_holding_get AssetBalance intrinsic
				auto addr = translate(*_node.arguments()[0]);

				// asset_holding_get returns (value, did_exist) — use CheckedMaybe
				auto* tupleWtype = m_typeMapper.createType<awst::WTuple>(
					std::vector<awst::WType const*>{
						awst::WType::uint64Type(), awst::WType::boolType()
					}
				);

				auto call = std::make_shared<awst::IntrinsicCall>();
				call->sourceLocation = loc;
				call->wtype = tupleWtype;
				call->opCode = "asset_holding_get";
				call->immediates.push_back(std::string("AssetBalance"));
				call->stackArgs.push_back(std::move(addr));
				call->stackArgs.push_back(resolveAssetId());

				auto checked = std::make_shared<awst::CheckedMaybe>();
				checked->sourceLocation = loc;
				checked->wtype = awst::WType::uint64Type();
				checked->expr = std::move(call);
				checked->comment = "account is opted in to asset";

				push(checked);
				return false;
			}
			else if (memberName == "approve" || memberName == "allowance")
			{
				// No Algorand equivalent — keep stub with warning
				Logger::instance().warning(
					"ERC20 '" + memberName
						+ "' has no Algorand equivalent, returning stub value",
					loc
				);
				auto* resultType = m_typeMapper.map(_node.annotation().type);
				if (resultType == awst::WType::boolType())
				{
					auto e = std::make_shared<awst::BoolConstant>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::boolType();
					e->value = true;
					push(e);
				}
				else
				{
					auto e = std::make_shared<awst::IntegerConstant>();
					e->sourceLocation = loc;
					e->wtype = resultType ? resultType : awst::WType::uint64Type();
					e->value = "0";
					push(e);
				}
				return false;
			}
		}
	}

	// Check for require/assert
	if (auto const* identifier = dynamic_cast<Identifier const*>(&funcExpr))
	{
		std::string name = identifier->name();

		if (name == "require" || name == "assert")
		{
			push(translateRequire(_node, loc));
			return false;
		}

		if (name == "revert")
		{
			auto assertExpr = std::make_shared<awst::AssertExpression>();
			assertExpr->sourceLocation = loc;
			assertExpr->wtype = awst::WType::voidType();
			auto falseLit = std::make_shared<awst::BoolConstant>();
			falseLit->sourceLocation = loc;
			falseLit->wtype = awst::WType::boolType();
			falseLit->value = false;
			assertExpr->condition = std::move(falseLit);
			if (!_node.arguments().empty())
			{
				if (auto const* lit = dynamic_cast<Literal const*>(_node.arguments()[0].get()))
					assertExpr->errorMessage = lit->value();
				else
					assertExpr->errorMessage = "revert";
			}
			else
				assertExpr->errorMessage = "revert";
			push(assertExpr);
			return false;
		}

		if (name == "keccak256")
		{
			auto call = std::make_shared<awst::IntrinsicCall>();
			call->sourceLocation = loc;
			call->opCode = "keccak256";
			call->wtype = awst::WType::bytesType();
			for (auto const& arg: _node.arguments())
				call->stackArgs.push_back(translate(*arg));
			push(call);
			return false;
		}

		// mulmod(x, y, z) → (x * y) % z using biguint full precision
		if (name == "mulmod" && _node.arguments().size() == 3)
		{
			auto x = translate(*_node.arguments()[0]);
			auto y = translate(*_node.arguments()[1]);
			auto z = translate(*_node.arguments()[2]);

			// Promote all to biguint if needed
			auto promoteToBigUInt = [&](std::shared_ptr<awst::Expression>& operand)
			{
				if (operand->wtype == awst::WType::uint64Type())
				{
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = loc;
					itob->wtype = awst::WType::bytesType();
					itob->opCode = "itob";
					itob->stackArgs.push_back(std::move(operand));
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::biguintType();
					cast->expr = std::move(itob);
					operand = std::move(cast);
				}
			};
			promoteToBigUInt(x);
			promoteToBigUInt(y);
			promoteToBigUInt(z);

			// x * y
			auto mul = std::make_shared<awst::BigUIntBinaryOperation>();
			mul->sourceLocation = loc;
			mul->wtype = awst::WType::biguintType();
			mul->left = std::move(x);
			mul->right = std::move(y);
			mul->op = awst::BigUIntBinaryOperator::Mult;

			// (x * y) % z
			auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
			mod->sourceLocation = loc;
			mod->wtype = awst::WType::biguintType();
			mod->left = std::move(mul);
			mod->right = std::move(z);
			mod->op = awst::BigUIntBinaryOperator::Mod;

			push(mod);
			return false;
		}

		// addmod(x, y, z) → (x + y) % z using biguint full precision
		if (name == "addmod" && _node.arguments().size() == 3)
		{
			auto x = translate(*_node.arguments()[0]);
			auto y = translate(*_node.arguments()[1]);
			auto z = translate(*_node.arguments()[2]);

			auto promoteToBigUInt = [&](std::shared_ptr<awst::Expression>& operand)
			{
				if (operand->wtype == awst::WType::uint64Type())
				{
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = loc;
					itob->wtype = awst::WType::bytesType();
					itob->opCode = "itob";
					itob->stackArgs.push_back(std::move(operand));
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::biguintType();
					cast->expr = std::move(itob);
					operand = std::move(cast);
				}
			};
			promoteToBigUInt(x);
			promoteToBigUInt(y);
			promoteToBigUInt(z);

			auto add = std::make_shared<awst::BigUIntBinaryOperation>();
			add->sourceLocation = loc;
			add->wtype = awst::WType::biguintType();
			add->left = std::move(x);
			add->right = std::move(y);
			add->op = awst::BigUIntBinaryOperator::Add;

			auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
			mod->sourceLocation = loc;
			mod->wtype = awst::WType::biguintType();
			mod->left = std::move(add);
			mod->right = std::move(z);
			mod->op = awst::BigUIntBinaryOperator::Mod;

			push(mod);
			return false;
		}

		// gasleft() → 0 (no equivalent on Algorand)
		if (name == "gasleft")
		{
			Logger::instance().warning("gasleft() has no Algorand equivalent, returning 0", loc);
			auto e = std::make_shared<awst::IntegerConstant>();
			e->sourceLocation = loc;
			e->wtype = awst::WType::biguintType();
			e->value = "0";
			push(e);
			return false;
		}
	}

	// Handle `new` expressions: new bytes(N), new T[](N)
	if (dynamic_cast<NewExpression const*>(&funcExpr))
	{
		auto* resultType = m_typeMapper.map(_node.annotation().type);
		if (resultType && resultType->kind() == awst::WTypeKind::Bytes)
		{
			// new bytes(N) → bzero(N) intrinsic
			auto sizeExpr = !_node.arguments().empty()
				? translate(*_node.arguments()[0])
				: nullptr;
			if (sizeExpr)
				sizeExpr = implicitNumericCast(std::move(sizeExpr), awst::WType::uint64Type(), loc);

			auto e = std::make_shared<awst::IntrinsicCall>();
			e->sourceLocation = loc;
			e->wtype = resultType;
			e->opCode = "bzero";
			if (sizeExpr)
				e->stackArgs.push_back(std::move(sizeExpr));
			push(e);
			return false;
		}
		else if (resultType && resultType->kind() == awst::WTypeKind::ReferenceArray)
		{
			// new T[](N) → empty NewArray (elements added via index assignment)
			auto e = std::make_shared<awst::NewArray>();
			e->sourceLocation = loc;
			e->wtype = resultType;
			push(e);
			return false;
		}
	}

	// Handle abi.encodePacked(...) and abi.encodeCall(...)
	if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr))
	{
		auto const* baseType = memberAccess->expression().annotation().type;
		if (auto const* magicType = dynamic_cast<MagicType const*>(baseType))
		{
			std::string memberName = memberAccess->memberName();

			// abi.encodePacked(...) → chain of concat intrinsics
			if (memberName == "encodePacked" || memberName == "encode")
			{
				auto const& args = _node.arguments();
				if (args.empty())
				{
					auto e = std::make_shared<awst::BytesConstant>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::bytesType();
					e->encoding = awst::BytesEncoding::Base16;
					e->value = {};
					push(e);
					return false;
				}

				// Helper: convert an expression to bytes
				auto toBytes = [&](std::shared_ptr<awst::Expression> expr) -> std::shared_ptr<awst::Expression> {
					if (expr->wtype == awst::WType::bytesType())
						return expr;
					if (expr->wtype == awst::WType::stringType()
						|| (expr->wtype && expr->wtype->kind() == awst::WTypeKind::Bytes))
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(expr);
						return cast;
					}
					if (expr->wtype == awst::WType::uint64Type())
					{
						auto itob = std::make_shared<awst::IntrinsicCall>();
						itob->sourceLocation = loc;
						itob->wtype = awst::WType::bytesType();
						itob->opCode = "itob";
						itob->stackArgs.push_back(std::move(expr));
						return itob;
					}
					if (expr->wtype == awst::WType::biguintType())
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(expr);
						return cast;
					}
					if (expr->wtype == awst::WType::accountType())
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(expr);
						return cast;
					}
					// Fallback: reinterpret as bytes
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(expr);
					return cast;
				};

				auto result = toBytes(translate(*args[0]));
				for (size_t i = 1; i < args.size(); ++i)
				{
					auto arg = toBytes(translate(*args[i]));
					auto concat = std::make_shared<awst::IntrinsicCall>();
					concat->sourceLocation = loc;
					concat->wtype = awst::WType::bytesType();
					concat->opCode = "concat";
					concat->stackArgs.push_back(std::move(result));
					concat->stackArgs.push_back(std::move(arg));
					result = std::move(concat);
				}
				push(std::move(result));
				return false;
			}

			// abi.encodeCall(fn, (args)) → empty bytes placeholder
			// EVM calldata encoding has no Algorand equivalent
			if (memberName == "encodeCall")
			{
				Logger::instance().warning("abi.encodeCall() not supported on Algorand, returning empty bytes", loc);
				auto e = std::make_shared<awst::BytesConstant>();
				e->sourceLocation = loc;
				e->wtype = awst::WType::bytesType();
				e->encoding = awst::BytesEncoding::Base16;
				e->value = {};
				push(e);
				return false;
			}

			// abi.decode — pass through first argument, cast to target type
			if (memberName == "decode")
			{
				auto* targetType = m_typeMapper.map(_node.annotation().type);
				if (!_node.arguments().empty())
				{
					auto decoded = translate(*_node.arguments()[0]);
					if (targetType && decoded->wtype != targetType)
					{
						// For scalar types, use ReinterpretCast
						if (targetType->kind() != awst::WTypeKind::WTuple)
						{
							auto cast = std::make_shared<awst::ReinterpretCast>();
							cast->sourceLocation = loc;
							cast->wtype = targetType;
							cast->expr = std::move(decoded);
							push(cast);
						}
						else if (auto const* tupleType = dynamic_cast<awst::WTuple const*>(targetType))
						{
							// For struct/tuple types, create default-constructed tuple
							// (full ABI decoding not yet supported)
							auto tuple = std::make_shared<awst::TupleExpression>();
							tuple->sourceLocation = loc;
							tuple->wtype = targetType;
							for (auto const* fieldType: tupleType->types())
							{
								if (fieldType == awst::WType::boolType())
								{
									auto def = std::make_shared<awst::BoolConstant>();
									def->sourceLocation = loc;
									def->wtype = fieldType;
									def->value = false;
									tuple->items.push_back(std::move(def));
								}
								else if (fieldType == awst::WType::uint64Type()
									|| fieldType == awst::WType::biguintType())
								{
									auto def = std::make_shared<awst::IntegerConstant>();
									def->sourceLocation = loc;
									def->wtype = fieldType;
									def->value = "0";
									tuple->items.push_back(std::move(def));
								}
								else if (fieldType == awst::WType::stringType())
								{
									auto def = std::make_shared<awst::StringConstant>();
									def->sourceLocation = loc;
									def->wtype = fieldType;
									def->value = "";
									tuple->items.push_back(std::move(def));
								}
								else if (fieldType && fieldType->kind() == awst::WTypeKind::Bytes)
								{
									auto const* bytesT = dynamic_cast<awst::BytesWType const*>(fieldType);
									auto def = std::make_shared<awst::BytesConstant>();
									def->sourceLocation = loc;
									def->wtype = fieldType;
									def->encoding = awst::BytesEncoding::Base16;
									if (bytesT && bytesT->length())
										def->value = std::vector<uint8_t>(*bytesT->length(), 0);
									else
										def->value = {};
									tuple->items.push_back(std::move(def));
								}
								else if (fieldType && fieldType->kind() == awst::WTypeKind::ReferenceArray)
								{
									auto def = std::make_shared<awst::NewArray>();
									def->sourceLocation = loc;
									def->wtype = fieldType;
									tuple->items.push_back(std::move(def));
								}
								else if (fieldType == awst::WType::accountType())
								{
									auto def = std::make_shared<awst::AddressConstant>();
									def->sourceLocation = loc;
									def->wtype = fieldType;
									def->value = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ";
									tuple->items.push_back(std::move(def));
								}
								else
								{
									auto def = std::make_shared<awst::IntegerConstant>();
									def->sourceLocation = loc;
									def->wtype = fieldType;
									def->value = "0";
									tuple->items.push_back(std::move(def));
								}
							}
							push(tuple);
						}
					}
					else
						push(decoded);
				}
				else
				{
					auto e = std::make_shared<awst::BytesConstant>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::bytesType();
					e->encoding = awst::BytesEncoding::Base16;
					e->value = {};
					push(e);
				}
				return false;
			}
		}
	}

	// Generic function call → SubroutineCallExpression
	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = loc;
	bool isUsingDirectiveCall = false;

	if (auto const* identifier = dynamic_cast<Identifier const*>(&funcExpr))
	{
		std::string name = identifier->name();
		auto const* decl = identifier->annotation().referencedDeclaration;
		if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(decl))
		{
			call->wtype = m_typeMapper.map(funcDef->returnParameters().empty()
				? nullptr
				: funcDef->returnParameters()[0]->type());

			// Check if this function belongs to a library (e.g. calling within a library)
			bool resolvedAsLibrary = false;
			if (auto const* scope = funcDef->scope())
			{
				if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(scope))
				{
					if (contractDef->isLibrary())
					{
						std::string key = contractDef->name() + "." + funcDef->name();
						auto it = m_libraryFunctionIds.find(key);
						if (it == m_libraryFunctionIds.end())
						{
							// Try disambiguated overload name
							key += "(" + std::to_string(funcDef->parameters().size()) + ")";
							it = m_libraryFunctionIds.find(key);
						}
						if (it != m_libraryFunctionIds.end())
						{
							call->target = awst::SubroutineID{it->second};
							resolvedAsLibrary = true;
						}
					}
				}
			}
			if (!resolvedAsLibrary)
				call->target = awst::InstanceMethodTarget{resolveMethodName(*funcDef)};
		}
		else
		{
			call->wtype = m_typeMapper.map(_node.annotation().type);
			call->target = awst::InstanceMethodTarget{name};
		}
	}
	else if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr))
	{
		call->wtype = m_typeMapper.map(_node.annotation().type);

		// Check if the base expression references a library contract
		bool resolvedAsLibrary = false;
		auto const& baseExpr = memberAccess->expression();
		if (auto const* baseId = dynamic_cast<Identifier const*>(&baseExpr))
		{
			auto const* decl = baseId->annotation().referencedDeclaration;
			if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(decl))
			{
				if (contractDef->isLibrary())
				{
					std::string key = contractDef->name() + "." + memberAccess->memberName();
					auto it = m_libraryFunctionIds.find(key);
					if (it == m_libraryFunctionIds.end())
					{
						// Try disambiguated overload name
						key += "(" + std::to_string(_node.arguments().size()) + ")";
						it = m_libraryFunctionIds.find(key);
					}
					if (it != m_libraryFunctionIds.end())
					{
						call->target = awst::SubroutineID{it->second};
						resolvedAsLibrary = true;
					}
				}
			}
		}
		// Also check if the member's referenced declaration is a library function
		// (handles `using Library for Type` patterns like token.safeTransfer())
		if (!resolvedAsLibrary)
		{
			auto const* refDecl = memberAccess->annotation().referencedDeclaration;
			if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(refDecl))
			{
				if (auto const* scope = funcDef->scope())
				{
					if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(scope))
					{
						if (contractDef->isLibrary())
						{
							std::string key = contractDef->name() + "." + funcDef->name();
							auto it = m_libraryFunctionIds.find(key);
							if (it == m_libraryFunctionIds.end())
							{
								// Try disambiguated overload name
								key += "(" + std::to_string(funcDef->parameters().size()) + ")";
								it = m_libraryFunctionIds.find(key);
							}
							if (it != m_libraryFunctionIds.end())
							{
								call->target = awst::SubroutineID{it->second};
								resolvedAsLibrary = true;
								isUsingDirectiveCall = true;
							}
						}
					}
				}
			}
		}
		if (!resolvedAsLibrary)
		{
			// Check if base is an interface/contract type (external call)
			auto const* baseType = memberAccess->expression().annotation().type;
			bool isExternalCall = false;
			if (baseType && baseType->category() == solidity::frontend::Type::Category::Contract)
			{
				auto const* contractType = dynamic_cast<solidity::frontend::ContractType const*>(baseType);
				if (contractType && contractType->contractDefinition().isInterface())
					isExternalCall = true;
			}
			if (isExternalCall)
			{
				// External interface call → application call inner transaction
				auto baseTranslated = translate(memberAccess->expression());

				// Build ARC4 method selector string
				std::string methodSelector = memberAccess->memberName() + "(";
				auto const* extRefDecl = memberAccess->annotation().referencedDeclaration;
				if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(extRefDecl))
				{
					bool first = true;
					for (auto const& param: funcDef->parameters())
					{
						if (!first) methodSelector += ",";
						methodSelector += param->type()->toString(true);
						first = false;
					}
					methodSelector += ")";
					if (!funcDef->returnParameters().empty())
						methodSelector += funcDef->returnParameters()[0]->type()->toString(true);
					else
						methodSelector += "void";
				}
				else
					methodSelector += ")void";

				auto methodConst = std::make_shared<awst::MethodConstant>();
				methodConst->sourceLocation = loc;
				methodConst->wtype = awst::WType::bytesType();
				methodConst->value = methodSelector;

				std::map<std::string, std::shared_ptr<awst::Expression>> fields;
				fields["ApplicationID"] = std::move(baseTranslated);
				fields["OnCompletion"] = makeUint64("0", loc); // NoOp
				fields["ApplicationArgs"] = std::move(methodConst);

				auto create = buildCreateInnerTransaction(
					TxnTypeAppl, std::move(fields), loc
				);
				auto* retType = m_typeMapper.map(_node.annotation().type);
				push(buildSubmitAndReturn(std::move(create), retType, loc));
				return false;
			}
			// Resolve overloaded method names
			std::string methodName = memberAccess->memberName();
			auto const* refDecl = memberAccess->annotation().referencedDeclaration;
			if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(refDecl))
				methodName = resolveMethodName(*funcDef);
			call->target = awst::InstanceMethodTarget{methodName};
		}
	}
	else
	{
		Logger::instance().warning("could not resolve function call target", loc);
		call->wtype = m_typeMapper.map(_node.annotation().type);
		call->target = awst::InstanceMethodTarget{"unknown"};
	}

	// For `using Library for Type` calls, prepend the receiver as the first arg
	if (isUsingDirectiveCall)
	{
		if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr))
		{
			awst::CallArg ca;
			ca.value = translate(memberAccess->expression());
			call->args.push_back(std::move(ca));
		}
	}

	for (auto const& arg: _node.arguments())
	{
		awst::CallArg ca;
		ca.value = translate(*arg);
		call->args.push_back(std::move(ca));
	}

	push(call);
	return false;
}

bool ExpressionTranslator::visit(solidity::frontend::MemberAccess const& _node)
{
	auto loc = makeLoc(_node.location());
	std::string memberName = _node.memberName();

	auto const& baseExpr = _node.expression();

	// Try intrinsic mapping first (msg.sender, block.timestamp, etc.)
	if (auto const* baseId = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpr))
	{
		auto intrinsic = IntrinsicMapper::tryMapMemberAccess(baseId->name(), memberName, loc);
		if (intrinsic)
		{
			push(intrinsic);
			return false;
		}
	}

	// type(uintN).max / type(uintN).min → IntegerConstant
	if (memberName == "max" || memberName == "min")
	{
		solidity::frontend::IntegerType const* intType = nullptr;
		auto const* baseType = baseExpr.annotation().type;

		if (baseType)
		{
			// type(X) produces a MagicType with Kind::MetaType
			if (auto const* magicType = dynamic_cast<solidity::frontend::MagicType const*>(baseType))
			{
				if (auto const* arg = magicType->typeArgument())
					intType = dynamic_cast<solidity::frontend::IntegerType const*>(arg);
			}
			// Also handle TypeType (for completeness)
			else if (auto const* typeType = dynamic_cast<solidity::frontend::TypeType const*>(baseType))
			{
				intType = dynamic_cast<solidity::frontend::IntegerType const*>(typeType->actualType());
			}
		}

		if (intType)
		{
			unsigned bits = intType->numBits();
			auto* wtype = (bits <= 64)
				? awst::WType::uint64Type()
				: awst::WType::biguintType();

			auto e = std::make_shared<awst::IntegerConstant>();
			e->sourceLocation = loc;
			e->wtype = wtype;

			if (memberName == "max")
				e->value = computeMaxUint(bits);
			else
				e->value = "0"; // min for unsigned integers

			push(e);
			return false;
		}
	}

	// .length on arrays or bytes
	if (memberName == "length")
	{
		// Check if this is a box-stored dynamic array state variable
		if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpr))
		{
			if (auto const* varDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
			{
				if (varDecl->isStateVariable() && StorageMapper::shouldUseBoxStorage(*varDecl)
					&& dynamic_cast<solidity::frontend::ArrayType const*>(varDecl->type()))
				{
					// Read the _length global state variable
					std::string lengthVar = ident->name() + "_length";
					push(m_storageMapper.createStateRead(
						lengthVar, awst::WType::uint64Type(),
						awst::AppStorageKind::AppGlobal, loc
					));
					return false;
				}
			}
		}

		auto base = translate(baseExpr);
		// For bytes-typed expressions, use the AVM `len` intrinsic instead of ArrayLength
		if (base->wtype == awst::WType::bytesType())
		{
			auto e = std::make_shared<awst::IntrinsicCall>();
			e->sourceLocation = loc;
			e->wtype = awst::WType::uint64Type();
			e->opCode = "len";
			e->stackArgs.push_back(std::move(base));
			push(e);
			return false;
		}
		auto e = std::make_shared<awst::ArrayLength>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::uint64Type();
		e->array = std::move(base);
		push(e);
		return false;
	}

	// Library/contract constant access: inline the constant value
	auto const* refDecl = _node.annotation().referencedDeclaration;
	if (auto const* varDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(refDecl))
	{
		if (varDecl->isConstant() && varDecl->value())
		{
			push(translate(*varDecl->value()));
			return false;
		}
	}

	// Contract member access (e.g. token.transfer used as function selector in abi.encodeCall)
	auto const* baseType = baseExpr.annotation().type;
	if (baseType && baseType->category() == solidity::frontend::Type::Category::Contract)
	{
		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::bytesType();
		e->encoding = awst::BytesEncoding::Utf8;
		std::string sel = memberName;
		e->value = std::vector<uint8_t>(sel.begin(), sel.end());
		push(e);
		return false;
	}

	// Address property (.code, .balance, etc.)
	if (baseType && baseType->category() == solidity::frontend::Type::Category::Address)
	{
		Logger::instance().warning("address property '." + memberName + "' has no Algorand equivalent", loc);
		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::bytesType();
		e->encoding = awst::BytesEncoding::Base16;
		e->value = {};
		push(e);
		return false;
	}

	// type(Interface).interfaceId → bytes4 constant with actual ERC165 interface ID
	if (memberName == "interfaceId" && baseType
		&& baseType->category() == solidity::frontend::Type::Category::TypeType)
	{
		auto const* typeType = dynamic_cast<solidity::frontend::TypeType const*>(baseType);
		uint32_t interfaceIdValue = 0;
		if (typeType)
		{
			auto const* actualType = typeType->actualType();
			if (auto const* contractType = dynamic_cast<solidity::frontend::ContractType const*>(actualType))
				interfaceIdValue = contractType->contractDefinition().interfaceId();
		}
		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = loc;
		e->wtype = m_typeMapper.map(_node.annotation().type);
		e->encoding = awst::BytesEncoding::Base16;
		e->value = {
			static_cast<uint8_t>((interfaceIdValue >> 24) & 0xFF),
			static_cast<uint8_t>((interfaceIdValue >> 16) & 0xFF),
			static_cast<uint8_t>((interfaceIdValue >> 8) & 0xFF),
			static_cast<uint8_t>(interfaceIdValue & 0xFF)
		};
		push(e);
		return false;
	}

	// General field access — only valid for struct/tuple base types
	auto base = translate(baseExpr);
	if (base->wtype && base->wtype->kind() == awst::WTypeKind::WTuple)
	{
		auto e = std::make_shared<awst::FieldExpression>();
		e->sourceLocation = loc;
		e->base = std::move(base);
		e->name = memberName;
		e->wtype = m_typeMapper.map(_node.annotation().type);
		push(e);
		return false;
	}

	// Fallback: non-struct field access → emit mapped type default
	Logger::instance().warning("unsupported member access '." + memberName + "', returning default", loc);
	auto* wtype = m_typeMapper.map(_node.annotation().type);
	if (wtype == awst::WType::uint64Type() || wtype == awst::WType::biguintType())
	{
		auto e = std::make_shared<awst::IntegerConstant>();
		e->sourceLocation = loc;
		e->wtype = wtype;
		e->value = "0";
		push(e);
	}
	else if (wtype == awst::WType::boolType())
	{
		auto e = std::make_shared<awst::BoolConstant>();
		e->sourceLocation = loc;
		e->wtype = wtype;
		e->value = false;
		push(e);
	}
	else
	{
		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::bytesType();
		e->encoding = awst::BytesEncoding::Base16;
		e->value = {};
		push(e);
	}
	return false;
}

bool ExpressionTranslator::visit(solidity::frontend::IndexAccess const& _node)
{
	auto loc = makeLoc(_node.location());

	// If this is a mapping or dynamic array access, it becomes a box value read.
	// For nested mappings (e.g. _operatorApprovals[owner][operator]), walk up
	// the chain of IndexAccess nodes to find the root Identifier and collect all keys.
	auto const* baseType = _node.baseExpression().annotation().type;
	bool isDynamicArrayAccess = false;
	if (auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(baseType))
	{
		// Check if this is a state variable (box-stored dynamic array)
		if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&_node.baseExpression()))
		{
			if (auto const* varDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
			{
				if (varDecl->isStateVariable() && arrType->isDynamicallySized())
					isDynamicArrayAccess = true;
			}
		}
	}

	// Also detect nested mapping access: if base is an IndexAccess whose base type is a mapping
	bool isNestedMappingAccess = false;
	if (auto const* baseIndexAccess = dynamic_cast<solidity::frontend::IndexAccess const*>(&_node.baseExpression()))
	{
		auto const* innerBaseType = baseIndexAccess->baseExpression().annotation().type;
		if (innerBaseType && innerBaseType->category() == solidity::frontend::Type::Category::Mapping)
		{
			auto const* innerMapping = dynamic_cast<solidity::frontend::MappingType const*>(innerBaseType);
			if (innerMapping && innerMapping->valueType()->category() == solidity::frontend::Type::Category::Mapping)
				isNestedMappingAccess = true;
		}
	}

	if (baseType && (baseType->category() == solidity::frontend::Type::Category::Mapping
		|| isDynamicArrayAccess || isNestedMappingAccess))
	{
		// Collect all index expressions and find root variable name by walking up IndexAccess chain.
		std::vector<solidity::frontend::Expression const*> indexExprs;
		solidity::frontend::Expression const* cursor = &_node;
		std::string varName = "map";

		while (auto const* idxAccess = dynamic_cast<solidity::frontend::IndexAccess const*>(cursor))
		{
			if (idxAccess->indexExpression())
				indexExprs.push_back(idxAccess->indexExpression());
			cursor = &idxAccess->baseExpression();
		}
		// cursor should now be the root Identifier
		if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(cursor))
			varName = ident->name();

		// Reverse so keys are in order: outermost first
		std::reverse(indexExprs.begin(), indexExprs.end());

		// Determine the final value type (the non-mapping type at this access level)
		awst::WType const* valueWType = nullptr;
		if (auto const* mappingType = dynamic_cast<solidity::frontend::MappingType const*>(baseType))
		{
			// For an access into a mapping, unwrap one level
			solidity::frontend::Type const* vt = mappingType->valueType();
			// If the value is still a mapping, unwrap further to get the final type
			while (auto const* nested = dynamic_cast<solidity::frontend::MappingType const*>(vt))
				vt = nested->valueType();
			valueWType = m_typeMapper.map(vt);
		}
		else
			valueWType = m_typeMapper.map(_node.annotation().type);

		auto e = std::make_shared<awst::BoxValueExpression>();
		e->sourceLocation = loc;
		e->wtype = valueWType;

		// Build prefix (variable name as box_key)
		auto prefix = std::make_shared<awst::BytesConstant>();
		prefix->sourceLocation = loc;
		prefix->wtype = awst::WType::boxKeyType();
		prefix->encoding = awst::BytesEncoding::Utf8;
		prefix->value = std::vector<uint8_t>(varName.begin(), varName.end());

		if (!indexExprs.empty())
		{
			// Translate all index expressions to bytes
			std::vector<std::shared_ptr<awst::Expression>> keyParts;
			for (auto const* idxExpr: indexExprs)
			{
				auto translated = translate(*idxExpr);
				auto reinterpret = std::make_shared<awst::ReinterpretCast>();
				reinterpret->sourceLocation = loc;
				reinterpret->wtype = awst::WType::bytesType();
				reinterpret->expr = std::move(translated);
				keyParts.push_back(std::move(reinterpret));
			}

			// For a single key, use it directly.
			// For multiple keys (nested mapping), concatenate them.
			std::shared_ptr<awst::Expression> compositeKey;
			if (keyParts.size() == 1)
			{
				compositeKey = std::move(keyParts[0]);
			}
			else
			{
				// Concatenate all key parts: concat(key1, key2, ...)
				compositeKey = std::move(keyParts[0]);
				for (size_t i = 1; i < keyParts.size(); ++i)
				{
					auto concat = std::make_shared<awst::IntrinsicCall>();
					concat->sourceLocation = loc;
					concat->wtype = awst::WType::bytesType();
					concat->opCode = "concat";
					concat->stackArgs.push_back(std::move(compositeKey));
					concat->stackArgs.push_back(std::move(keyParts[i]));
					compositeKey = std::move(concat);
				}

				// Hash the composite key with sha256 to fit within 64-byte box key limit.
				// prefix(varName) + raw concat could exceed 64 bytes for nested mappings
				// with address keys (e.g. 19 + 32 + 32 = 83 bytes for _operatorApprovals).
				// sha256 produces 32 bytes, so prefix + 32 always fits.
				auto hashCall = std::make_shared<awst::IntrinsicCall>();
				hashCall->sourceLocation = loc;
				hashCall->wtype = awst::WType::bytesType();
				hashCall->opCode = "sha256";
				hashCall->stackArgs.push_back(std::move(compositeKey));
				compositeKey = std::move(hashCall);
			}

			// Build BoxPrefixedKeyExpression
			auto boxKey = std::make_shared<awst::BoxPrefixedKeyExpression>();
			boxKey->sourceLocation = loc;
			boxKey->wtype = awst::WType::boxKeyType();
			boxKey->prefix = prefix;
			boxKey->key = std::move(compositeKey);

			e->key = std::move(boxKey);
		}
		else
		{
			e->key = std::move(prefix);
		}

		// When used as a write target (assignment LHS), push BoxValueExpression directly.
		// When reading, wrap in StateGet with a default so missing boxes return the
		// Solidity default (0/false/empty) instead of asserting existence.
		if (_node.annotation().willBeWrittenTo)
		{
			push(e);
		}
		else
		{
			auto defaultVal = StorageMapper::makeDefaultValue(e->wtype, loc);

			auto stateGet = std::make_shared<awst::StateGet>();
			stateGet->sourceLocation = loc;
			stateGet->wtype = e->wtype;
			stateGet->field = e;
			stateGet->defaultValue = defaultVal;
			push(stateGet);
		}
		return false;
	}

	// Not a mapping/dynamic-array — translate normally
	auto base = translate(_node.baseExpression());

	std::shared_ptr<awst::Expression> index;
	if (_node.indexExpression())
		index = translate(*_node.indexExpression());

	// Regular array index — ensure index is uint64
	if (index && index->wtype == awst::WType::biguintType())
	{
		// biguint → bytes (reinterpret) → uint64 (btoi)
		auto toBytes = std::make_shared<awst::ReinterpretCast>();
		toBytes->sourceLocation = loc;
		toBytes->wtype = awst::WType::bytesType();
		toBytes->expr = std::move(index);

		auto btoi = std::make_shared<awst::IntrinsicCall>();
		btoi->sourceLocation = loc;
		btoi->wtype = awst::WType::uint64Type();
		btoi->opCode = "btoi";
		btoi->stackArgs.push_back(std::move(toBytes));
		index = std::move(btoi);
	}

	auto e = std::make_shared<awst::IndexExpression>();
	e->sourceLocation = loc;
	e->base = std::move(base);
	e->index = std::move(index);
	e->wtype = m_typeMapper.map(_node.annotation().type);
	push(e);
	return false;
}

bool ExpressionTranslator::visit(solidity::frontend::IndexRangeAccess const& _node)
{
	auto loc = makeLoc(_node.location());
	auto base = translate(_node.baseExpression());

	std::shared_ptr<awst::Expression> start;
	std::shared_ptr<awst::Expression> end;

	if (_node.startExpression())
		start = translate(*_node.startExpression());
	else
	{
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = loc;
		zero->wtype = awst::WType::uint64Type();
		zero->value = "0";
		start = std::move(zero);
	}

	if (_node.endExpression())
		end = translate(*_node.endExpression());
	else
	{
		// arr[start:] → end = len(arr)
		auto lenCall = std::make_shared<awst::IntrinsicCall>();
		lenCall->sourceLocation = loc;
		lenCall->wtype = awst::WType::uint64Type();
		lenCall->opCode = "len";
		lenCall->stackArgs.push_back(base);
		end = std::move(lenCall);
	}

	// Coerce start/end to uint64 if needed (e.g. biguint literals)
	auto coerceToUint64 = [&](std::shared_ptr<awst::Expression> e) -> std::shared_ptr<awst::Expression> {
		if (e->wtype == awst::WType::biguintType())
		{
			auto toBytes = std::make_shared<awst::ReinterpretCast>();
			toBytes->sourceLocation = loc;
			toBytes->wtype = awst::WType::bytesType();
			toBytes->expr = std::move(e);

			auto btoi = std::make_shared<awst::IntrinsicCall>();
			btoi->sourceLocation = loc;
			btoi->wtype = awst::WType::uint64Type();
			btoi->opCode = "btoi";
			btoi->stackArgs.push_back(std::move(toBytes));
			return btoi;
		}
		return e;
	};
	start = coerceToUint64(std::move(start));
	end = coerceToUint64(std::move(end));

	// substring3(base, start, end) — extracts bytes from position start to end
	auto slice = std::make_shared<awst::IntrinsicCall>();
	slice->sourceLocation = loc;
	slice->wtype = m_typeMapper.map(_node.annotation().type);
	slice->opCode = "substring3";
	slice->stackArgs.push_back(std::move(base));
	slice->stackArgs.push_back(std::move(start));
	slice->stackArgs.push_back(std::move(end));
	push(slice);
	return false;
}

bool ExpressionTranslator::visit(solidity::frontend::Conditional const& _node)
{
	auto loc = makeLoc(_node.location());
	auto e = std::make_shared<awst::ConditionalExpression>();
	e->sourceLocation = loc;
	e->condition = translate(_node.condition());
	e->trueExpr = translate(_node.trueExpression());
	e->falseExpr = translate(_node.falseExpression());
	e->wtype = m_typeMapper.map(_node.annotation().type);
	push(e);
	return false;
}

bool ExpressionTranslator::visit(solidity::frontend::Assignment const& _node)
{
	using Token = solidity::frontend::Token;
	auto loc = makeLoc(_node.location());

	auto target = translate(_node.leftHandSide());
	auto value = translate(_node.rightHandSide());

	Token op = _node.assignmentOperator();

	// Check if target is an IndexExpression on bytes (bytes[i] = value).
	// Bytes are immutable on AVM, so transform to: base = replace3(base, i, value)
	if (auto const* indexExpr = dynamic_cast<awst::IndexExpression const*>(target.get()))
	{
		if (indexExpr->base && indexExpr->base->wtype
			&& indexExpr->base->wtype->kind() == awst::WTypeKind::Bytes)
		{
			if (op != Token::Assign)
				value = buildBinaryOp(op, translate(_node.leftHandSide()), std::move(value), indexExpr->wtype, loc);

			// Coerce value to bytes if needed (e.g. bytes[1])
			if (value->wtype && value->wtype->kind() == awst::WTypeKind::Bytes
				&& value->wtype != awst::WType::bytesType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = awst::WType::bytesType();
				cast->expr = std::move(value);
				value = std::move(cast);
			}

			auto replace = std::make_shared<awst::IntrinsicCall>();
			replace->sourceLocation = loc;
			replace->wtype = indexExpr->base->wtype;
			replace->opCode = "replace3";
			replace->stackArgs.push_back(indexExpr->base);
			replace->stackArgs.push_back(indexExpr->index);
			replace->stackArgs.push_back(std::move(value));

			auto e = std::make_shared<awst::AssignmentExpression>();
			e->sourceLocation = loc;
			e->wtype = indexExpr->base->wtype;
			e->target = indexExpr->base;
			e->value = std::move(replace);
			push(e);
			return false;
		}
	}

	// Check if target is a FieldExpression on a WTuple (struct field assignment).
	// WTuples are immutable in Puya, so we use copy-on-write: build a new tuple
	// with all fields copied except the modified one, then assign the whole tuple.
	if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(target.get()))
	{
		auto const* tupleType = dynamic_cast<awst::WTuple const*>(fieldExpr->base->wtype);
		if (tupleType && tupleType->names().has_value())
		{
			auto base = fieldExpr->base;
			std::string fieldName = fieldExpr->name;

			if (op != Token::Assign)
			{
				// Compound assignment: read current field value for the binary op
				auto currentField = std::make_shared<awst::FieldExpression>();
				currentField->sourceLocation = loc;
				currentField->base = base;
				currentField->name = fieldName;
				currentField->wtype = fieldExpr->wtype;
				value = buildBinaryOp(op, std::move(currentField), std::move(value), fieldExpr->wtype, loc);
			}

			// Cast value to match the field's type
			value = implicitNumericCast(std::move(value), fieldExpr->wtype, loc);

			auto newTuple = buildTupleWithUpdatedField(base, fieldName, std::move(value), loc);

			// For the assignment target, unwrap StateGet to get the underlying
			// StorageExpression (e.g. BoxValueExpression). StateGet is only valid
			// for reads, not as an Lvalue.
			auto writeTarget = base;
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
				writeTarget = sg->field;

			auto e = std::make_shared<awst::AssignmentExpression>();
			e->sourceLocation = loc;
			e->wtype = writeTarget->wtype;
			e->target = std::move(writeTarget);
			e->value = std::move(newTuple);
			push(e);
			return false;
		}
	}

	if (op != Token::Assign)
	{
		// Compound assignment: target op= value → target = target op value
		value = buildBinaryOp(op, translate(_node.leftHandSide()), std::move(value), target->wtype, loc);
	}

	// Insert implicit numeric cast if value type differs from target type
	value = implicitNumericCast(std::move(value), target->wtype, loc);
	// Coerce between bytes-compatible types (string → bytes, bytes → bytes[N], etc.)
	if (value->wtype != target->wtype
		&& target->wtype && target->wtype->kind() == awst::WTypeKind::Bytes)
	{
		bool valueIsCompatible = value->wtype == awst::WType::stringType()
			|| (value->wtype && value->wtype->kind() == awst::WTypeKind::Bytes);
		if (valueIsCompatible)
		{
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = loc;
			cast->wtype = target->wtype;
			cast->expr = std::move(value);
			value = std::move(cast);
		}
	}

	// Unwrap StateGet for the assignment target — StateGet is only valid for reads,
	// not as an Lvalue. Use the underlying StorageExpression instead.
	if (auto const* sg = dynamic_cast<awst::StateGet const*>(target.get()))
		target = sg->field;

	auto e = std::make_shared<awst::AssignmentExpression>();
	e->sourceLocation = loc;
	e->wtype = target->wtype;
	e->target = std::move(target);
	e->value = std::move(value);
	push(e);
	return false;
}

bool ExpressionTranslator::visit(solidity::frontend::TupleExpression const& _node)
{
	auto loc = makeLoc(_node.location());

	if (_node.components().size() == 1 && _node.components()[0])
	{
		// Single-element tuple is just parenthesization
		push(translate(*_node.components()[0]));
		return false;
	}

	// Inline array literals: [val1, val2, ...] → NewArray
	if (_node.isInlineArray())
	{
		auto* wtype = m_typeMapper.map(_node.annotation().type);
		auto* elementType = awst::WType::uint64Type();
		if (auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(wtype))
			elementType = refArr->elementType();

		auto e = std::make_shared<awst::NewArray>();
		e->sourceLocation = loc;
		e->wtype = wtype;
		for (auto const& comp: _node.components())
		{
			if (comp)
			{
				auto val = translate(*comp);
				val = implicitNumericCast(std::move(val), elementType, loc);
				e->values.push_back(std::move(val));
			}
		}
		push(e);
		return false;
	}

	auto e = std::make_shared<awst::TupleExpression>();
	e->sourceLocation = loc;
	std::vector<awst::WType const*> types;
	for (auto const& comp: _node.components())
	{
		if (comp)
		{
			auto translated = translate(*comp);
			types.push_back(translated->wtype);
			e->items.push_back(std::move(translated));
		}
	}
	e->wtype = new awst::WTuple(types); // TODO: memory management
	push(e);
	return false;
}

bool ExpressionTranslator::visit(solidity::frontend::FunctionCallOptions const& _node)
{
	auto loc = makeLoc(_node.location());
	auto const& innerExpr = _node.expression();

	// Check for .call{value: X} → payment inner transaction
	if (auto const* innerMember =
			dynamic_cast<solidity::frontend::MemberAccess const*>(&innerExpr))
	{
		if (innerMember->memberName() == "call" || innerMember->memberName() == "send")
		{
			// Extract value option
			std::shared_ptr<awst::Expression> valueAmount;
			auto const& optNames = _node.names();
			auto optValues = _node.options();
			for (size_t i = 0; i < optNames.size(); ++i)
			{
				if (*optNames[i] == "value" && i < optValues.size())
				{
					valueAmount = translate(*optValues[i]);
					valueAmount = implicitNumericCast(
						std::move(valueAmount), awst::WType::uint64Type(), loc
					);
					break;
				}
			}

			if (valueAmount)
			{
				auto receiver = translate(innerMember->expression());

				std::map<std::string, std::shared_ptr<awst::Expression>> fields;
				fields["Receiver"] = std::move(receiver);
				fields["Amount"] = std::move(valueAmount);

				auto create = buildCreateInnerTransaction(
					TxnTypePay, std::move(fields), loc
				);
				push(buildSubmitAndReturn(
					std::move(create), awst::WType::voidType(), loc
				));
				return false;
			}
		}
	}

	// Non-call options: translate base expression and warn
	Logger::instance().warning(
		"function call options {value:, gas:} ignored on Algorand", loc
	);
	push(translate(innerExpr));
	return false;
}

bool ExpressionTranslator::visit(solidity::frontend::ElementaryTypeNameExpression const& _node)
{
	// Type name used as expression (e.g., address(0))
	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = makeLoc(_node.location());
	vc->wtype = awst::WType::voidType();
	push(vc);
	return false;
}

std::shared_ptr<awst::Expression> ExpressionTranslator::buildTupleWithUpdatedField(
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

std::optional<ExpressionTranslator::StateVarInfo> ExpressionTranslator::resolveStateVar(
	std::string const& _name
)
{
	(void)_name;
	return std::nullopt;
}

std::shared_ptr<awst::IntegerConstant> ExpressionTranslator::makeUint64(
	std::string _value, awst::SourceLocation const& _loc
)
{
	auto e = std::make_shared<awst::IntegerConstant>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::uint64Type();
	e->value = std::move(_value);
	return e;
}

std::shared_ptr<awst::Expression> ExpressionTranslator::buildCreateInnerTransaction(
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

std::shared_ptr<awst::Expression> ExpressionTranslator::buildSubmitAndReturn(
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

	// For bool returns (transfer, send): emit submit as pending statement, return true
	if (_solidityReturnType == awst::WType::boolType())
	{
		auto stmt = std::make_shared<awst::ExpressionStatement>();
		stmt->sourceLocation = _loc;
		stmt->expr = std::move(submit);
		m_pendingStatements.push_back(std::move(stmt));

		auto result = std::make_shared<awst::BoolConstant>();
		result->sourceLocation = _loc;
		result->wtype = awst::WType::boolType();
		result->value = true;
		return result;
	}

	// For void returns: return submit directly
	if (!_solidityReturnType || _solidityReturnType == awst::WType::voidType())
		return submit;

	// For any other return type: emit submit as pending, return type-appropriate default
	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = _loc;
	stmt->expr = std::move(submit);
	m_pendingStatements.push_back(std::move(stmt));

	return StorageMapper::makeDefaultValue(_solidityReturnType, _loc);
}

} // namespace puyasol::builder
