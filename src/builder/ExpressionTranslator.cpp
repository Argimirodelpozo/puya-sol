#include "builder/ExpressionTranslator.h"
#include "builder/StorageMapper.h"
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

	// biguint → uint64: safely extract lower 64 bits
	// btoi only works on ≤8 bytes, but biguint from ABI-decoded uint256 is 32 bytes.
	// Approach: prepend 8 zero bytes, then extract last 8 bytes, then btoi.
	// This is always safe: concat(bzero(8), bytes) has len ≥ 9, so (len-8) ≥ 1.
	if (_expr->wtype == awst::WType::biguintType() && _targetType == awst::WType::uint64Type())
	{
		// reinterpret biguint → bytes
		auto toBytes = std::make_shared<awst::ReinterpretCast>();
		toBytes->sourceLocation = _loc;
		toBytes->wtype = awst::WType::bytesType();
		toBytes->expr = std::move(_expr);

		// bzero(8) — 8 zero bytes padding
		auto eight = std::make_shared<awst::IntegerConstant>();
		eight->sourceLocation = _loc;
		eight->wtype = awst::WType::uint64Type();
		eight->value = "8";

		auto padding = std::make_shared<awst::IntrinsicCall>();
		padding->sourceLocation = _loc;
		padding->wtype = awst::WType::bytesType();
		padding->opCode = "bzero";
		padding->stackArgs.push_back(std::move(eight));

		// concat(padding, bytes) → padded
		auto padded = std::make_shared<awst::IntrinsicCall>();
		padded->sourceLocation = _loc;
		padded->wtype = awst::WType::bytesType();
		padded->opCode = "concat";
		padded->stackArgs.push_back(std::move(padding));
		padded->stackArgs.push_back(std::move(toBytes));

		// len(padded) → paddedLen
		auto paddedLen = std::make_shared<awst::IntrinsicCall>();
		paddedLen->sourceLocation = _loc;
		paddedLen->wtype = awst::WType::uint64Type();
		paddedLen->opCode = "len";
		paddedLen->stackArgs.push_back(padded);

		// paddedLen - 8 → offset
		auto eight2 = std::make_shared<awst::IntegerConstant>();
		eight2->sourceLocation = _loc;
		eight2->wtype = awst::WType::uint64Type();
		eight2->value = "8";

		auto offset = std::make_shared<awst::UInt64BinaryOperation>();
		offset->sourceLocation = _loc;
		offset->wtype = awst::WType::uint64Type();
		offset->left = std::move(paddedLen);
		offset->op = awst::UInt64BinaryOperator::Sub;
		offset->right = std::move(eight2);

		// extract3(padded, offset, 8) → last 8 bytes
		auto eight3 = std::make_shared<awst::IntegerConstant>();
		eight3->sourceLocation = _loc;
		eight3->wtype = awst::WType::uint64Type();
		eight3->value = "8";

		auto extract = std::make_shared<awst::IntrinsicCall>();
		extract->sourceLocation = _loc;
		extract->wtype = awst::WType::bytesType();
		extract->opCode = "extract3";
		extract->stackArgs.push_back(std::move(padded));
		extract->stackArgs.push_back(std::move(offset));
		extract->stackArgs.push_back(std::move(eight3));

		// btoi(last8) → uint64
		auto btoi = std::make_shared<awst::IntrinsicCall>();
		btoi->sourceLocation = _loc;
		btoi->wtype = awst::WType::uint64Type();
		btoi->opCode = "btoi";
		btoi->stackArgs.push_back(std::move(extract));
		return btoi;
	}

	return _expr;
}

static OverloadedNamesSet const s_emptyOverloads;

FreeFunctionIdMap const ExpressionTranslator::s_emptyFreeFunctionIds;

ExpressionTranslator::ExpressionTranslator(
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
	Logger::instance().debug("[TRACE] ExpressionTranslator m_freeFunctionById.size()=" + std::to_string(m_freeFunctionById.size()) + " paramSize=" + std::to_string(_freeFunctionById.size()) + " addr=" + std::to_string((uintptr_t)&m_freeFunctionById));
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

void ExpressionTranslator::addParamRemap(int64_t _declId, std::string const& _uniqueName, awst::WType const* _type)
{
	m_paramRemaps[_declId] = {_uniqueName, _type};
}

void ExpressionTranslator::removeParamRemap(int64_t _declId)
{
	m_paramRemaps.erase(_declId);
}

void ExpressionTranslator::addSuperTarget(int64_t _funcId, std::string const& _name)
{
	m_superTargetNames[_funcId] = _name;
}

void ExpressionTranslator::addStorageAlias(int64_t _declId, std::shared_ptr<awst::Expression> _expr)
{
	m_storageAliases[_declId] = std::move(_expr);
}

void ExpressionTranslator::removeStorageAlias(int64_t _declId)
{
	m_storageAliases.erase(_declId);
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
		// Use RationalNumberType::literalValue() to get the actual computed value,
		// which includes sub-denomination multipliers (e.g. 365 days → 31536000)
		if (auto const* ratType = dynamic_cast<solidity::frontend::RationalNumberType const*>(solType))
			e->value = ratType->literalValue(nullptr).str();
		else
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
	case Token::HexStringLiteral:
	{
		// hex"..." literals contain raw binary data — use BytesConstant
		// so the serializer can base85-encode them (raw bytes break JSON/UTF-8)
		auto e = std::make_shared<awst::BytesConstant>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::bytesType();
		auto const& raw = _node.value();
		e->value.assign(raw.begin(), raw.end());
		e->encoding = awst::BytesEncoding::Base16;
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

	// Handle 'this' keyword → global CurrentApplicationAddress
	if (name == "this")
	{
		auto call = std::make_shared<awst::IntrinsicCall>();
		call->sourceLocation = loc;
		call->opCode = "global";
		call->immediates = {std::string("CurrentApplicationAddress")};
		call->wtype = awst::WType::accountType();
		push(std::move(call));
		return false;
	}

	// Check if this identifier is a remapped modifier parameter
	auto const* decl = _node.annotation().referencedDeclaration;
	if (decl)
	{
		auto remapIt = m_paramRemaps.find(decl->id());
		if (remapIt != m_paramRemaps.end())
		{
			auto var = std::make_shared<awst::VarExpression>();
			var->sourceLocation = loc;
			var->name = remapIt->second.name;
			var->wtype = remapIt->second.type;
			push(std::move(var));
			return false;
		}

		// Check for storage pointer alias (e.g. Type storage p = _mapping[key])
		auto aliasIt = m_storageAliases.find(decl->id());
		if (aliasIt != m_storageAliases.end())
		{
			push(aliasIt->second);
			return false;
		}
	}

	// Check if this is a variable reference (state, constant, or immutable)
	if (auto const* varDecl = dynamic_cast<solidity::frontend::VariableDeclaration const*>(decl))
	{
		// File-level and contract-level constants/immutables: inline the value
		if ((varDecl->isConstant() || varDecl->immutable()) && varDecl->value())
		{
			auto val = translate(*varDecl->value());
			// If the declared type is bytes[N] but the literal translated as
			// an IntegerConstant (e.g. bytes32 x = 0x00), convert to BytesConstant.
			auto* targetType = m_typeMapper.map(varDecl->type());
			if (auto const* bytesType = dynamic_cast<awst::BytesWType const*>(targetType))
			{
				if (auto* intConst = dynamic_cast<awst::IntegerConstant*>(val.get()))
				{
					int len = bytesType->length().value_or(0);
					std::vector<unsigned char> bytes(static_cast<size_t>(len), 0);
					// Parse the decimal string and convert to big-endian bytes
					std::string numStr = intConst->value;
					// Simple decimal-to-bytes: process digit by digit
					std::vector<unsigned char> bignum;
					for (char c : numStr)
					{
						int digit = c - '0';
						int carry = digit;
						for (auto& b : bignum)
						{
							int v = b * 10 + carry;
							b = static_cast<unsigned char>(v & 0xFF);
							carry = v >> 8;
						}
						while (carry > 0)
						{
							bignum.push_back(static_cast<unsigned char>(carry & 0xFF));
							carry >>= 8;
						}
					}
					// bignum is little-endian; copy to big-endian bytes[]
					for (size_t i = 0; i < bignum.size() && i < bytes.size(); ++i)
						bytes[bytes.size() - 1 - i] = bignum[i];

					auto bc = std::make_shared<awst::BytesConstant>();
					bc->sourceLocation = val->sourceLocation;
					bc->wtype = targetType;
					bc->encoding = awst::BytesEncoding::Base16;
					bc->value = std::move(bytes);
					push(std::move(bc));
					return false;
				}
			}
			// If declared type is bytes but inlined value is string, cast
			if (targetType == awst::WType::bytesType()
				&& val->wtype == awst::WType::stringType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = val->sourceLocation;
				cast->wtype = awst::WType::bytesType();
				cast->expr = std::move(val);
				push(std::move(cast));
				return false;
			}
			push(std::move(val));
			return false;
		}

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

	// Helper to coerce bytes[N] operands to a numeric type when used in numeric context.
	// For bytes[N] where N > 8, promotes to biguint (btoi only handles ≤8 bytes).
	// For smaller bytes, uses btoi → uint64.
	auto coerceBytesToUint = [&](std::shared_ptr<awst::Expression>& operand) {
		if (operand->wtype && operand->wtype->kind() == awst::WTypeKind::Bytes)
		{
			auto const* bytesWType = dynamic_cast<awst::BytesWType const*>(operand->wtype);
			if (bytesWType && bytesWType->length().has_value() && *bytesWType->length() > 8)
			{
				// bytes[N>8] → biguint via ReinterpretCast (btoi can't handle >8 bytes)
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = _loc;
				cast->wtype = awst::WType::biguintType();
				cast->expr = std::move(operand);
				operand = std::move(cast);
				return;
			}
			// bytes[N≤8] or unsized bytes → bytes → btoi → uint64
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
			auto thirtyTwo = std::make_shared<awst::IntegerConstant>();
			thirtyTwo->sourceLocation = _loc;
			thirtyTwo->wtype = awst::WType::uint64Type();
			thirtyTwo->value = "32";

			auto bzero = std::make_shared<awst::IntrinsicCall>();
			bzero->sourceLocation = _loc;
			bzero->wtype = awst::WType::bytesType();
			bzero->opCode = "bzero";
			bzero->stackArgs.push_back(std::move(thirtyTwo));

			// 255 - n: setbit uses MSB-first ordering, so bit (255-n) = 2^n
			auto twoFiftyFive = std::make_shared<awst::IntegerConstant>();
			twoFiftyFive->sourceLocation = _loc;
			twoFiftyFive->wtype = awst::WType::uint64Type();
			twoFiftyFive->value = "255";

			auto bitIdx = std::make_shared<awst::UInt64BinaryOperation>();
			bitIdx->sourceLocation = _loc;
			bitIdx->wtype = awst::WType::uint64Type();
			bitIdx->left = std::move(twoFiftyFive);
			bitIdx->right = std::move(shiftAmt);
			bitIdx->op = awst::UInt64BinaryOperator::Sub;

			// setbit(bzero(32), 255-n, 1) → bytes with only bit n set
			auto one = std::make_shared<awst::IntegerConstant>();
			one->sourceLocation = _loc;
			one->wtype = awst::WType::uint64Type();
			one->value = "1";

			auto setbit = std::make_shared<awst::IntrinsicCall>();
			setbit->sourceLocation = _loc;
			setbit->wtype = awst::WType::bytesType();
			setbit->opCode = "setbit";
			setbit->stackArgs.push_back(std::move(bzero));
			setbit->stackArgs.push_back(std::move(bitIdx));
			setbit->stackArgs.push_back(std::move(one));

			// Cast bytes → biguint
			auto castToBigUInt = std::make_shared<awst::ReinterpretCast>();
			castToBigUInt->sourceLocation = _loc;
			castToBigUInt->wtype = awst::WType::biguintType();
			castToBigUInt->expr = std::move(setbit);

			e->left = std::move(_left);
			e->right = std::move(castToBigUInt);
			e->op = (_op == Token::SHL || _op == Token::AssignShl)
				? awst::BigUIntBinaryOperator::Mult
				: awst::BigUIntBinaryOperator::FloorDiv;
			return e;
		}

		// For non-shift ops, promote right operand to biguint now
		promoteToBigUInt(_right);

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
		case Token::SHR: case Token::AssignShr: case Token::SAR: case Token::AssignSar: e->op = awst::UInt64BinaryOperator::RShift; break;
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

	// Check for user-defined operator overloading (e.g. `using {add as +} for Fr`)
	if (auto const* userFunc = *_node.annotation().userDefinedFunction)
	{
		// Look up the subroutine ID for this free/library function
		std::string subroutineId;
		auto it = m_freeFunctionById.find(userFunc->id());
		if (it != m_freeFunctionById.end())
			subroutineId = it->second;
		else
		{
			// Try library function lookup — prefer AST ID for overloaded functions
			auto byId = m_freeFunctionById.find(userFunc->id());
			if (byId != m_freeFunctionById.end())
				subroutineId = byId->second;
			else
			{
				auto const* scope = userFunc->scope();
				auto const* libContract = dynamic_cast<solidity::frontend::ContractDefinition const*>(scope);
				if (libContract && libContract->isLibrary())
				{
					std::string qualifiedName = libContract->name() + "." + userFunc->name();
					auto libIt = m_libraryFunctionIds.find(qualifiedName);
					if (libIt != m_libraryFunctionIds.end())
						subroutineId = libIt->second;
				}
			}
			if (subroutineId.empty())
				subroutineId = m_sourceFile + "." + userFunc->name();
		}

		auto left = translate(_node.leftExpression());
		auto right = translate(_node.rightExpression());
		auto* resultType = m_typeMapper.map(_node.annotation().type);

		auto call = std::make_shared<awst::SubroutineCallExpression>();
		call->sourceLocation = loc;
		call->wtype = resultType;
		call->target = awst::SubroutineID{subroutineId};

		awst::CallArg argA;
		argA.name = userFunc->parameters()[0]->name();
		argA.value = std::move(left);
		call->args.push_back(std::move(argA));

		awst::CallArg argB;
		argB.name = userFunc->parameters()[1]->name();
		argB.value = std::move(right);
		call->args.push_back(std::move(argB));

		push(std::move(call));
		return false;
	}

	// Check if the Solidity type checker resolved this to a compile-time constant
	// (e.g., 2**136, MODULUS - 1, 1 << 68). This handles all constant binary ops
	// including ** and << for biguint which AWST doesn't support as runtime ops.
	if (auto const* ratType = dynamic_cast<solidity::frontend::RationalNumberType const*>(
		_node.annotation().type))
	{
		if (!ratType->isFractional())
		{
			auto* resultType = m_typeMapper.map(_node.annotation().type);
			auto e = std::make_shared<awst::IntegerConstant>();
			e->sourceLocation = loc;
			e->wtype = resultType;
			e->value = ratType->literalValue(nullptr).str();
			push(e);
			return false;
		}
	}

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
		// i++ / i-- / ++i / --i → assignment expression: i = i +/- 1
		// Prefix (++i): result is the new value
		// Postfix (i++): result is the old value (before increment)
		bool isPrefix = _node.isPrefixOperation();
		auto isInc = (_node.getOperator() == Token::Inc);

		// For box-stored mappings, the operand is a bare BoxValueExpression
		// (willBeWrittenTo=true skips StateGet wrapping). Wrap it in StateGet
		// with a default value so the read works for uninitialized boxes.
		if (dynamic_cast<awst::BoxValueExpression const*>(operand.get()))
		{
			auto defaultVal = StorageMapper::makeDefaultValue(operand->wtype, loc);
			auto stateGet = std::make_shared<awst::StateGet>();
			stateGet->sourceLocation = loc;
			stateGet->wtype = operand->wtype;
			stateGet->field = operand;
			stateGet->defaultValue = defaultVal;
			operand = std::move(stateGet);
		}

		if (isPrefix)
		{
			// ++i / --i: compute new value, assign, return new value
			auto one = std::make_shared<awst::IntegerConstant>();
			one->sourceLocation = loc;
			one->wtype = operand->wtype;
			one->value = "1";

			std::shared_ptr<awst::Expression> newValue;
			if (isBigUInt(operand->wtype))
			{
				auto binOp = std::make_shared<awst::BigUIntBinaryOperation>();
				binOp->sourceLocation = loc;
				binOp->wtype = awst::WType::biguintType();
				binOp->left = operand;
				binOp->op = isInc ? awst::BigUIntBinaryOperator::Add : awst::BigUIntBinaryOperator::Sub;
				binOp->right = std::move(one);
				newValue = std::move(binOp);
			}
			else
			{
				auto binOp = std::make_shared<awst::UInt64BinaryOperation>();
				binOp->sourceLocation = loc;
				binOp->wtype = awst::WType::uint64Type();
				binOp->left = operand;
				binOp->op = isInc ? awst::UInt64BinaryOperator::Add : awst::UInt64BinaryOperator::Sub;
				binOp->right = std::move(one);
				newValue = std::move(binOp);
			}

			auto assignExpr = std::make_shared<awst::AssignmentExpression>();
			assignExpr->sourceLocation = loc;
			assignExpr->wtype = operand->wtype;
			auto target = translate(_node.subExpression());
			assignExpr->target = std::move(target);
			assignExpr->value = std::move(newValue);
			push(assignExpr);
		}
		else
		{
			// i++ / i--: capture old value via SingleEvaluation, increment as side effect
			// SingleEvaluation ensures the operand is read exactly once and cached.
			// Both the return value and the increment expression share the same
			// SingleEvaluation, so whichever evaluates first captures the old value.
			auto singleEval = std::make_shared<awst::SingleEvaluation>();
			singleEval->sourceLocation = loc;
			singleEval->wtype = operand->wtype;
			singleEval->source = operand;
			singleEval->id = static_cast<int>(_node.id());

			auto one = std::make_shared<awst::IntegerConstant>();
			one->sourceLocation = loc;
			one->wtype = operand->wtype;
			one->value = "1";

			// Build new value: singleEval +/- 1
			std::shared_ptr<awst::Expression> newValue;
			if (isBigUInt(operand->wtype))
			{
				auto binOp = std::make_shared<awst::BigUIntBinaryOperation>();
				binOp->sourceLocation = loc;
				binOp->wtype = awst::WType::biguintType();
				binOp->left = singleEval;
				binOp->op = isInc ? awst::BigUIntBinaryOperator::Add : awst::BigUIntBinaryOperator::Sub;
				binOp->right = std::move(one);
				newValue = std::move(binOp);
			}
			else
			{
				auto binOp = std::make_shared<awst::UInt64BinaryOperation>();
				binOp->sourceLocation = loc;
				binOp->wtype = awst::WType::uint64Type();
				binOp->left = singleEval;
				binOp->op = isInc ? awst::UInt64BinaryOperator::Add : awst::UInt64BinaryOperator::Sub;
				binOp->right = std::move(one);
				newValue = std::move(binOp);
			}

			// Emit increment as a pending statement
			auto incrStmt = std::make_shared<awst::AssignmentStatement>();
			incrStmt->sourceLocation = loc;
			auto target = translate(_node.subExpression());
			incrStmt->target = std::move(target);
			incrStmt->value = std::move(newValue);
			m_pendingStatements.push_back(std::move(incrStmt));

			// Return the SingleEvaluation (old value) as the expression result
			push(singleEval);
		}
		break;
	}
	case Token::BitNot:
	{
		// Bitwise NOT: ~operand → b~ (requires bytes type operand)
		auto expr = std::move(operand);
		auto* resultType = expr->wtype;
		// b~ requires bytes-typed operand; cast biguint → bytes if needed
		if (expr->wtype == awst::WType::biguintType())
		{
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = loc;
			cast->wtype = awst::WType::bytesType();
			cast->expr = std::move(expr);
			expr = std::move(cast);
		}
		auto e = std::make_shared<awst::BytesUnaryOperation>();
		e->sourceLocation = loc;
		e->wtype = expr->wtype;
		e->op = awst::BytesUnaryOperator::BitInvert;
		e->expr = std::move(expr);
		// Cast result back to original type
		if (resultType == awst::WType::biguintType())
		{
			auto castBack = std::make_shared<awst::ReinterpretCast>();
			castBack->sourceLocation = loc;
			castBack->wtype = resultType;
			castBack->expr = std::move(e);
			push(std::move(castBack));
		}
		else
			push(e);
		break;
	}
	case Token::Delete:
	{
		// delete x → StateDelete for mapping values (box delete),
		//            or assign zero/default for state vars and locals
		auto target = translate(_node.subExpression());

		// If target is a BoxValueExpression, emit StateDelete using the box as the field
		if (dynamic_cast<awst::BoxValueExpression const*>(target.get()))
		{
			auto stateDelete = std::make_shared<awst::StateDelete>();
			stateDelete->sourceLocation = loc;
			stateDelete->wtype = awst::WType::boolType();
			stateDelete->field = target; // The BoxValueExpression IS the field
			m_pendingStatements.push_back(std::make_shared<awst::ExpressionStatement>());
			m_pendingStatements.back()->sourceLocation = loc;
			static_cast<awst::ExpressionStatement*>(m_pendingStatements.back().get())->expr = std::move(stateDelete);
		}
		else
		{
			// For other targets: assign the zero/default value
			auto defaultVal = std::make_shared<awst::IntegerConstant>();
			defaultVal->sourceLocation = loc;
			if (isBigUInt(target->wtype))
			{
				defaultVal->wtype = awst::WType::biguintType();
				defaultVal->value = "0";
			}
			else
			{
				defaultVal->wtype = awst::WType::uint64Type();
				defaultVal->value = "0";
			}

			auto assignStmt = std::make_shared<awst::AssignmentStatement>();
			assignStmt->sourceLocation = loc;
			assignStmt->target = target;
			assignStmt->value = std::move(defaultVal);
			m_pendingStatements.push_back(std::move(assignStmt));
		}

		// Delete expression evaluates to void; push a dummy
		push(std::move(operand));
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
		// Check for custom error constructor (e.g., require(cond, Errors.Foo()))
		// before attempting to translate, as error constructors may not be translatable
		bool isCustomError = false;
		if (auto const* errorCall = dynamic_cast<solidity::frontend::FunctionCall const*>(args[1].get()))
		{
			auto const& errExpr = errorCall->expression();
			if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(&errExpr))
			{
				message = ma->memberName();
				isCustomError = true;
			}
			else if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&errExpr))
			{
				message = id->name();
				isCustomError = true;
			}
		}
		if (!isCustomError)
		{
			auto msgExpr = translate(*args[1]);
			if (auto const* sc = dynamic_cast<awst::StringConstant const*>(msgExpr.get()))
				message = sc->value;
			else
				message = "assertion failed";
		}
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

			// Narrowing biguint cast: uint256 → uint160/uint128/etc.
			// Both map to biguint, but we must insert truncation (AND mask)
			// so SafeCast-style overflow checks work correctly on AVM.
			if (targetType == awst::WType::biguintType()
				&& converted->wtype == awst::WType::biguintType())
			{
				auto const* solTargetType = _node.annotation().type;
				auto const* solSourceType = _node.arguments()[0]->annotation().type;
				if (auto const* targetIntType = dynamic_cast<solidity::frontend::IntegerType const*>(solTargetType))
				{
					unsigned targetBits = targetIntType->numBits();
					unsigned sourceBits = 256; // default
					if (auto const* srcIntType = dynamic_cast<solidity::frontend::IntegerType const*>(solSourceType))
						sourceBits = srcIntType->numBits();
					if (targetBits < sourceBits && targetBits < 256)
					{
						// Insert: converted = converted & ((1 << targetBits) - 1)
						auto mask = std::make_shared<awst::IntegerConstant>();
						mask->sourceLocation = loc;
						mask->wtype = awst::WType::biguintType();
						// Compute mask: (2^targetBits) - 1
						solidity::u256 maskVal = (solidity::u256(1) << targetBits) - 1;
						mask->value = maskVal.str();

						auto bitAnd = std::make_shared<awst::BigUIntBinaryOperation>();
						bitAnd->sourceLocation = loc;
						bitAnd->wtype = awst::WType::biguintType();
						bitAnd->left = std::move(converted);
						bitAnd->op = awst::BigUIntBinaryOperator::BitAnd;
						bitAnd->right = std::move(mask);
						converted = std::move(bitAnd);
					}
				}
			}

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
					// uint64 → bytes[N]: itob produces 8 bytes, truncate to N if needed
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = loc;
					itob->wtype = awst::WType::bytesType();
					itob->opCode = "itob";
					itob->stackArgs.push_back(std::move(converted));

					// Determine target byte width from Solidity type
					int byteWidth = 8; // default (itob output size)
					auto const* solTargetType = _node.annotation().type;
					if (auto const* fbType = dynamic_cast<solidity::frontend::FixedBytesType const*>(solTargetType))
						byteWidth = static_cast<int>(fbType->numBytes());

					std::shared_ptr<awst::Expression> result = std::move(itob);

					if (byteWidth < 8)
					{
						// itob produces 8 bytes; extract last byteWidth bytes
						// extract3(itob_result, 8 - byteWidth, byteWidth)
						auto offsetConst = std::make_shared<awst::IntegerConstant>();
						offsetConst->sourceLocation = loc;
						offsetConst->wtype = awst::WType::uint64Type();
						offsetConst->value = std::to_string(8 - byteWidth);

						auto widthConst = std::make_shared<awst::IntegerConstant>();
						widthConst->sourceLocation = loc;
						widthConst->wtype = awst::WType::uint64Type();
						widthConst->value = std::to_string(byteWidth);

						auto extract = std::make_shared<awst::IntrinsicCall>();
						extract->sourceLocation = loc;
						extract->wtype = awst::WType::bytesType();
						extract->opCode = "extract3";
						extract->stackArgs.push_back(std::move(result));
						extract->stackArgs.push_back(std::move(offsetConst));
						extract->stackArgs.push_back(std::move(widthConst));
						result = std::move(extract);
					}
					else if (byteWidth > 8)
					{
						// itob produces 8 bytes; pad to byteWidth with leading zeros
						// concat(bzero(byteWidth), itob_result) → extract last byteWidth bytes
						auto widthConst = std::make_shared<awst::IntegerConstant>();
						widthConst->sourceLocation = loc;
						widthConst->wtype = awst::WType::uint64Type();
						widthConst->value = std::to_string(byteWidth);

						auto pad = std::make_shared<awst::IntrinsicCall>();
						pad->sourceLocation = loc;
						pad->wtype = awst::WType::bytesType();
						pad->opCode = "bzero";
						pad->stackArgs.push_back(std::move(widthConst));

						auto cat = std::make_shared<awst::IntrinsicCall>();
						cat->sourceLocation = loc;
						cat->wtype = awst::WType::bytesType();
						cat->opCode = "concat";
						cat->stackArgs.push_back(std::move(pad));
						cat->stackArgs.push_back(std::move(result));

						auto lenExpr = std::make_shared<awst::IntrinsicCall>();
						lenExpr->sourceLocation = loc;
						lenExpr->wtype = awst::WType::uint64Type();
						lenExpr->opCode = "len";
						lenExpr->stackArgs.push_back(cat);

						auto widthConst2 = std::make_shared<awst::IntegerConstant>();
						widthConst2->sourceLocation = loc;
						widthConst2->wtype = awst::WType::uint64Type();
						widthConst2->value = std::to_string(byteWidth);

						auto offsetExpr = std::make_shared<awst::UInt64BinaryOperation>();
						offsetExpr->sourceLocation = loc;
						offsetExpr->wtype = awst::WType::uint64Type();
						offsetExpr->left = std::move(lenExpr);
						offsetExpr->right = std::move(widthConst2);
						offsetExpr->op = awst::UInt64BinaryOperator::Sub;

						auto widthConst3 = std::make_shared<awst::IntegerConstant>();
						widthConst3->sourceLocation = loc;
						widthConst3->wtype = awst::WType::uint64Type();
						widthConst3->value = std::to_string(byteWidth);

						auto extract = std::make_shared<awst::IntrinsicCall>();
						extract->sourceLocation = loc;
						extract->wtype = awst::WType::bytesType();
						extract->opCode = "extract3";
						extract->stackArgs.push_back(std::move(cat));
						extract->stackArgs.push_back(std::move(offsetExpr));
						extract->stackArgs.push_back(std::move(widthConst3));
						result = std::move(extract);
					}

					if (targetType != awst::WType::bytesType())
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = targetType;
						cast->expr = std::move(result);
						push(std::move(cast));
					}
					else
						push(std::move(result));
				}
				else if (isBigUInt(converted->wtype) && targetIsBytes)
				{
					// biguint → bytes[N]: pad/truncate to exact byte width
					// 1. ReinterpretCast biguint → bytes (variable-length)
					auto toBytes = std::make_shared<awst::ReinterpretCast>();
					toBytes->sourceLocation = loc;
					toBytes->wtype = awst::WType::bytesType();
					toBytes->expr = std::move(converted);

					// Determine target byte width from Solidity type
					int byteWidth = 32; // default for bytes32
					auto const* solTargetType = _node.annotation().type;
					if (auto const* fbType = dynamic_cast<solidity::frontend::FixedBytesType const*>(solTargetType))
						byteWidth = static_cast<int>(fbType->numBytes());

					// 2. concat(bzero(width), bytes) → ensure at least width bytes
					auto widthConst = std::make_shared<awst::IntegerConstant>();
					widthConst->sourceLocation = loc;
					widthConst->wtype = awst::WType::uint64Type();
					widthConst->value = std::to_string(byteWidth);

					auto pad = std::make_shared<awst::IntrinsicCall>();
					pad->sourceLocation = loc;
					pad->wtype = awst::WType::bytesType();
					pad->opCode = "bzero";
					pad->stackArgs.push_back(std::move(widthConst));

					auto cat = std::make_shared<awst::IntrinsicCall>();
					cat->sourceLocation = loc;
					cat->wtype = awst::WType::bytesType();
					cat->opCode = "concat";
					cat->stackArgs.push_back(std::move(pad));
					cat->stackArgs.push_back(std::move(toBytes));

					// 3. extract last byteWidth bytes
					auto lenCall = std::make_shared<awst::IntrinsicCall>();
					lenCall->sourceLocation = loc;
					lenCall->wtype = awst::WType::uint64Type();
					lenCall->opCode = "len";
					lenCall->stackArgs.push_back(cat);

					auto wc2 = std::make_shared<awst::IntegerConstant>();
					wc2->sourceLocation = loc;
					wc2->wtype = awst::WType::uint64Type();
					wc2->value = std::to_string(byteWidth);

					auto offset = std::make_shared<awst::IntrinsicCall>();
					offset->sourceLocation = loc;
					offset->wtype = awst::WType::uint64Type();
					offset->opCode = "-";
					offset->stackArgs.push_back(std::move(lenCall));
					offset->stackArgs.push_back(std::move(wc2));

					auto wc3 = std::make_shared<awst::IntegerConstant>();
					wc3->sourceLocation = loc;
					wc3->wtype = awst::WType::uint64Type();
					wc3->value = std::to_string(byteWidth);

					auto extract = std::make_shared<awst::IntrinsicCall>();
					extract->sourceLocation = loc;
					extract->wtype = awst::WType::bytesType();
					extract->opCode = "extract3";
					extract->stackArgs.push_back(std::move(cat));
					extract->stackArgs.push_back(std::move(offset));
					extract->stackArgs.push_back(std::move(wc3));

					// 4. ReinterpretCast to target bytes[N] type if needed
					if (targetType != awst::WType::bytesType())
					{
						auto finalCast = std::make_shared<awst::ReinterpretCast>();
						finalCast->sourceLocation = loc;
						finalCast->wtype = targetType;
						finalCast->expr = std::move(extract);
						push(std::move(finalCast));
					}
					else
						push(std::move(extract));
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

	// Handle user-defined value type wrap/unwrap: Fr.wrap(x) and Fr.unwrap(y)
	// These are no-ops since UDVT and underlying type both map to the same WType
	if (auto const* funcType = dynamic_cast<solidity::frontend::FunctionType const*>(
		funcExpr.annotation().type))
	{
		if (funcType->kind() == solidity::frontend::FunctionType::Kind::Wrap
			|| funcType->kind() == solidity::frontend::FunctionType::Kind::Unwrap)
		{
			if (!_node.arguments().empty())
			{
				auto val = translate(*_node.arguments()[0]);
				auto* targetType = m_typeMapper.map(_node.annotation().type);
				val = implicitNumericCast(std::move(val), targetType, loc);
				push(std::move(val));
			}
			return false;
		}
	}

	// Handle struct creation
	if (*_node.annotation().kind == FunctionCallKind::StructConstructorCall)
	{
		auto* solType = _node.annotation().type;
		auto* wtype = m_typeMapper.map(solType);

		auto const& names = _node.names();
		auto const& args = _node.arguments();

		// Collect field values into an ordered map
		std::map<std::string, std::shared_ptr<awst::Expression>> fieldValues;

		auto const* tupleType = dynamic_cast<awst::WTuple const*>(wtype);
		auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(wtype);
		if (!names.empty())
		{
			for (size_t i = 0; i < names.size(); ++i)
			{
				auto val = translate(*args[i]);
				if (tupleType && i < tupleType->types().size())
					val = implicitNumericCast(std::move(val), tupleType->types()[i], loc);
				else if (arc4StructType)
				{
					for (auto const& [fname, ftype]: arc4StructType->fields())
						if (fname == *names[i])
						{
							val = implicitNumericCast(std::move(val), ftype, loc);
							break;
						}
				}
				fieldValues[*names[i]] = std::move(val);
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
					else if (arc4StructType && i < arc4StructType->fields().size())
						val = implicitNumericCast(std::move(val), arc4StructType->fields()[i].second, loc);
					fieldValues[members[i]->name()] = std::move(val);
				}
			}
		}

		// Use NewStruct for ARC4Struct, NamedTupleExpression for WTuple
		if (arc4StructType)
		{
			// Wrap field values in ARC4Encode where the value's wtype doesn't match the field's ARC4 type
			for (auto const& [fname, ftype]: arc4StructType->fields())
			{
				auto it = fieldValues.find(fname);
				if (it != fieldValues.end() && it->second->wtype != ftype)
				{
					auto encode = std::make_shared<awst::ARC4Encode>();
					encode->sourceLocation = loc;
					encode->wtype = ftype;
					encode->value = std::move(it->second);
					it->second = std::move(encode);
				}
			}
			auto newStruct = std::make_shared<awst::NewStruct>();
			newStruct->sourceLocation = loc;
			newStruct->wtype = wtype;
			newStruct->values = std::move(fieldValues);
			push(newStruct);
		}
		else
		{
			auto structExpr = std::make_shared<awst::NamedTupleExpression>();
			structExpr->sourceLocation = loc;
			structExpr->wtype = wtype;
			structExpr->values = std::move(fieldValues);
			push(structExpr);
		}
		return false;
	}

	// Handle specific function calls
	if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr))
	{
		std::string memberName = memberAccess->memberName();

		auto const& baseExpr = memberAccess->expression();

		// Handle .call(...) and .call{value: X}("") → inner transaction or stub
		// On EVM: low-level call to another contract.
		// On AVM: translated to inner transaction (payment or app call).
		if (memberName == "call")
		{
			auto receiver = translate(baseExpr);
			bool handledAsInnerCall = false;

			if (callValueAmount)
			{
				// .call{value: X}("") → payment inner transaction
				std::map<std::string, std::shared_ptr<awst::Expression>> fields;
				fields["Receiver"] = std::move(receiver);
				fields["Amount"] = std::move(callValueAmount);

				auto create = buildCreateInnerTransaction(TxnTypePay, std::move(fields), loc);

				auto* submitWtype = m_typeMapper.createType<awst::WInnerTransaction>(TxnTypePay);
				auto submit = std::make_shared<awst::SubmitInnerTransaction>();
				submit->sourceLocation = loc;
				submit->wtype = submitWtype;
				submit->itxns.push_back(std::move(create));

				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = loc;
				stmt->expr = std::move(submit);
				m_pendingStatements.push_back(std::move(stmt));
			}
			else
			{
				// .call(data) without value — detect abi.encodeCall for inner app call
				if (!_node.arguments().empty())
				{
					auto const& dataArg = *_node.arguments()[0];
					if (auto const* encodeCallExpr = dynamic_cast<solidity::frontend::FunctionCall const*>(&dataArg))
					{
						auto const* encodeMA = dynamic_cast<MemberAccess const*>(&encodeCallExpr->expression());
						if (encodeMA && encodeMA->memberName() == "encodeCall"
									&& encodeCallExpr->arguments().size() >= 2)
						{
									// Extract target function from first arg (e.g., IERC20.transfer)
									auto const& targetFnExpr = *encodeCallExpr->arguments()[0];
									solidity::frontend::FunctionDefinition const* targetFuncDef = nullptr;
									
									// The first arg's type annotation is a FunctionType
									if (auto const* fnType = dynamic_cast<solidity::frontend::FunctionType const*>(
												targetFnExpr.annotation().type))
									{
												if (fnType->hasDeclaration())
												{
															targetFuncDef = dynamic_cast<solidity::frontend::FunctionDefinition const*>(
																		&fnType->declaration()
															);
												}
									}
									
									if (targetFuncDef)
									{
												handledAsInnerCall = true;
												
												// Build ARC4 method selector (reuse solTypeToARC4Name logic)
												auto solTypeToARC4 = [this](solidity::frontend::Type const* _type) -> std::string {
															auto* wtype = m_typeMapper.map(_type);
															if (wtype == awst::WType::biguintType()) return "uint256";
															if (wtype == awst::WType::uint64Type()) return "uint64";
															if (wtype == awst::WType::boolType()) return "bool";
															if (wtype == awst::WType::accountType()) return "address";
															if (wtype == awst::WType::bytesType()) return "byte[]";
															if (wtype == awst::WType::stringType()) return "string";
															if (wtype->kind() == awst::WTypeKind::Bytes)
															{
																		auto const* bw = static_cast<awst::BytesWType const*>(wtype);
																		if (bw->length().has_value())
																					return "byte[" + std::to_string(bw->length().value()) + "]";
																		return "byte[]";
															}
															if (auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(_type))
																		return "struct " + structType->structDefinition().name();
															return _type->toString(true);
												};
												
												std::string methodSelector = targetFuncDef->name() + "(";
												bool first = true;
												for (auto const& param: targetFuncDef->parameters())
												{
															if (!first) methodSelector += ",";
															methodSelector += solTypeToARC4(param->type());
															first = false;
												}
												methodSelector += ")";
												if (targetFuncDef->returnParameters().size() > 1)
												{
															methodSelector += "(";
															bool firstRet = true;
															for (auto const& retParam: targetFuncDef->returnParameters())
															{
																		if (!firstRet) methodSelector += ",";
																		methodSelector += solTypeToARC4(retParam->type());
																		firstRet = false;
															}
															methodSelector += ")";
												}
												else if (targetFuncDef->returnParameters().size() == 1)
															methodSelector += solTypeToARC4(targetFuncDef->returnParameters()[0]->type());
												else
															methodSelector += "void";

												auto methodConst = std::make_shared<awst::MethodConstant>();
												methodConst->sourceLocation = loc;
												methodConst->wtype = awst::WType::bytesType();
												methodConst->value = methodSelector;
												
												// Build ApplicationArgs tuple
												auto argsTuple = std::make_shared<awst::TupleExpression>();
												argsTuple->sourceLocation = loc;
												argsTuple->items.push_back(std::move(methodConst));
												
												// Extract call arguments from second arg (tuple)
												auto const& argsExpr = *encodeCallExpr->arguments()[1];
												std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression const>> callArgs;
												if (auto const* tupleExpr = dynamic_cast<solidity::frontend::TupleExpression const*>(&argsExpr))
												{
															for (auto const& comp: tupleExpr->components())
																		if (comp) callArgs.push_back(comp);
												}
												else
															callArgs.push_back(encodeCallExpr->arguments()[1]);
												
												// Encode each argument with proper ARC4 encoding
												for (auto const& arg: callArgs)
												{
															auto argExpr = translate(*arg);
															if (argExpr->wtype == awst::WType::bytesType()
																		|| argExpr->wtype->kind() == awst::WTypeKind::Bytes)
															{
																		argsTuple->items.push_back(std::move(argExpr));
															}
															else if (argExpr->wtype == awst::WType::uint64Type())
															{
																		auto itob = std::make_shared<awst::IntrinsicCall>();
																		itob->sourceLocation = loc;
																		itob->wtype = awst::WType::bytesType();
																		itob->opCode = "itob";
																		itob->stackArgs.push_back(std::move(argExpr));
																		argsTuple->items.push_back(std::move(itob));
															}
															else if (argExpr->wtype == awst::WType::biguintType())
															{
																		// biguint → ARC4 uint256 = 32 bytes, left-padded
																		auto cast = std::make_shared<awst::ReinterpretCast>();
																		cast->sourceLocation = loc;
																		cast->wtype = awst::WType::bytesType();
																		cast->expr = std::move(argExpr);

																		auto zeros = std::make_shared<awst::IntrinsicCall>();
																		zeros->sourceLocation = loc;
																		zeros->wtype = awst::WType::bytesType();
																		zeros->opCode = "bzero";
																		zeros->stackArgs.push_back(makeUint64("32", loc));

																		auto padded = std::make_shared<awst::IntrinsicCall>();
																		padded->sourceLocation = loc;
																		padded->wtype = awst::WType::bytesType();
																		padded->opCode = "concat";
																		padded->stackArgs.push_back(std::move(zeros));
																		padded->stackArgs.push_back(std::move(cast));

																		auto lenCall = std::make_shared<awst::IntrinsicCall>();
																		lenCall->sourceLocation = loc;
																		lenCall->wtype = awst::WType::uint64Type();
																		lenCall->opCode = "len";
																		lenCall->stackArgs.push_back(padded);

																		auto offset = std::make_shared<awst::IntrinsicCall>();
																		offset->sourceLocation = loc;
																		offset->wtype = awst::WType::uint64Type();
																		offset->opCode = "-";
																		offset->stackArgs.push_back(std::move(lenCall));
																		offset->stackArgs.push_back(makeUint64("32", loc));

																		auto extracted = std::make_shared<awst::IntrinsicCall>();
																		extracted->sourceLocation = loc;
																		extracted->wtype = awst::WType::bytesType();
																		extracted->opCode = "extract3";
																		extracted->stackArgs.push_back(std::move(padded));
																		extracted->stackArgs.push_back(std::move(offset));
																		extracted->stackArgs.push_back(makeUint64("32", loc));
																		
																		argsTuple->items.push_back(std::move(extracted));
															}
															else if (argExpr->wtype == awst::WType::boolType())
															{
																		// bool → ARC4 bool = 1 byte
																		auto zeroByte = std::make_shared<awst::BytesConstant>();
																		zeroByte->sourceLocation = loc;
																		zeroByte->wtype = awst::WType::bytesType();
																		zeroByte->encoding = awst::BytesEncoding::Base16;
																		zeroByte->value = {0x00};
																		
																		auto setbit = std::make_shared<awst::IntrinsicCall>();
																		setbit->sourceLocation = loc;
																		setbit->wtype = awst::WType::bytesType();
																		setbit->opCode = "setbit";
																		setbit->stackArgs.push_back(std::move(zeroByte));
																		setbit->stackArgs.push_back(makeUint64("0", loc));
																		setbit->stackArgs.push_back(std::move(argExpr));
																		
																		argsTuple->items.push_back(std::move(setbit));
															}
															else if (argExpr->wtype == awst::WType::accountType())
															{
																		// account/address → 32 bytes
																		auto cast = std::make_shared<awst::ReinterpretCast>();
																		cast->sourceLocation = loc;
																		cast->wtype = awst::WType::bytesType();
																		cast->expr = std::move(argExpr);
																		argsTuple->items.push_back(std::move(cast));
															}
															else
															{
																		// Fallback: reinterpret as bytes
																		auto cast = std::make_shared<awst::ReinterpretCast>();
																		cast->sourceLocation = loc;
																		cast->wtype = awst::WType::bytesType();
																		cast->expr = std::move(argExpr);
																		argsTuple->items.push_back(std::move(cast));
															}
												}
												
												// Build WTuple type for args
												std::vector<awst::WType const*> argTypes;
												for (auto const& item: argsTuple->items)
															argTypes.push_back(item->wtype);
												argsTuple->wtype = m_typeMapper.createType<awst::WTuple>(
															std::move(argTypes), std::nullopt
												);
												
												// Convert receiver address → app ID
												std::shared_ptr<awst::Expression> appId;
												if (receiver->wtype == awst::WType::applicationType())
												{
															appId = std::move(receiver);
												}
												else
												{
															std::shared_ptr<awst::Expression> bytesExpr = std::move(receiver);
															if (bytesExpr->wtype == awst::WType::accountType())
															{
																		auto toBytes = std::make_shared<awst::ReinterpretCast>();
																		toBytes->sourceLocation = loc;
																		toBytes->wtype = awst::WType::bytesType();
																		toBytes->expr = std::move(bytesExpr);
																		bytesExpr = std::move(toBytes);
															}
															auto extract = std::make_shared<awst::IntrinsicCall>();
															extract->sourceLocation = loc;
															extract->wtype = awst::WType::bytesType();
															extract->opCode = "extract";
															extract->immediates = {24, 8};
															extract->stackArgs.push_back(std::move(bytesExpr));
															
															auto btoi = std::make_shared<awst::IntrinsicCall>();
															btoi->sourceLocation = loc;
															btoi->wtype = awst::WType::uint64Type();
															btoi->opCode = "btoi";
															btoi->stackArgs.push_back(std::move(extract));
															
															auto cast = std::make_shared<awst::ReinterpretCast>();
															cast->sourceLocation = loc;
															cast->wtype = awst::WType::applicationType();
															cast->expr = std::move(btoi);
															appId = std::move(cast);
												}
												
												std::map<std::string, std::shared_ptr<awst::Expression>> fields;
												fields["ApplicationID"] = std::move(appId);
												fields["OnCompletion"] = makeUint64("0", loc);
												fields["ApplicationArgs"] = std::move(argsTuple);
												
												auto create = buildCreateInnerTransaction(TxnTypeAppl, std::move(fields), loc);
									
									// Submit inner transaction as a pending statement
									auto* submitWtype = m_typeMapper.createType<awst::WInnerTransaction>(TxnTypeAppl);
									auto submit = std::make_shared<awst::SubmitInnerTransaction>();
									submit->sourceLocation = loc;
									submit->wtype = submitWtype;
									submit->itxns.push_back(std::move(create));

									auto submitStmt = std::make_shared<awst::ExpressionStatement>();
									submitStmt->sourceLocation = loc;
									submitStmt->expr = std::move(submit);
									m_pendingStatements.push_back(std::move(submitStmt));

									// Read LastLog from most recently submitted inner txn
									auto readLog = std::make_shared<awst::IntrinsicCall>();
									readLog->sourceLocation = loc;
									readLog->wtype = awst::WType::bytesType();
									readLog->opCode = "itxn";
									readLog->immediates = {std::string("LastLog")};

									// Strip the 4-byte ARC4 return prefix (0x151f7c75)
									auto stripPrefix = std::make_shared<awst::IntrinsicCall>();
									stripPrefix->sourceLocation = loc;
									stripPrefix->opCode = "extract";
									stripPrefix->immediates = {4, 0};
									stripPrefix->wtype = awst::WType::bytesType();
									stripPrefix->stackArgs.push_back(std::move(readLog));

									// Return (true, return_data)
									auto trueLit2 = std::make_shared<awst::BoolConstant>();
									trueLit2->sourceLocation = loc;
									trueLit2->wtype = awst::WType::boolType();
									trueLit2->value = true;

									auto tuple2 = std::make_shared<awst::TupleExpression>();
									tuple2->sourceLocation = loc;
									auto* tupleWtype2 = m_typeMapper.createType<awst::WTuple>(
										std::vector<awst::WType const*>{awst::WType::boolType(), awst::WType::bytesType()}
									);
									tuple2->wtype = tupleWtype2;
									tuple2->items.push_back(std::move(trueLit2));
									tuple2->items.push_back(std::move(stripPrefix));

									push(tuple2);
												return false;
									}
						}
					}
				}
				
				if (!handledAsInnerCall)
				{
					// Generic .call(data) stub — data is not abi.encodeCall
					Logger::instance().warning(
								"address.call(data) stubbed — returns (true, empty). "
								"Cross-contract calls need inner app call translation.",
								loc
					);
					for (auto const& arg: _node.arguments())
								translate(*arg);
				}
			}
			
			if (!handledAsInnerCall)
			{
				// Return (true, empty_bytes) — EVM .call returns (bool, bytes)
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
			}
			return false;

		}

		// Handle .staticcall(...) — route to precompile if address is known
		if (memberName == "staticcall")
		{
			// Try to resolve precompile address from address(N) base expression
			std::optional<uint64_t> precompileAddr;
			if (auto const* baseCall = dynamic_cast<solidity::frontend::FunctionCall const*>(&baseExpr))
			{
				if (baseCall->annotation().kind.set()
					&& *baseCall->annotation().kind == solidity::frontend::FunctionCallKind::TypeConversion
					&& !baseCall->arguments().empty())
				{
					auto const* argType = baseCall->arguments()[0]->annotation().type;
					if (auto const* ratType = dynamic_cast<solidity::frontend::RationalNumberType const*>(argType))
					{
						auto val = ratType->literalValue(nullptr);
						if (val >= 1 && val <= 10)
							precompileAddr = static_cast<uint64_t>(val);
					}
				}
			}

			if (precompileAddr && !_node.arguments().empty())
			{
				// Translate the input data argument
				auto inputData = translate(*_node.arguments()[0]);

				std::shared_ptr<awst::Expression> resultBytes;

				auto makeExtract = [&](std::shared_ptr<awst::Expression> source, int offset, int length) {
					auto call = std::make_shared<awst::IntrinsicCall>();
					call->sourceLocation = loc;
					call->wtype = awst::WType::bytesType();
					call->opCode = "extract3";
					call->stackArgs.push_back(std::move(source));
					auto offExpr = std::make_shared<awst::IntegerConstant>();
					offExpr->sourceLocation = loc;
					offExpr->wtype = awst::WType::uint64Type();
					offExpr->value = std::to_string(offset);
					call->stackArgs.push_back(std::move(offExpr));
					auto lenExpr = std::make_shared<awst::IntegerConstant>();
					lenExpr->sourceLocation = loc;
					lenExpr->wtype = awst::WType::uint64Type();
					lenExpr->value = std::to_string(length);
					call->stackArgs.push_back(std::move(lenExpr));
					return call;
				};

				switch (*precompileAddr)
				{
				case 6: // ecAdd: input = [x0:32|y0:32|x1:32|y1:32] → ec_add BN254g1
				{
					Logger::instance().debug("staticcall precompile 0x06: ecAdd → ec_add BN254g1", loc);
					auto pointA = makeExtract(inputData, 0, 64);
					auto pointB = makeExtract(inputData, 64, 64);
					auto ecCall = std::make_shared<awst::IntrinsicCall>();
					ecCall->sourceLocation = loc;
					ecCall->wtype = awst::WType::bytesType();
					ecCall->opCode = "ec_add";
					ecCall->immediates.push_back("BN254g1");
					ecCall->stackArgs.push_back(std::move(pointA));
					ecCall->stackArgs.push_back(std::move(pointB));
					resultBytes = std::move(ecCall);
					break;
				}
				case 7: // ecMul: input = [x:32|y:32|scalar:32] → ec_scalar_mul BN254g1
				{
					Logger::instance().debug("staticcall precompile 0x07: ecMul → ec_scalar_mul BN254g1", loc);
					auto point = makeExtract(inputData, 0, 64);
					auto scalar = makeExtract(inputData, 64, 32);
					auto ecCall = std::make_shared<awst::IntrinsicCall>();
					ecCall->sourceLocation = loc;
					ecCall->wtype = awst::WType::bytesType();
					ecCall->opCode = "ec_scalar_mul";
					ecCall->immediates.push_back("BN254g1");
					ecCall->stackArgs.push_back(std::move(point));
					ecCall->stackArgs.push_back(std::move(scalar));
					resultBytes = std::move(ecCall);
					break;
				}
				case 8: // ecPairing: input groups of 192 bytes
				{
					Logger::instance().debug("staticcall precompile 0x08: ecPairing → ec_pairing_check BN254g1", loc);
					// EVM ecPairing format per pair (192 bytes):
					//   [G1.x:32 | G1.y:32 | G2.x_im:32 | G2.x_re:32 | G2.y_im:32 | G2.y_re:32]
					// AVM ec_pairing_check BN254g1:
					//   stack[0] = G1 points: concat of (G1.x||G1.y) per pair = 64*N bytes
					//   stack[1] = G2 points: concat of (x_re||x_im||y_re||y_im) per pair = 128*N bytes
					// G2 coordinate swap: EVM=(x_im,x_re,y_im,y_re) → AVM=(x_re,x_im,y_re,y_im)

					auto makeConcat = [&](std::shared_ptr<awst::Expression> a, std::shared_ptr<awst::Expression> b) {
						auto c = std::make_shared<awst::IntrinsicCall>();
						c->sourceLocation = loc;
						c->wtype = awst::WType::bytesType();
						c->opCode = "concat";
						c->stackArgs.push_back(std::move(a));
						c->stackArgs.push_back(std::move(b));
						return c;
					};

					// Build G1s and G2s for 2 pairs (384 bytes input)
					// Pair 0: input[0:192], Pair 1: input[192:384]
					// G1s = input[0:64] || input[192:256]
					auto g1_0 = makeExtract(inputData, 0, 64);
					auto g1_1 = makeExtract(inputData, 192, 64);
					auto g1s = makeConcat(std::move(g1_0), std::move(g1_1));

					// G2 pair 0 (EVM): input[64:192] = [x_im:32|x_re:32|y_im:32|y_re:32]
					// G2 pair 0 (AVM): [x_re:32|x_im:32|y_re:32|y_im:32]
					auto g2_0_xre = makeExtract(inputData, 96, 32);
					auto g2_0_xim = makeExtract(inputData, 64, 32);
					auto g2_0_yre = makeExtract(inputData, 160, 32);
					auto g2_0_yim = makeExtract(inputData, 128, 32);
					auto g2_0 = makeConcat(
						makeConcat(std::move(g2_0_xre), std::move(g2_0_xim)),
						makeConcat(std::move(g2_0_yre), std::move(g2_0_yim))
					);

					// G2 pair 1 (EVM): input[256:384] = [x_im:32|x_re:32|y_im:32|y_re:32]
					auto g2_1_xre = makeExtract(inputData, 288, 32);
					auto g2_1_xim = makeExtract(inputData, 256, 32);
					auto g2_1_yre = makeExtract(inputData, 352, 32);
					auto g2_1_yim = makeExtract(inputData, 320, 32);
					auto g2_1 = makeConcat(
						makeConcat(std::move(g2_1_xre), std::move(g2_1_xim)),
						makeConcat(std::move(g2_1_yre), std::move(g2_1_yim))
					);

					auto g2s = makeConcat(std::move(g2_0), std::move(g2_1));

					auto ecCall = std::make_shared<awst::IntrinsicCall>();
					ecCall->sourceLocation = loc;
					ecCall->wtype = awst::WType::boolType();
					ecCall->opCode = "ec_pairing_check";
					ecCall->immediates.push_back("BN254g1");
					ecCall->stackArgs.push_back(std::move(g1s));
					ecCall->stackArgs.push_back(std::move(g2s));

					// ec_pairing_check returns bool directly (1 or 0)
					// The Solidity code expects (bool success, bytes result) where
					// result is ABI-encoded bool (32 bytes, last byte = 0/1)
					// Build: result = itob(ecResult ? 1 : 0) padded to 32 bytes

					// First, convert bool to uint64
					auto boolToInt = std::make_shared<awst::IntrinsicCall>();
					boolToInt->sourceLocation = loc;
					boolToInt->wtype = awst::WType::uint64Type();
					boolToInt->opCode = "select";
					auto zero64 = std::make_shared<awst::IntegerConstant>();
					zero64->sourceLocation = loc;
					zero64->wtype = awst::WType::uint64Type();
					zero64->value = "0";
					auto one64 = std::make_shared<awst::IntegerConstant>();
					one64->sourceLocation = loc;
					one64->wtype = awst::WType::uint64Type();
					one64->value = "1";
					boolToInt->stackArgs.push_back(std::move(zero64));
					boolToInt->stackArgs.push_back(std::move(one64));
					boolToInt->stackArgs.push_back(std::move(ecCall));

					// itob gives 8 bytes, pad to 32 with zeros: bzero(24) || itob(val)
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = loc;
					itob->wtype = awst::WType::bytesType();
					itob->opCode = "itob";
					itob->stackArgs.push_back(std::move(boolToInt));

					auto padding = std::make_shared<awst::IntrinsicCall>();
					padding->sourceLocation = loc;
					padding->wtype = awst::WType::bytesType();
					padding->opCode = "bzero";
					auto pad24 = std::make_shared<awst::IntegerConstant>();
					pad24->sourceLocation = loc;
					pad24->wtype = awst::WType::uint64Type();
					pad24->value = "24";
					padding->stackArgs.push_back(std::move(pad24));

					resultBytes = makeConcat(std::move(padding), std::move(itob));
					break;
				}
				default:
					Logger::instance().warning(
						"address.staticcall to precompile 0x" + std::to_string(*precompileAddr) +
						" not yet supported on AVM",
						loc
					);
					resultBytes = nullptr;
					break;
				}

				if (resultBytes)
				{
					// Return (true, resultBytes) tuple
					auto trueLit = std::make_shared<awst::BoolConstant>();
					trueLit->sourceLocation = loc;
					trueLit->wtype = awst::WType::boolType();
					trueLit->value = true;

					auto tuple = std::make_shared<awst::TupleExpression>();
					tuple->sourceLocation = loc;
					auto* tupleWtype = m_typeMapper.createType<awst::WTuple>(
						std::vector<awst::WType const*>{awst::WType::boolType(), awst::WType::bytesType()}
					);
					tuple->wtype = tupleWtype;
					tuple->items.push_back(std::move(trueLit));
					tuple->items.push_back(std::move(resultBytes));

					push(tuple);
					return false;
				}
			}

			// Fallback: stub for non-precompile or unsupported precompile addresses
			for (auto const& arg: _node.arguments())
				translate(*arg);

			Logger::instance().warning(
				"address.staticcall(data) stubbed — returns (true, empty).",
				loc
			);

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

					auto lenToBytes = std::make_shared<awst::IntrinsicCall>();
					lenToBytes->sourceLocation = loc;
					lenToBytes->wtype = awst::WType::bytesType();
					lenToBytes->opCode = "itob";
					lenToBytes->stackArgs.push_back(lenRead);

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

		if (name == "sha256")
		{
			auto call = std::make_shared<awst::IntrinsicCall>();
			call->sourceLocation = loc;
			call->opCode = "sha256";
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

		// ecrecover(digest, v, r, s) → address
		// On EVM: recovers ECDSA signer address from signature.
		// On AVM: ecdsa_pk_recover exists but returns (X, Y) which requires
		// multi-value handling. For now, return a zero address as a stub.
		// The permit() function using ecrecover is an EVM-specific EIP-2612
		// pattern that needs an Algorand-native redesign.
		// TODO: implement proper ecdsa_pk_recover → keccak256 → address pipeline
		if (name == "ecrecover" && _node.arguments().size() == 4)
		{
			Logger::instance().warning(
				"ecrecover() stubbed — returns zero address. "
				"AVM ecdsa_pk_recover returns (X,Y) requiring multi-value support.",
				loc
			);
			// Translate arguments so they're not skipped (may have side effects)
			for (auto const& arg: _node.arguments())
				translate(*arg);
			// Return zero address
			auto e = std::make_shared<awst::AddressConstant>();
			e->sourceLocation = loc;
			e->wtype = awst::WType::accountType();
			e->value = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ";
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
			auto* refArr = dynamic_cast<awst::ReferenceArray const*>(resultType);
			auto e = std::make_shared<awst::NewArray>();
			e->sourceLocation = loc;
			e->wtype = resultType;

			// Try to resolve N at compile time so the array is properly sized
			// (replace2 fails on empty arrays)
			if (!_node.arguments().empty() && refArr)
			{
				auto const* argType = _node.arguments()[0]->annotation().type;
				if (auto const* ratType = dynamic_cast<RationalNumberType const*>(argType))
				{
					auto val = ratType->literalValue(nullptr);
					if (val > 0)
					{
						unsigned long long n = static_cast<unsigned long long>(val);
						for (unsigned long long i = 0; i < n; ++i)
							e->values.push_back(
								StorageMapper::makeDefaultValue(refArr->elementType(), loc));
					}
				}
			}

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

				bool isPacked = (memberName == "encodePacked");

				// Helper: convert expression to bytes for abi.encode/encodePacked
				// For encodePacked, respects the Solidity type's packed byte width
				auto toPackedBytes = [&](std::shared_ptr<awst::Expression> expr, solidity::frontend::Type const* solType) -> std::shared_ptr<awst::Expression> {
					// Determine packed byte width from Solidity type
					int packedWidth = 0; // 0 means dynamic/no truncation needed
					if (isPacked && solType)
					{
						auto cat = solType->category();
						if (cat == Type::Category::Integer)
						{
							auto const* intType = dynamic_cast<IntegerType const*>(solType);
							if (intType)
								packedWidth = static_cast<int>(intType->numBits() / 8);
						}
						else if (cat == Type::Category::FixedBytes)
						{
							auto const* fbType = dynamic_cast<FixedBytesType const*>(solType);
							if (fbType)
								packedWidth = static_cast<int>(fbType->numBytes());
						}
						else if (cat == Type::Category::Bool)
						{
							packedWidth = 1;
						}
					}

					std::shared_ptr<awst::Expression> bytesExpr;
					if (expr->wtype == awst::WType::bytesType())
						bytesExpr = std::move(expr);
					else if (expr->wtype == awst::WType::stringType()
						|| (expr->wtype && expr->wtype->kind() == awst::WTypeKind::Bytes))
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(expr);
						bytesExpr = std::move(cast);
					}
					else if (expr->wtype == awst::WType::uint64Type())
					{
						auto itob = std::make_shared<awst::IntrinsicCall>();
						itob->sourceLocation = loc;
						itob->wtype = awst::WType::bytesType();
						itob->opCode = "itob";
						itob->stackArgs.push_back(std::move(expr));
						bytesExpr = std::move(itob);
					}
					else if (expr->wtype == awst::WType::biguintType())
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(expr);
						bytesExpr = std::move(cast);
					}
					else if (expr->wtype == awst::WType::accountType())
					{
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(expr);
						bytesExpr = std::move(cast);
					}
					else
					{
						// Fallback: reinterpret as bytes
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(expr);
						bytesExpr = std::move(cast);
					}

					// For encodePacked with fixed-width types, extract exact byte width
					// e.g. uint8 → itob produces 8 bytes, extract last 1 byte
					// e.g. uint256 → biguint bytes are variable, pad/extract to 32 bytes
					if (packedWidth > 0 && packedWidth != 8)
					{
						if (packedWidth <= 8)
						{
							// Small uint (uint8..uint64): itob produces 8 bytes, extract suffix
							auto extract = std::make_shared<awst::IntrinsicCall>();
							extract->sourceLocation = loc;
							extract->wtype = awst::WType::bytesType();
							extract->opCode = "extract";
							extract->immediates.push_back(8 - packedWidth);
							extract->immediates.push_back(packedWidth);
							extract->stackArgs.push_back(std::move(bytesExpr));
							bytesExpr = std::move(extract);
						}
						else
						{
							// Large types (uint128, uint256, bytesN where N>8):
							// Ensure exactly packedWidth bytes:
							// 1. concat(bzero(packedWidth), bytes) → guaranteed >= packedWidth
							// 2. extract last packedWidth bytes via len/extract3
							auto widthConst = std::make_shared<awst::IntegerConstant>();
							widthConst->sourceLocation = loc;
							widthConst->wtype = awst::WType::uint64Type();
							widthConst->value = std::to_string(packedWidth);

							auto pad = std::make_shared<awst::IntrinsicCall>();
							pad->sourceLocation = loc;
							pad->wtype = awst::WType::bytesType();
							pad->opCode = "bzero";
							pad->stackArgs.push_back(std::move(widthConst));

							// concat(zeros, bytes) to ensure at least packedWidth bytes
							auto cat = std::make_shared<awst::IntrinsicCall>();
							cat->sourceLocation = loc;
							cat->wtype = awst::WType::bytesType();
							cat->opCode = "concat";
							cat->stackArgs.push_back(std::move(pad));
							cat->stackArgs.push_back(std::move(bytesExpr));

							// len(concat_result)
							auto lenCall = std::make_shared<awst::IntrinsicCall>();
							lenCall->sourceLocation = loc;
							lenCall->wtype = awst::WType::uint64Type();
							lenCall->opCode = "len";
							lenCall->stackArgs.push_back(cat);

							// offset = len - packedWidth
							auto widthConst2 = std::make_shared<awst::IntegerConstant>();
							widthConst2->sourceLocation = loc;
							widthConst2->wtype = awst::WType::uint64Type();
							widthConst2->value = std::to_string(packedWidth);

							auto offset = std::make_shared<awst::IntrinsicCall>();
							offset->sourceLocation = loc;
							offset->wtype = awst::WType::uint64Type();
							offset->opCode = "-";
							offset->stackArgs.push_back(std::move(lenCall));
							offset->stackArgs.push_back(std::move(widthConst2));

							// extract3(concat_result, offset, packedWidth)
							auto widthConst3 = std::make_shared<awst::IntegerConstant>();
							widthConst3->sourceLocation = loc;
							widthConst3->wtype = awst::WType::uint64Type();
							widthConst3->value = std::to_string(packedWidth);

							auto extract = std::make_shared<awst::IntrinsicCall>();
							extract->sourceLocation = loc;
							extract->wtype = awst::WType::bytesType();
							extract->opCode = "extract3";
							extract->stackArgs.push_back(std::move(cat));
							extract->stackArgs.push_back(std::move(offset));
							extract->stackArgs.push_back(std::move(widthConst3));

							bytesExpr = std::move(extract);
						}
					}

					return bytesExpr;
				};

				// Helper: pack a single argument, expanding arrays element-by-element
				auto packArg = [&](size_t argIdx) -> std::shared_ptr<awst::Expression> {
					auto const* solType = args[argIdx]->annotation().type;

					// Check if the argument is an array type
					auto const* arrType = dynamic_cast<ArrayType const*>(solType);
					// Also check for UDVT arrays: type checker resolves UDVT to underlying
					if (!arrType && solType && solType->category() == Type::Category::UserDefinedValueType)
					{
						auto const* udvt = dynamic_cast<UserDefinedValueType const*>(solType);
						if (udvt)
							arrType = dynamic_cast<ArrayType const*>(&udvt->underlyingType());
					}

					if (arrType && !arrType->isByteArrayOrString())
					{
						auto arrayExpr = translate(*args[argIdx]);
						auto const* elemSolType = arrType->baseType();

						// Static array: unroll element access
						if (!arrType->isDynamicallySized())
						{
							int len = static_cast<int>(arrType->length());
							std::shared_ptr<awst::Expression> packed;
							for (int j = 0; j < len; ++j)
							{
								auto idx = std::make_shared<awst::IntegerConstant>();
								idx->sourceLocation = loc;
								idx->wtype = awst::WType::uint64Type();
								idx->value = std::to_string(j);

								auto indexExpr = std::make_shared<awst::IndexExpression>();
								indexExpr->sourceLocation = loc;
								indexExpr->base = arrayExpr;
								indexExpr->index = std::move(idx);
								indexExpr->wtype = m_typeMapper.map(elemSolType);

								auto elemBytes = toPackedBytes(std::move(indexExpr), elemSolType);
								if (!packed)
									packed = std::move(elemBytes);
								else
								{
									auto cat = std::make_shared<awst::IntrinsicCall>();
									cat->sourceLocation = loc;
									cat->wtype = awst::WType::bytesType();
									cat->opCode = "concat";
									cat->stackArgs.push_back(std::move(packed));
									cat->stackArgs.push_back(std::move(elemBytes));
									packed = std::move(cat);
								}
							}
							return packed ? packed : toPackedBytes(translate(*args[argIdx]), solType);
						}
						else
						{
							// Dynamic array: ARC4Encode produces concatenated element bytes.
							// ReferenceArray encoding uses length_header=False, so the result
							// is the raw concatenation of elements — exactly what encodePacked needs.
							auto encode = std::make_shared<awst::ARC4Encode>();
							encode->sourceLocation = loc;
							encode->wtype = awst::WType::bytesType();
							encode->value = std::move(arrayExpr);
							return std::shared_ptr<awst::Expression>(std::move(encode));
						}
					}

					return toPackedBytes(translate(*args[argIdx]), solType);
				};

				auto result = packArg(0);
				for (size_t i = 1; i < args.size(); ++i)
				{
					auto arg = packArg(i);
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

			// Helper: encode a single expression as ARC4 bytes for calldata encoding
			auto encodeArgAsARC4Bytes = [&](std::shared_ptr<awst::Expression> argExpr) -> std::shared_ptr<awst::Expression> {
				if (argExpr->wtype == awst::WType::bytesType()
					|| argExpr->wtype->kind() == awst::WTypeKind::Bytes)
				{
					return argExpr;
				}
				else if (argExpr->wtype == awst::WType::uint64Type())
				{
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = loc;
					itob->wtype = awst::WType::bytesType();
					itob->opCode = "itob";
					itob->stackArgs.push_back(std::move(argExpr));
					return itob;
				}
				else if (argExpr->wtype == awst::WType::biguintType())
				{
					// biguint → 32 bytes, left-padded
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(argExpr);

					auto zeros = std::make_shared<awst::IntrinsicCall>();
					zeros->sourceLocation = loc;
					zeros->wtype = awst::WType::bytesType();
					zeros->opCode = "bzero";
					zeros->stackArgs.push_back(makeUint64("32", loc));

					auto padded = std::make_shared<awst::IntrinsicCall>();
					padded->sourceLocation = loc;
					padded->wtype = awst::WType::bytesType();
					padded->opCode = "concat";
					padded->stackArgs.push_back(std::move(zeros));
					padded->stackArgs.push_back(std::move(cast));

					auto lenCall = std::make_shared<awst::IntrinsicCall>();
					lenCall->sourceLocation = loc;
					lenCall->wtype = awst::WType::uint64Type();
					lenCall->opCode = "len";
					lenCall->stackArgs.push_back(padded);

					auto offset = std::make_shared<awst::IntrinsicCall>();
					offset->sourceLocation = loc;
					offset->wtype = awst::WType::uint64Type();
					offset->opCode = "-";
					offset->stackArgs.push_back(std::move(lenCall));
					offset->stackArgs.push_back(makeUint64("32", loc));

					auto extracted = std::make_shared<awst::IntrinsicCall>();
					extracted->sourceLocation = loc;
					extracted->wtype = awst::WType::bytesType();
					extracted->opCode = "extract3";
					extracted->stackArgs.push_back(std::move(padded));
					extracted->stackArgs.push_back(std::move(offset));
					extracted->stackArgs.push_back(makeUint64("32", loc));
					return extracted;
				}
				else if (argExpr->wtype == awst::WType::boolType())
				{
					auto zeroByte = std::make_shared<awst::BytesConstant>();
					zeroByte->sourceLocation = loc;
					zeroByte->wtype = awst::WType::bytesType();
					zeroByte->encoding = awst::BytesEncoding::Base16;
					zeroByte->value = {0x00};

					auto setbit = std::make_shared<awst::IntrinsicCall>();
					setbit->sourceLocation = loc;
					setbit->wtype = awst::WType::bytesType();
					setbit->opCode = "setbit";
					setbit->stackArgs.push_back(std::move(zeroByte));
					setbit->stackArgs.push_back(makeUint64("0", loc));
					setbit->stackArgs.push_back(std::move(argExpr));
					return setbit;
				}
				else if (argExpr->wtype == awst::WType::accountType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(argExpr);
					return cast;
				}
				else
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(argExpr);
					return cast;
				}
			};

			// Helper: build ARC4 method selector string from a FunctionDefinition
			auto buildARC4MethodSelector = [this](solidity::frontend::FunctionDefinition const* funcDef) -> std::string {
				auto solTypeToARC4 = [this](solidity::frontend::Type const* _type) -> std::string {
					auto* wtype = m_typeMapper.map(_type);
					if (wtype == awst::WType::biguintType()) return "uint256";
					if (wtype == awst::WType::uint64Type()) return "uint64";
					if (wtype == awst::WType::boolType()) return "bool";
					if (wtype == awst::WType::accountType()) return "address";
					if (wtype == awst::WType::bytesType()) return "byte[]";
					if (wtype == awst::WType::stringType()) return "string";
					if (wtype->kind() == awst::WTypeKind::Bytes)
					{
						auto const* bw = static_cast<awst::BytesWType const*>(wtype);
						if (bw->length().has_value())
							return "byte[" + std::to_string(bw->length().value()) + "]";
						return "byte[]";
					}
					if (auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(_type))
						return "struct " + structType->structDefinition().name();
					return _type->toString(true);
				};

				std::string selector = funcDef->name() + "(";
				bool first = true;
				for (auto const& param: funcDef->parameters())
				{
					if (!first) selector += ",";
					selector += solTypeToARC4(param->type());
					first = false;
				}
				selector += ")";
				if (funcDef->returnParameters().size() > 1)
				{
					selector += "(";
					bool firstRet = true;
					for (auto const& retParam: funcDef->returnParameters())
					{
						if (!firstRet) selector += ",";
						selector += solTypeToARC4(retParam->type());
						firstRet = false;
					}
					selector += ")";
				}
				else if (funcDef->returnParameters().size() == 1)
					selector += solTypeToARC4(funcDef->returnParameters()[0]->type());
				else
					selector += "void";
				return selector;
			};

			// Helper: concatenate a list of byte expressions using concat intrinsics
			auto concatByteExprs = [&](std::vector<std::shared_ptr<awst::Expression>> parts) -> std::shared_ptr<awst::Expression> {
				if (parts.empty())
				{
					auto e = std::make_shared<awst::BytesConstant>();
					e->sourceLocation = loc;
					e->wtype = awst::WType::bytesType();
					e->encoding = awst::BytesEncoding::Base16;
					e->value = {};
					return e;
				}
				auto result = std::move(parts[0]);
				for (size_t i = 1; i < parts.size(); ++i)
				{
					auto concat = std::make_shared<awst::IntrinsicCall>();
					concat->sourceLocation = loc;
					concat->wtype = awst::WType::bytesType();
					concat->opCode = "concat";
					concat->stackArgs.push_back(std::move(result));
					concat->stackArgs.push_back(std::move(parts[i]));
					result = std::move(concat);
				}
				return result;
			};

			// abi.encodeCall(fn, (args)) → method_selector || ARC4_encoded_args
			if (memberName == "encodeCall")
			{
				if (_node.arguments().size() >= 2)
				{
					auto const& targetFnExpr = *_node.arguments()[0];
					solidity::frontend::FunctionDefinition const* targetFuncDef = nullptr;
					if (auto const* fnType = dynamic_cast<solidity::frontend::FunctionType const*>(
								targetFnExpr.annotation().type))
					{
						if (fnType->hasDeclaration())
							targetFuncDef = dynamic_cast<solidity::frontend::FunctionDefinition const*>(
								&fnType->declaration());
					}

					if (targetFuncDef)
					{
						std::string methodSig = buildARC4MethodSelector(targetFuncDef);
						auto methodConst = std::make_shared<awst::MethodConstant>();
						methodConst->sourceLocation = loc;
						methodConst->wtype = awst::WType::bytesType();
						methodConst->value = methodSig;

						// Extract arguments from second arg (tuple)
						std::vector<std::shared_ptr<awst::Expression>> parts;
						parts.push_back(std::move(methodConst));

						auto const& argsExpr = *_node.arguments()[1];
						std::vector<solidity::frontend::ASTPointer<solidity::frontend::Expression const>> callArgs;
						if (auto const* tupleExpr = dynamic_cast<solidity::frontend::TupleExpression const*>(&argsExpr))
						{
							for (auto const& comp: tupleExpr->components())
								if (comp) callArgs.push_back(comp);
						}
						else
							callArgs.push_back(_node.arguments()[1]);

						for (auto const& arg: callArgs)
							parts.push_back(encodeArgAsARC4Bytes(translate(*arg)));

						push(concatByteExprs(std::move(parts)));
						return false;
					}
				}
				// Fallback: empty bytes
				auto e = std::make_shared<awst::BytesConstant>();
				e->sourceLocation = loc;
				e->wtype = awst::WType::bytesType();
				e->encoding = awst::BytesEncoding::Base16;
				e->value = {};
				push(e);
				return false;
			}

			// abi.encodeWithSelector(bytes4, args...) → selector || ARC4_encoded_args
			if (memberName == "encodeWithSelector")
			{
				auto const& args = _node.arguments();
				if (!args.empty())
				{
					std::vector<std::shared_ptr<awst::Expression>> parts;
					// First arg is the bytes4 selector
					parts.push_back(translate(*args[0]));
					// Remaining args are encoded as ARC4
					for (size_t i = 1; i < args.size(); ++i)
						parts.push_back(encodeArgAsARC4Bytes(translate(*args[i])));
					push(concatByteExprs(std::move(parts)));
					return false;
				}
				auto e = std::make_shared<awst::BytesConstant>();
				e->sourceLocation = loc;
				e->wtype = awst::WType::bytesType();
				e->encoding = awst::BytesEncoding::Base16;
				e->value = {};
				push(e);
				return false;
			}

			// abi.encodeWithSignature(string, args...) → sha256(sig)[0:4] || ARC4_encoded_args
			if (memberName == "encodeWithSignature")
			{
				auto const& args = _node.arguments();
				if (!args.empty())
				{
					std::vector<std::shared_ptr<awst::Expression>> parts;
					// First arg is the string signature — use MethodConstant to compute 4-byte selector
					// Extract the string literal value
					auto sigExpr = translate(*args[0]);
					if (auto const* strConst = dynamic_cast<awst::BytesConstant const*>(sigExpr.get()))
					{
						auto methodConst = std::make_shared<awst::MethodConstant>();
						methodConst->sourceLocation = loc;
						methodConst->wtype = awst::WType::bytesType();
						methodConst->value = std::string(strConst->value.begin(), strConst->value.end());
						parts.push_back(std::move(methodConst));
					}
					else
					{
						// Dynamic signature: hash at runtime — sha256(sig), extract first 4 bytes
						auto hash = std::make_shared<awst::IntrinsicCall>();
						hash->sourceLocation = loc;
						hash->wtype = awst::WType::bytesType();
						hash->opCode = "sha256";
						hash->stackArgs.push_back(std::move(sigExpr));

						auto extract4 = std::make_shared<awst::IntrinsicCall>();
						extract4->sourceLocation = loc;
						extract4->wtype = awst::WType::bytesType();
						extract4->opCode = "extract";
						extract4->immediates = {0, 4};
						extract4->stackArgs.push_back(std::move(hash));
						parts.push_back(std::move(extract4));
					}

					for (size_t i = 1; i < args.size(); ++i)
						parts.push_back(encodeArgAsARC4Bytes(translate(*args[i])));
					push(concatByteExprs(std::move(parts)));
					return false;
				}
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
						// For scalar types, use appropriate cast
						if (targetType == awst::WType::boolType())
						{
							// bytes → bool: check if any byte is non-zero
							// ABI-encoded bool is 32 bytes, last byte is 0 or 1
							// Use btoi on the last byte, then compare != 0
							auto bytesExpr = std::move(decoded);
							if (bytesExpr->wtype != awst::WType::bytesType())
							{
								auto toBytes = std::make_shared<awst::ReinterpretCast>();
								toBytes->sourceLocation = loc;
								toBytes->wtype = awst::WType::bytesType();
								toBytes->expr = std::move(bytesExpr);
								bytesExpr = std::move(toBytes);
							}
							// btoi(bytes) — will interpret as big-endian integer
							auto btoi = std::make_shared<awst::IntrinsicCall>();
							btoi->sourceLocation = loc;
							btoi->wtype = awst::WType::uint64Type();
							btoi->opCode = "btoi";
							btoi->stackArgs.push_back(std::move(bytesExpr));

							auto zero = std::make_shared<awst::IntegerConstant>();
							zero->sourceLocation = loc;
							zero->wtype = awst::WType::uint64Type();
							zero->value = "0";

							auto cmp = std::make_shared<awst::NumericComparisonExpression>();
							cmp->sourceLocation = loc;
							cmp->wtype = awst::WType::boolType();
							cmp->lhs = std::move(btoi);
							cmp->rhs = std::move(zero);
							cmp->op = awst::NumericComparison::Ne;
							push(cmp);
						}
						else if (targetType->kind() != awst::WTypeKind::WTuple)
						{
							auto cast = std::make_shared<awst::ReinterpretCast>();
							cast->sourceLocation = loc;
							cast->wtype = targetType;
							cast->expr = std::move(decoded);
							push(cast);
						}
						else if (auto const* tupleType = dynamic_cast<awst::WTuple const*>(targetType))
						{
							// ABI decode bytes into tuple fields by extracting at 32-byte boundaries
							// Each ABI-encoded field is 32 bytes (uint256, address, bytes32, etc.)
							auto tuple = std::make_shared<awst::TupleExpression>();
							tuple->sourceLocation = loc;
							tuple->wtype = targetType;

							// Ensure source is bytes for extraction
							auto bytesSource = std::move(decoded);
							if (bytesSource->wtype != awst::WType::bytesType())
							{
								auto toBytes = std::make_shared<awst::ReinterpretCast>();
								toBytes->sourceLocation = loc;
								toBytes->wtype = awst::WType::bytesType();
								toBytes->expr = std::move(bytesSource);
								bytesSource = std::move(toBytes);
							}

							int offset = 0;
							for (auto const* fieldType: tupleType->types())
							{
								int fieldSize = 32; // ABI default: 32 bytes per field
								if (fieldType == awst::WType::boolType())
									fieldSize = 32;
								else if (fieldType == awst::WType::biguintType())
									fieldSize = 32;
								else if (fieldType && fieldType->kind() == awst::WTypeKind::Bytes)
								{
									auto const* bytesT = dynamic_cast<awst::BytesWType const*>(fieldType);
									if (bytesT && bytesT->length())
										fieldSize = static_cast<int>(*bytesT->length());
								}

								// extract3(source, offset, fieldSize) → bytes
								auto extract = std::make_shared<awst::IntrinsicCall>();
								extract->sourceLocation = loc;
								extract->wtype = awst::WType::bytesType();
								extract->opCode = "extract3";
								extract->stackArgs.push_back(bytesSource); // shared ptr copied
								auto offExpr = std::make_shared<awst::IntegerConstant>();
								offExpr->sourceLocation = loc;
								offExpr->wtype = awst::WType::uint64Type();
								offExpr->value = std::to_string(offset);
								extract->stackArgs.push_back(std::move(offExpr));
								auto lenExpr = std::make_shared<awst::IntegerConstant>();
								lenExpr->sourceLocation = loc;
								lenExpr->wtype = awst::WType::uint64Type();
								lenExpr->value = std::to_string(fieldSize);
								extract->stackArgs.push_back(std::move(lenExpr));

								// Cast extracted bytes to the target field type
								if (fieldType == awst::WType::biguintType())
								{
									auto cast = std::make_shared<awst::ReinterpretCast>();
									cast->sourceLocation = loc;
									cast->wtype = fieldType;
									cast->expr = std::move(extract);
									tuple->items.push_back(std::move(cast));
								}
								else if (fieldType == awst::WType::boolType())
								{
									// btoi(extracted) != 0
									auto btoi = std::make_shared<awst::IntrinsicCall>();
									btoi->sourceLocation = loc;
									btoi->wtype = awst::WType::uint64Type();
									btoi->opCode = "btoi";
									btoi->stackArgs.push_back(std::move(extract));
									auto zero = std::make_shared<awst::IntegerConstant>();
									zero->sourceLocation = loc;
									zero->wtype = awst::WType::uint64Type();
									zero->value = "0";
									auto cmp = std::make_shared<awst::NumericComparisonExpression>();
									cmp->sourceLocation = loc;
									cmp->wtype = awst::WType::boolType();
									cmp->lhs = std::move(btoi);
									cmp->rhs = std::move(zero);
									cmp->op = awst::NumericComparison::Ne;
									tuple->items.push_back(std::move(cmp));
								}
								else
								{
									auto cast = std::make_shared<awst::ReinterpretCast>();
									cast->sourceLocation = loc;
									cast->wtype = fieldType;
									cast->expr = std::move(extract);
									tuple->items.push_back(std::move(cast));
								}
								offset += fieldSize;
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

		// bytes.concat(a, b, ...) → chain of concat intrinsics
		// TypeType wrapping BytesType with concat member
		if (auto const* typeType = dynamic_cast<solidity::frontend::TypeType const*>(baseType))
		{
			auto const* actualType = typeType->actualType();
			if (actualType && actualType->category() == solidity::frontend::Type::Category::Array)
			{
				auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(actualType);
				if (arrType && arrType->isByteArrayOrString()
					&& memberAccess->memberName() == "concat")
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

					// Helper: convert expression to bytes
					auto toBytes = [&](std::shared_ptr<awst::Expression> expr) -> std::shared_ptr<awst::Expression> {
						if (expr->wtype == awst::WType::bytesType()
							|| (expr->wtype && expr->wtype->kind() == awst::WTypeKind::Bytes))
							return expr;
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
			}
		}
	}

	// Generic function call → SubroutineCallExpression
	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = loc;
	bool isUsingDirectiveCall = false;
	FunctionDefinition const* resolvedFuncDef = nullptr;

	if (auto const* identifier = dynamic_cast<Identifier const*>(&funcExpr))
	{
		std::string name = identifier->name();
		auto const* decl = identifier->annotation().referencedDeclaration;
		if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(decl))
		{
			resolvedFuncDef = funcDef;
			if (funcDef->returnParameters().empty())
				call->wtype = awst::WType::voidType();
			else if (funcDef->returnParameters().size() == 1)
				call->wtype = m_typeMapper.map(funcDef->returnParameters()[0]->type());
			else
			{
				// Multi-value return → WTuple
				std::vector<awst::WType const*> retTypes;
				for (auto const& param: funcDef->returnParameters())
					retTypes.push_back(m_typeMapper.map(param->type()));
				call->wtype = m_typeMapper.createType<awst::WTuple>(
					std::move(retTypes), std::nullopt
				);
			}

			// Check if this function belongs to a library (e.g. calling within a library)
			bool resolvedAsLibrary = false;
			if (auto const* scope = funcDef->scope())
			{
				if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(scope))
				{
					if (contractDef->isLibrary())
					{
						// Prefer AST ID lookup for precise overload resolution
						auto byId = m_freeFunctionById.find(funcDef->id());
						if (byId != m_freeFunctionById.end())
						{
							call->target = awst::SubroutineID{byId->second};
							resolvedAsLibrary = true;
						}
						else
						{
							std::string key = contractDef->name() + "." + funcDef->name();
							auto it = m_libraryFunctionIds.find(key);
							if (it == m_libraryFunctionIds.end())
							{
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
			}
			if (!resolvedAsLibrary && funcDef->isFree())
			{
				auto it = m_freeFunctionById.find(funcDef->id());
				if (it != m_freeFunctionById.end())
				{
					call->target = awst::SubroutineID{it->second};
					resolvedAsLibrary = true;
				}
			}
			if (!resolvedAsLibrary)
			{
				Logger::instance().debug("library resolution failed for '" + name + "', falling back to InstanceMethodTarget");
				call->target = awst::InstanceMethodTarget{resolveMethodName(*funcDef)};
			}
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
					// Set resolvedFuncDef for argument type coercion
					auto const* refDecl = memberAccess->annotation().referencedDeclaration;
					if (auto const* fd = dynamic_cast<FunctionDefinition const*>(refDecl))
						resolvedFuncDef = fd;

					// Prefer AST ID lookup for precise overload resolution
					if (resolvedFuncDef)
					{
						auto byId = m_freeFunctionById.find(resolvedFuncDef->id());
						if (byId != m_freeFunctionById.end())
						{
							call->target = awst::SubroutineID{byId->second};
							resolvedAsLibrary = true;
						}
					}
					if (!resolvedAsLibrary)
					{
						std::string key = contractDef->name() + "." + memberAccess->memberName();
						auto it = m_libraryFunctionIds.find(key);
						if (it == m_libraryFunctionIds.end())
						{
							size_t paramCount = _node.arguments().size();
							if (resolvedFuncDef)
								paramCount = resolvedFuncDef->parameters().size();
							key += "(" + std::to_string(paramCount) + ")";
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
		}
		// Also check if the member's referenced declaration is a library function
		// (handles `using Library for Type` and `using {func} for Type` patterns)
		if (!resolvedAsLibrary)
		{
			auto const* refDecl = memberAccess->annotation().referencedDeclaration;
			if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(refDecl))
			{
				resolvedFuncDef = funcDef;
				std::string dbgScope = "(no scope)";
				if (auto const* scope = funcDef->scope())
				{
					if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(scope))
					{
						dbgScope = contractDef->name() + (contractDef->isLibrary() ? " [lib]" : "");
						if (contractDef->isLibrary())
						{
							// Prefer AST ID lookup for precise overload resolution
							auto byId = m_freeFunctionById.find(funcDef->id());
							Logger::instance().debug("[RES] lib using-for: " + contractDef->name() + "." + funcDef->name() + " id=" + std::to_string(funcDef->id()) + " mapSize=" + std::to_string(m_freeFunctionById.size()) + " found=" + (byId != m_freeFunctionById.end() ? "YES" : "NO"));
							if (byId != m_freeFunctionById.end())
							{
								call->target = awst::SubroutineID{byId->second};
								resolvedAsLibrary = true;
								isUsingDirectiveCall = true;
							}
							else
							{
								std::string key = contractDef->name() + "." + funcDef->name();
								auto it = m_libraryFunctionIds.find(key);
								if (it == m_libraryFunctionIds.end())
								{
									key += "(" + std::to_string(funcDef->parameters().size()) + ")";
									it = m_libraryFunctionIds.find(key);
								}
								Logger::instance().debug("[RES] lib name fallback key='" + key + "' found=" + (it != m_libraryFunctionIds.end() ? "YES" : "NO"));
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
				// Check if it's a free function bound via `using {func} for Type`
				if (!resolvedAsLibrary && funcDef->isFree())
				{
					auto it = m_freeFunctionById.find(funcDef->id());
					Logger::instance().debug("[RES] free using-for: " + funcDef->name() + " id=" + std::to_string(funcDef->id()) + " found=" + (it != m_freeFunctionById.end() ? "YES" : "NO"));
					if (it != m_freeFunctionById.end())
					{
						call->target = awst::SubroutineID{it->second};
						resolvedAsLibrary = true;
						isUsingDirectiveCall = true;
					}
				}
				if (!resolvedAsLibrary)
				{
					Logger::instance().debug("[RES] FAILED for " + funcDef->name() + " id=" + std::to_string(funcDef->id()) + " scope=" + dbgScope + " isFree=" + (funcDef->isFree() ? "Y" : "N"));
				}
			}
		}
		if (!resolvedAsLibrary)
		{
			// Check if this is a super.method() call
			auto const* baseType = memberAccess->expression().annotation().type;
			// Unwrap TypeType if needed (super has type TypeType(ContractType(isSuper=true)))
			if (baseType && baseType->category() == solidity::frontend::Type::Category::TypeType)
			{
				auto const* typeType = dynamic_cast<solidity::frontend::TypeType const*>(baseType);
				if (typeType)
					baseType = typeType->actualType();
			}
			bool isSuperCall = false;
			if (baseType && baseType->category() == solidity::frontend::Type::Category::Contract)
			{
				auto const* contractType = dynamic_cast<solidity::frontend::ContractType const*>(baseType);
				if (contractType && contractType->isSuper())
				{
					isSuperCall = true;
					auto const* refDecl = memberAccess->annotation().referencedDeclaration;
					if (auto const* funcDef = dynamic_cast<solidity::frontend::FunctionDefinition const*>(refDecl))
					{
						resolvedFuncDef = funcDef;
						// Look up the super target name by AST ID
						auto it = m_superTargetNames.find(funcDef->id());
						if (it != m_superTargetNames.end())
						{
							call->target = awst::InstanceMethodTarget{it->second};
						}
						else
						{
							// Fallback: use resolved name (will be same as override — bug)
							Logger::instance().warning("super call target not registered for " + funcDef->name(), loc);
							call->target = awst::InstanceMethodTarget{resolveMethodName(*funcDef)};
						}
					}
				}
			}
			// Check if base is an interface/contract type (external call)
			bool isExternalCall = false;
			if (!isSuperCall && baseType && baseType->category() == solidity::frontend::Type::Category::Contract)
			{
				auto const* contractType = dynamic_cast<solidity::frontend::ContractType const*>(baseType);
				if (contractType && contractType->contractDefinition().isInterface())
					isExternalCall = true;
			}
			if (isExternalCall)
			{
				// External interface call → application call inner transaction
				auto baseTranslated = translate(memberAccess->expression());

				// Build ARC4 method selector string.
				// Must use ARC4 type names (matching puya output) not Solidity names.
				auto solTypeToARC4Name = [this](solidity::frontend::Type const* _type) -> std::string {
					auto* wtype = m_typeMapper.map(_type);
					if (wtype == awst::WType::biguintType())
						return "uint256";
					if (wtype == awst::WType::uint64Type())
						return "uint64";
					if (wtype == awst::WType::boolType())
						return "bool";
					if (wtype == awst::WType::accountType())
						return "address";
					if (wtype == awst::WType::bytesType())
						return "byte[]";
					if (wtype == awst::WType::stringType())
						return "string";
					if (wtype->kind() == awst::WTypeKind::Bytes)
					{
						auto const* bw = static_cast<awst::BytesWType const*>(wtype);
						if (bw->length().has_value())
							return "byte[" + std::to_string(bw->length().value()) + "]";
						return "byte[]";
					}
					// Struct types: use short name to match callee's compiled method selector
					if (auto const* structType = dynamic_cast<solidity::frontend::StructType const*>(_type))
						return "struct " + structType->structDefinition().name();
					// Fallback: use Solidity name
					return _type->toString(true);
				};

				std::string methodSelector = memberAccess->memberName() + "(";
				auto const* extRefDecl = memberAccess->annotation().referencedDeclaration;
				if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(extRefDecl))
				{
					bool first = true;
					for (auto const& param: funcDef->parameters())
					{
						if (!first) methodSelector += ",";
						methodSelector += solTypeToARC4Name(param->type());
						first = false;
					}
					methodSelector += ")";
					if (funcDef->returnParameters().size() > 1)
					{
						// Multiple return values → tuple: (type1,type2,...)
						methodSelector += "(";
						bool firstRet = true;
						for (auto const& retParam: funcDef->returnParameters())
						{
							if (!firstRet) methodSelector += ",";
							methodSelector += solTypeToARC4Name(retParam->type());
							firstRet = false;
						}
						methodSelector += ")";
					}
					else if (funcDef->returnParameters().size() == 1)
						methodSelector += solTypeToARC4Name(funcDef->returnParameters()[0]->type());
					else
						methodSelector += "void";
				}
				else
					methodSelector += ")void";

				auto methodConst = std::make_shared<awst::MethodConstant>();
				methodConst->sourceLocation = loc;
				methodConst->wtype = awst::WType::bytesType();
				methodConst->value = methodSelector;

				// ApplicationArgs must be a WTuple of bytes values
				auto argsTuple = std::make_shared<awst::TupleExpression>();
				argsTuple->sourceLocation = loc;
				argsTuple->items.push_back(std::move(methodConst));
				// Add actual call arguments (converted to bytes for ApplicationArgs)
				for (auto const& arg: _node.arguments())
				{
					auto argExpr = translate(*arg);
					// Inner transaction ApplicationArgs must all be bytes.
					if (argExpr->wtype == awst::WType::bytesType()
						|| argExpr->wtype->kind() == awst::WTypeKind::Bytes)
					{
						argsTuple->items.push_back(std::move(argExpr));
					}
					else if (argExpr->wtype == awst::WType::uint64Type())
					{
						// uint64 → itob → bytes
						auto itob = std::make_shared<awst::IntrinsicCall>();
						itob->sourceLocation = loc;
						itob->wtype = awst::WType::bytesType();
						itob->opCode = "itob";
						itob->stackArgs.push_back(std::move(argExpr));
						argsTuple->items.push_back(std::move(itob));
					}
					else if (argExpr->wtype == awst::WType::biguintType())
					{
						// biguint → ARC4 uint256 = 32 bytes, left-padded with zeros
						// concat(bzero(32), reinterpret_as_bytes(value)) → extract last 32 bytes
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(argExpr);

						auto zeros = std::make_shared<awst::IntrinsicCall>();
						zeros->sourceLocation = loc;
						zeros->wtype = awst::WType::bytesType();
						zeros->opCode = "bzero";
						zeros->stackArgs.push_back(makeUint64("32", loc));

						auto padded = std::make_shared<awst::IntrinsicCall>();
						padded->sourceLocation = loc;
						padded->wtype = awst::WType::bytesType();
						padded->opCode = "concat";
						padded->stackArgs.push_back(std::move(zeros));
						padded->stackArgs.push_back(std::move(cast));

						auto lenCall = std::make_shared<awst::IntrinsicCall>();
						lenCall->sourceLocation = loc;
						lenCall->wtype = awst::WType::uint64Type();
						lenCall->opCode = "len";
						lenCall->stackArgs.push_back(padded);

						auto offset = std::make_shared<awst::IntrinsicCall>();
						offset->sourceLocation = loc;
						offset->wtype = awst::WType::uint64Type();
						offset->opCode = "-";
						offset->stackArgs.push_back(std::move(lenCall));
						offset->stackArgs.push_back(makeUint64("32", loc));

						auto extracted = std::make_shared<awst::IntrinsicCall>();
						extracted->sourceLocation = loc;
						extracted->wtype = awst::WType::bytesType();
						extracted->opCode = "extract3";
						extracted->stackArgs.push_back(std::move(padded));
						extracted->stackArgs.push_back(std::move(offset));
						extracted->stackArgs.push_back(makeUint64("32", loc));
						
						argsTuple->items.push_back(std::move(extracted));
					}
					else if (argExpr->wtype == awst::WType::boolType())
					{
						// bool → ARC4 bool = 1 byte: 0x80 for true, 0x00 for false
						// setbit(byte 0x00, 0, boolValue)
						auto zeroByte = std::make_shared<awst::BytesConstant>();
						zeroByte->sourceLocation = loc;
						zeroByte->wtype = awst::WType::bytesType();
						zeroByte->encoding = awst::BytesEncoding::Base16;
						zeroByte->value = {0x00};
						
						auto setbit = std::make_shared<awst::IntrinsicCall>();
						setbit->sourceLocation = loc;
						setbit->wtype = awst::WType::bytesType();
						setbit->opCode = "setbit";
						setbit->stackArgs.push_back(std::move(zeroByte));
						setbit->stackArgs.push_back(makeUint64("0", loc));
						setbit->stackArgs.push_back(std::move(argExpr));
						
						argsTuple->items.push_back(std::move(setbit));
					}
					else if (argExpr->wtype->kind() == awst::WTypeKind::ReferenceArray
						|| argExpr->wtype->kind() == awst::WTypeKind::ARC4StaticArray
						|| argExpr->wtype->kind() == awst::WTypeKind::ARC4DynamicArray
						|| argExpr->wtype->kind() == awst::WTypeKind::ARC4Struct
						|| argExpr->wtype->kind() == awst::WTypeKind::ARC4Tuple)
					{
						// Complex types: use abi.encodePacked equivalent.
						// For inner txn args, serialize via concatenation of
						// element bytes. Use ReinterpretCast to biguint then
						// to bytes as a simple serialization.
						// For ARC4 types, they're already bytes-backed.
						if (argExpr->wtype->kind() == awst::WTypeKind::ReferenceArray)
						{
							// For reference arrays, just pass an empty bytes placeholder.
							// Full array serialization would require iterating elements.
							// This is a limitation — inner txn args with array types
							// need proper ABI encoding.
							auto placeholder = std::make_shared<awst::BytesConstant>();
							placeholder->sourceLocation = loc;
							placeholder->wtype = awst::WType::bytesType();
							placeholder->encoding = awst::BytesEncoding::Base16;
							placeholder->value = {};
							argsTuple->items.push_back(std::move(placeholder));
						}
						else
						{
							// ARC4 types are bytes-backed, reinterpret is fine
							auto cast = std::make_shared<awst::ReinterpretCast>();
							cast->sourceLocation = loc;
							cast->wtype = awst::WType::bytesType();
							cast->expr = std::move(argExpr);
							argsTuple->items.push_back(std::move(cast));
						}
					}
					else
					{
						// Fallback: try reinterpret as bytes
						auto cast = std::make_shared<awst::ReinterpretCast>();
						cast->sourceLocation = loc;
						cast->wtype = awst::WType::bytesType();
						cast->expr = std::move(argExpr);
						argsTuple->items.push_back(std::move(cast));
					}
				}
				// Build WTuple type for the args (all bytes)
				std::vector<awst::WType const*> argTypes;
				for (auto const& item: argsTuple->items)
					argTypes.push_back(item->wtype);
				argsTuple->wtype = m_typeMapper.createType<awst::WTuple>(
					std::move(argTypes), std::nullopt
				);

				// Convert address/account to application ID for inner transaction.
				// On Algorand, interface-typed variables should hold app IDs (uint64).
				// Use an intrinsic call (btoi) to convert the bytes-backed account
				// to a uint64, then reinterpret as application type.
				std::shared_ptr<awst::Expression> appId;
				if (baseTranslated->wtype == awst::WType::applicationType())
				{
					appId = std::move(baseTranslated);
				}
				else
				{
					// Convert bytes-backed type (account/address) to app ID (uint64).
					// Addresses are 32 bytes but app IDs are stored as big-endian,
					// so extract the last 8 bytes then btoi.
					std::shared_ptr<awst::Expression> bytesExpr = std::move(baseTranslated);
					// If account type, reinterpret as bytes first
					if (bytesExpr->wtype == awst::WType::accountType())
					{
						auto toBytes = std::make_shared<awst::ReinterpretCast>();
						toBytes->sourceLocation = loc;
						toBytes->wtype = awst::WType::bytesType();
						toBytes->expr = std::move(bytesExpr);
						bytesExpr = std::move(toBytes);
					}
					// extract last 8 bytes (offset 24, length 8) from 32-byte address
					auto extract = std::make_shared<awst::IntrinsicCall>();
					extract->sourceLocation = loc;
					extract->wtype = awst::WType::bytesType();
					extract->opCode = "extract";
					extract->immediates = {24, 8};
					extract->stackArgs.push_back(std::move(bytesExpr));

					auto btoi = std::make_shared<awst::IntrinsicCall>();
					btoi->sourceLocation = loc;
					btoi->wtype = awst::WType::uint64Type();
					btoi->opCode = "btoi";
					btoi->stackArgs.push_back(std::move(extract));

					// Reinterpret uint64 as application (both are uint64-backed)
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = loc;
					cast->wtype = awst::WType::applicationType();
					cast->expr = std::move(btoi);
					appId = std::move(cast);
				}

				std::map<std::string, std::shared_ptr<awst::Expression>> fields;
				fields["ApplicationID"] = std::move(appId);
				fields["OnCompletion"] = makeUint64("0", loc); // NoOp
				fields["ApplicationArgs"] = std::move(argsTuple);

				auto create = buildCreateInnerTransaction(
					TxnTypeAppl, std::move(fields), loc
				);
				auto* retType = m_typeMapper.map(_node.annotation().type);
				push(buildSubmitAndReturn(std::move(create), retType, loc));
				return false;
			}
			if (!isSuperCall)
			{
				// Last resort: try library/free function resolution by AST ID
				auto const* refDecl = memberAccess->annotation().referencedDeclaration;
				bool resolvedHere = false;
				if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(refDecl))
				{
					resolvedFuncDef = funcDef;
					// Try AST ID lookup in free functions
					auto byId = m_freeFunctionById.find(funcDef->id());
					if (byId != m_freeFunctionById.end())
					{
						call->target = awst::SubroutineID{byId->second};
						resolvedHere = true;
						isUsingDirectiveCall = true;
					}
					else
					{
						// Try library function map with scope
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
										key += "(" + std::to_string(funcDef->parameters().size()) + ")";
										it = m_libraryFunctionIds.find(key);
									}
									if (it != m_libraryFunctionIds.end())
									{
										call->target = awst::SubroutineID{it->second};
										resolvedHere = true;
										isUsingDirectiveCall = true;
									}
								}
							}
						}
					}
				}
				if (!resolvedHere)
				{
					std::string methodName = memberAccess->memberName();
					if (resolvedFuncDef)
						methodName = resolveMethodName(*resolvedFuncDef);
					Logger::instance().debug("library resolution failed for member '" + methodName + "', falling back to InstanceMethodTarget");
					call->target = awst::InstanceMethodTarget{methodName};
				}
			}
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

	// Collect parameter types from resolved function definition for type coercion
	std::vector<awst::WType const*> paramTypes;
	if (resolvedFuncDef)
	{
		for (auto const& param: resolvedFuncDef->parameters())
			paramTypes.push_back(m_typeMapper.map(param->type()));
	}

	for (size_t i = 0; i < _node.arguments().size(); ++i)
	{
		awst::CallArg ca;
		ca.value = translate(*_node.arguments()[i]);
		// Coerce argument type to match callee parameter type if needed
		// For `using` directive calls, param index 0 is the receiver (already prepended above),
		// so call-site arg[i] maps to paramTypes[i+1]
		size_t paramIdx = isUsingDirectiveCall ? (i + 1) : i;
		if (paramIdx < paramTypes.size())
		{
			ca.value = implicitNumericCast(std::move(ca.value), paramTypes[paramIdx], loc);
		}
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

	// Enum member access: MyEnum.Value → integer constant
	if (auto const* enumVal = dynamic_cast<solidity::frontend::EnumValue const*>(
		_node.annotation().referencedDeclaration))
	{
		// Find the enum definition to determine the member's ordinal index
		auto const* enumDef = dynamic_cast<solidity::frontend::EnumDefinition const*>(
			enumVal->scope()
		);
		if (enumDef)
		{
			int index = 0;
			for (auto const& member: enumDef->members())
			{
				if (member.get() == enumVal)
					break;
				++index;
			}
			auto e = std::make_shared<awst::IntegerConstant>();
			e->sourceLocation = loc;
			e->wtype = awst::WType::uint64Type();
			e->value = std::to_string(index);
			push(e);
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
	if (memberName == "interfaceId" && baseType)
	{
		uint32_t interfaceIdValue = 0;
		// type(X) produces a MagicType with Kind::MetaType
		if (auto const* magicType = dynamic_cast<solidity::frontend::MagicType const*>(baseType))
		{
			if (auto const* arg = magicType->typeArgument())
				if (auto const* contractType = dynamic_cast<solidity::frontend::ContractType const*>(arg))
					interfaceIdValue = contractType->contractDefinition().interfaceId();
		}
		// Also handle TypeType (for completeness)
		else if (auto const* typeType = dynamic_cast<solidity::frontend::TypeType const*>(baseType))
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

	// General field access — valid for struct/tuple base types (WTuple or ARC4Struct)
	auto base = translate(baseExpr);
	if (base->wtype && base->wtype->kind() == awst::WTypeKind::ARC4Struct)
	{
		auto const* structType = static_cast<awst::ARC4Struct const*>(base->wtype);
		// Look up the ARC4 field type from the struct definition
		awst::WType const* arc4FieldType = nullptr;
		for (auto const& [fname, ftype]: structType->fields())
			if (fname == memberName)
			{
				arc4FieldType = ftype;
				break;
			}
		auto field = std::make_shared<awst::FieldExpression>();
		field->sourceLocation = loc;
		field->base = std::move(base);
		field->name = memberName;
		field->wtype = arc4FieldType ? arc4FieldType : m_typeMapper.map(_node.annotation().type);
		// Wrap in ARC4Decode to convert back to native type for use in expressions
		auto* nativeType = m_typeMapper.map(_node.annotation().type);
		if (arc4FieldType && arc4FieldType != nativeType)
		{
			auto decode = std::make_shared<awst::ARC4Decode>();
			decode->sourceLocation = loc;
			decode->wtype = nativeType;
			decode->value = std::move(field);
			push(decode);
		}
		else
			push(field);
		return false;
	}
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
				if (varDecl->isStateVariable() && arrType->isDynamicallySized()
				&& !varDecl->isConstant() && !varDecl->immutable())
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
		// cursor should now be the root Identifier, or a MemberAccess for struct.mapping access
		if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(cursor))
			varName = ident->name();
		else if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(cursor))
		{
			// Handle struct.mapping[key] pattern, e.g. self.sideNodes[level]
			// Build varName from the member name (the mapping field name)
			varName = ma->memberName();
		}

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
				std::shared_ptr<awst::Expression> keyBytes;
				if (translated->wtype == awst::WType::uint64Type())
				{
					// uint64 → bytes via itob intrinsic
					auto itob = std::make_shared<awst::IntrinsicCall>();
					itob->sourceLocation = loc;
					itob->wtype = awst::WType::bytesType();
					itob->opCode = "itob";
					itob->stackArgs.push_back(std::move(translated));
					keyBytes = std::move(itob);
				}
				else if (translated->wtype == awst::WType::biguintType())
				{
					// biguint → normalize to exactly 32 bytes before hashing.
					// AVM biguint ops (b+, b-) produce minimal-length bytes via
					// big.Int.Bytes(), while itob produces 8 bytes. Without
					// normalization, the same number gets different sha256 hashes.
					// Pattern: concat(bzero(32), bytes) → extract last 32 bytes.
					auto reinterpret = std::make_shared<awst::ReinterpretCast>();
					reinterpret->sourceLocation = loc;
					reinterpret->wtype = awst::WType::bytesType();
					reinterpret->expr = std::move(translated);

					auto padWidth = std::make_shared<awst::IntegerConstant>();
					padWidth->sourceLocation = loc;
					padWidth->wtype = awst::WType::uint64Type();
					padWidth->value = "32";

					auto pad = std::make_shared<awst::IntrinsicCall>();
					pad->sourceLocation = loc;
					pad->wtype = awst::WType::bytesType();
					pad->opCode = "bzero";
					pad->stackArgs.push_back(std::move(padWidth));

					auto cat = std::make_shared<awst::IntrinsicCall>();
					cat->sourceLocation = loc;
					cat->wtype = awst::WType::bytesType();
					cat->opCode = "concat";
					cat->stackArgs.push_back(std::move(pad));
					cat->stackArgs.push_back(std::move(reinterpret));

					auto lenCall = std::make_shared<awst::IntrinsicCall>();
					lenCall->sourceLocation = loc;
					lenCall->wtype = awst::WType::uint64Type();
					lenCall->opCode = "len";
					lenCall->stackArgs.push_back(cat);

					auto width2 = std::make_shared<awst::IntegerConstant>();
					width2->sourceLocation = loc;
					width2->wtype = awst::WType::uint64Type();
					width2->value = "32";

					auto offset = std::make_shared<awst::IntrinsicCall>();
					offset->sourceLocation = loc;
					offset->wtype = awst::WType::uint64Type();
					offset->opCode = "-";
					offset->stackArgs.push_back(std::move(lenCall));
					offset->stackArgs.push_back(std::move(width2));

					auto width3 = std::make_shared<awst::IntegerConstant>();
					width3->sourceLocation = loc;
					width3->wtype = awst::WType::uint64Type();
					width3->value = "32";

					auto extract = std::make_shared<awst::IntrinsicCall>();
					extract->sourceLocation = loc;
					extract->wtype = awst::WType::bytesType();
					extract->opCode = "extract3";
					extract->stackArgs.push_back(std::move(cat));
					extract->stackArgs.push_back(std::move(offset));
					extract->stackArgs.push_back(std::move(width3));

					keyBytes = std::move(extract);
				}
				else
				{
					// bytes / address / other → ReinterpretCast to bytes
					auto reinterpret = std::make_shared<awst::ReinterpretCast>();
					reinterpret->sourceLocation = loc;
					reinterpret->wtype = awst::WType::bytesType();
					reinterpret->expr = std::move(translated);
					keyBytes = std::move(reinterpret);
				}
				keyParts.push_back(std::move(keyBytes));
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
			}

			// Hash the key with sha256 to fit within 64-byte box name limit.
			// prefix(varName) + raw key could exceed 64 bytes
			// (e.g. "registeredUsers" (15) + uint256 (32) = 47 bytes).
			// sha256 produces 32 bytes, so prefix + 32 always fits.
			{
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
		index = implicitNumericCast(std::move(index), awst::WType::uint64Type(), loc);

	auto* expectedType = m_typeMapper.map(_node.annotation().type);

	// Determine the actual element type from the base array
	auto* actualElemType = expectedType;
	if (base->wtype && base->wtype->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = static_cast<awst::ReferenceArray const*>(base->wtype);
		actualElemType = const_cast<awst::WType*>(refArr->elementType());
	}

	auto e = std::make_shared<awst::IndexExpression>();
	e->sourceLocation = loc;
	e->base = std::move(base);
	e->index = std::move(index);
	e->wtype = actualElemType;

	// If the element is ARC4 (e.g. from 2D array) but expected type is decoded,
	// wrap in ARC4Decode to convert to the expected reference type
	if (actualElemType != expectedType
		&& actualElemType->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		auto decode = std::make_shared<awst::ARC4Decode>();
		decode->sourceLocation = loc;
		decode->wtype = expectedType;
		decode->value = std::move(e);
		push(decode);
	}
	else
	{
		push(e);
	}
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

	// Coerce start/end to uint64 if needed (e.g. biguint from uint256 params)
	start = implicitNumericCast(std::move(start), awst::WType::uint64Type(), loc);
	end = implicitNumericCast(std::move(end), awst::WType::uint64Type(), loc);

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
	// Ensure both branches match the result type (e.g., uint64 literal → biguint)
	e->trueExpr = implicitNumericCast(std::move(e->trueExpr), e->wtype, loc);
	e->falseExpr = implicitNumericCast(std::move(e->falseExpr), e->wtype, loc);
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

	// Tuple assignment decomposition: (a, b, c) = func()
	// When the LHS is a tuple expression, decompose into individual assignments
	// to avoid issues with non-lvalue items (e.g. storage targets).
	if (auto const* tupleTarget = dynamic_cast<awst::TupleExpression const*>(target.get()))
	{
		auto const& items = tupleTarget->items;
		for (size_t i = 0; i < items.size(); ++i)
		{
			auto item = items[i];

			// Extract tuple element
			auto itemExpr = std::make_shared<awst::TupleItemExpression>();
			itemExpr->sourceLocation = loc;
			itemExpr->wtype = item->wtype;
			itemExpr->base = value;
			itemExpr->index = static_cast<int>(i);

			// Unwrap ARC4Decode for storage targets
			auto assignTarget = item;
			if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(item.get()))
				assignTarget = decodeExpr->value;
			// Unwrap StateGet for storage targets
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(assignTarget.get()))
				assignTarget = sg->field;

			// Encode value if target is ARC4 storage
			std::shared_ptr<awst::Expression> assignValue = std::move(itemExpr);
			if (assignTarget->wtype != assignValue->wtype)
			{
				// Need ARC4 encode for storage writes
				bool targetIsArc4 = false;
				switch (assignTarget->wtype->kind())
				{
				case awst::WTypeKind::ARC4UIntN:
				case awst::WTypeKind::ARC4StaticArray:
				case awst::WTypeKind::ARC4DynamicArray:
				case awst::WTypeKind::ARC4Struct:
					targetIsArc4 = true;
					break;
				default:
					break;
				}
				if (targetIsArc4)
				{
					auto encode = std::make_shared<awst::ARC4Encode>();
					encode->sourceLocation = loc;
					encode->wtype = assignTarget->wtype;
					encode->value = std::move(assignValue);
					assignValue = std::move(encode);
				}
				else
				{
					assignValue = implicitNumericCast(std::move(assignValue), assignTarget->wtype, loc);
				}
			}

			auto e = std::make_shared<awst::AssignmentExpression>();
			e->sourceLocation = loc;
			e->wtype = assignTarget->wtype;
			e->target = std::move(assignTarget);
			e->value = std::move(assignValue);
			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = loc;
			stmt->expr = e;
			m_pendingStatements.push_back(std::move(stmt));
		}
		// Push a void expression as the result (this is used in statement context)
		push(value);
		return false;
	}

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

	// Unwrap ARC4Decode for assignment targets — ARC4Decode is not an Lvalue.
	// When assigning to a struct field, the target is ARC4Decode(FieldExpression).
	std::shared_ptr<awst::Expression> unwrappedTarget = target;
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		unwrappedTarget = decodeExpr->value;

	// Check if target is a FieldExpression on a struct (field assignment).
	// Structs are immutable in Puya, so we use copy-on-write: build a new struct
	// with all fields copied except the modified one, then assign the whole struct.
	if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(unwrappedTarget.get()))
	{
		// ARC4Struct field assignment: copy-on-write with NewStruct
		auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
		if (arc4StructType)
		{
			auto base = fieldExpr->base;
			std::string fieldName = fieldExpr->name;

			// Ensure base is readable for field extraction.
			// If base is a bare BoxValueExpression (e.g. direct _mapping[key].field = value
			// with willBeWrittenTo=true), wrap in StateGet for reading.
			auto readBase = base;
			if (dynamic_cast<awst::BoxValueExpression const*>(base.get()))
			{
				auto stateGet = std::make_shared<awst::StateGet>();
				stateGet->sourceLocation = loc;
				stateGet->wtype = base->wtype;
				stateGet->field = base;
				stateGet->defaultValue = StorageMapper::makeDefaultValue(base->wtype, loc);
				readBase = stateGet;
			}

			if (op != Token::Assign)
			{
				auto currentField = std::make_shared<awst::FieldExpression>();
				currentField->sourceLocation = loc;
				currentField->base = readBase;
				currentField->name = fieldName;
				currentField->wtype = fieldExpr->wtype;
				// Decode the current field value for the binary op (it's ARC4)
				auto decoded = std::make_shared<awst::ARC4Decode>();
				decoded->sourceLocation = loc;
				decoded->wtype = m_typeMapper.map(_node.leftHandSide().annotation().type);
				decoded->value = std::move(currentField);
				value = buildBinaryOp(op, std::move(decoded), std::move(value), decoded->wtype, loc);
			}

			// Wrap value in ARC4Encode to match the ARC4 field type
			awst::WType const* arc4FieldType = nullptr;
			for (auto const& [fname, ftype]: arc4StructType->fields())
				if (fname == fieldName)
				{
					arc4FieldType = ftype;
					break;
				}
			if (arc4FieldType && value->wtype != arc4FieldType)
			{
				auto encode = std::make_shared<awst::ARC4Encode>();
				encode->sourceLocation = loc;
				encode->wtype = arc4FieldType;
				encode->value = std::move(value);
				value = std::move(encode);
			}

			// Build NewStruct with all fields copied, replacing the modified one
			auto newStruct = std::make_shared<awst::NewStruct>();
			newStruct->sourceLocation = loc;
			newStruct->wtype = arc4StructType;
			for (auto const& [fname, ftype]: arc4StructType->fields())
			{
				if (fname == fieldName)
					newStruct->values[fname] = std::move(value);
				else
				{
					auto field = std::make_shared<awst::FieldExpression>();
					field->sourceLocation = loc;
					field->base = readBase;
					field->name = fname;
					field->wtype = ftype;
					newStruct->values[fname] = std::move(field);
				}
			}

			auto writeTarget = base;
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
				writeTarget = sg->field;

			auto e = std::make_shared<awst::AssignmentExpression>();
			e->sourceLocation = loc;
			e->wtype = writeTarget->wtype;
			e->target = std::move(writeTarget);
			e->value = std::move(newStruct);
			push(e);
			return false;
		}

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
		auto currentValue = translate(_node.leftHandSide());
		// For box-stored mappings, the re-translated LHS is a BoxValueExpression
		// (no StateGet default) because willBeWrittenTo=true. Wrap it in StateGet
		// so missing boxes return the default value instead of asserting existence.
		if (dynamic_cast<awst::BoxValueExpression const*>(currentValue.get()))
		{
			auto defaultVal = StorageMapper::makeDefaultValue(currentValue->wtype, loc);
			auto stateGet = std::make_shared<awst::StateGet>();
			stateGet->sourceLocation = loc;
			stateGet->wtype = currentValue->wtype;
			stateGet->field = currentValue;
			stateGet->defaultValue = defaultVal;
			currentValue = std::move(stateGet);
		}
		value = buildBinaryOp(op, std::move(currentValue), std::move(value), target->wtype, loc);
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

	// Unwrap ARC4Decode for assignment targets — ARC4Decode is not an Lvalue.
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		target = decodeExpr->value;

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

	// Inline array literals: [val1, val2, ...] → NewArray
	// Must check this before the single-element parenthesization check
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

	if (_node.components().size() == 1 && _node.components()[0])
	{
		// Single-element tuple is just parenthesization
		push(translate(*_node.components()[0]));
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

	// For void returns: return submit directly
	if (!_solidityReturnType || _solidityReturnType == awst::WType::voidType())
		return submit;

	// For bool returns on non-app-call txns (transfer, send): return true
	if (_solidityReturnType == awst::WType::boolType()
		&& (!txnType.has_value() || txnType.value() != 6))
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

	// For application calls with return values: extract from LastLog
	if (txnType.has_value() && txnType.value() == 6) // Application call
	{
		// Submit as a pending statement to prevent duplication
		auto submitStmt = std::make_shared<awst::ExpressionStatement>();
		submitStmt->sourceLocation = _loc;
		submitStmt->expr = std::move(submit);
		m_pendingStatements.push_back(std::move(submitStmt));

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
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(stripPrefix);
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
			// Extract bit 0 (MSB) using getbit to get 0 or 1.
			auto getbit = std::make_shared<awst::IntrinsicCall>();
			getbit->sourceLocation = _loc;
			getbit->opCode = "getbit";
			getbit->wtype = awst::WType::uint64Type();
			getbit->stackArgs.push_back(std::move(stripPrefix));
			getbit->stackArgs.push_back(makeUint64("0", _loc));
			return getbit;
		}
		else if (_solidityReturnType == awst::WType::accountType())
		{
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::accountType();
			cast->expr = std::move(stripPrefix);
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
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = _loc;
					cast->wtype = awst::WType::biguintType();
					cast->expr = std::move(extract);
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
					fieldExpr = std::move(getbit);
				}
				else if (fieldType == awst::WType::accountType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = _loc;
					cast->wtype = awst::WType::accountType();
					cast->expr = std::move(extract);
					fieldExpr = std::move(cast);
				}
				else
				{
					// bytes or fixed bytes — cast to target type
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = _loc;
					cast->wtype = fieldType;
					cast->expr = std::move(extract);
					fieldExpr = std::move(cast);
				}

				tuple->items.push_back(std::move(fieldExpr));
				offset += fieldSize;
			}

			return tuple;
		}

		// For bytes/string: return stripped bytes directly
		return stripPrefix;
	}

	// For non-appl returns: emit submit as pending, return type-appropriate default
	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = _loc;
	stmt->expr = std::move(submit);
	m_pendingStatements.push_back(std::move(stmt));

	return StorageMapper::makeDefaultValue(_solidityReturnType, _loc);
}

} // namespace puyasol::builder
