#include "builder/AssemblyTranslator.h"
#include "Logger.h"

#include <liblangutil/DebugData.h>

#include <sstream>

namespace puyasol::builder
{

AssemblyTranslator::AssemblyTranslator(
	TypeMapper& _typeMapper,
	std::string const& _sourceFile,
	std::string const& _contextName
)
	: m_typeMapper(_typeMapper), m_sourceFile(_sourceFile), m_contextName(_contextName)
{
}

// ─── Public entry point ─────────────────────────────────────────────────────

std::vector<std::shared_ptr<awst::Statement>> AssemblyTranslator::translateBlock(
	solidity::yul::Block const& _block,
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	awst::WType const* _returnType,
	std::map<std::string, std::string> const& _constants
)
{
	m_returnType = _returnType;
	m_locals.clear();
	m_localConstants.clear();
	m_memoryMap.clear();
	m_calldataMap.clear();
	m_asmFunctions.clear();
	m_ownedTypes.clear();
	m_upgradedLocals.clear();
	m_constants = _constants;

	// Register function parameters as known locals (so they resolve with proper types)
	for (auto const& [name, type]: _params)
		m_locals[name] = type ? type : awst::WType::biguintType();

	initializeMemoryMap(_params);
	initializeCalldataMap(_params);

	// First pass: collect assembly function definitions
	for (auto const& stmt: _block.statements)
	{
		if (auto const* funcDef = std::get_if<solidity::yul::FunctionDefinition>(&stmt))
			m_asmFunctions[funcDef->name.str()] = funcDef;
	}

	// Second pass: translate statements (skipping function definitions)
	std::vector<std::shared_ptr<awst::Statement>> result;

	// Emit __free_memory_ptr declaration so puya knows it's assigned before use
	{
		awst::SourceLocation loc;
		loc.file = m_sourceFile;

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = loc;
		target->name = "__free_memory_ptr";
		target->wtype = awst::WType::biguintType();

		auto val = std::make_shared<awst::IntegerConstant>();
		val->sourceLocation = loc;
		val->wtype = awst::WType::biguintType();
		val->value = "128"; // 0x80

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = loc;
		assign->target = std::move(target);
		assign->value = std::move(val);
		result.push_back(std::move(assign));

		m_locals["__free_memory_ptr"] = awst::WType::biguintType();
	}

	for (auto const& stmt: _block.statements)
	{
		if (std::holds_alternative<solidity::yul::FunctionDefinition>(stmt))
			continue; // Already collected in first pass
		translateStatement(stmt, result);
	}

	// Coerce upgraded variables back to their original types at block end.
	// Within the assembly block, variables may be promoted to biguint for 256-bit
	// Yul semantics. After the block, Solidity code expects the original types.
	for (auto const& [name, origType]: m_upgradedLocals)
	{
		awst::SourceLocation loc;
		loc.file = m_sourceFile;

		// Read the biguint-typed variable
		auto src = std::make_shared<awst::VarExpression>();
		src->sourceLocation = loc;
		src->name = name;
		src->wtype = awst::WType::biguintType();

		// Convert to original type (uint64)
		auto converted = safeBtoi(std::move(src), loc);

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = loc;
		target->name = name;
		target->wtype = origType;

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = loc;
		assign->target = std::move(target);
		assign->value = std::move(converted);
		result.push_back(std::move(assign));

		// Restore the type in m_locals
		m_locals[name] = origType;
	}

	return result;
}

// ─── Memory model ───────────────────────────────────────────────────────────

void AssemblyTranslator::initializeMemoryMap(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params
)
{
	// Pre-initialize the free memory pointer slot (EVM convention: 0x40 = 0x80)
	{
		MemorySlot fmpSlot;
		fmpSlot.varName = "__free_memory_ptr";
		m_memoryMap[0x40] = fmpSlot;
		m_localConstants["__free_memory_ptr"] = 0x80;
	}

	// In Solidity's ABI calling convention for memory arrays:
	//   mload(0x80 + i*0x20) = array element i
	// We look for the first array parameter and map its elements.
	// Note: for calldata parameters, the calldataMap is used instead.
	for (auto const& [name, type]: _params)
	{
		if (type && type->kind() == awst::WTypeKind::ReferenceArray)
		{
			auto const* refArray = dynamic_cast<awst::ReferenceArray const*>(type);
			if (!refArray)
				continue;

			m_arrayParamName = name;
			m_arrayParamType = type;
			m_arrayParamSize = refArray->arraySize().value_or(0);

			for (int i = 0; i < m_arrayParamSize; ++i)
			{
				uint64_t offset = 0x80 + static_cast<uint64_t>(i) * 0x20;
				MemorySlot slot;
				slot.varName = name;
				slot.isParam = true;
				slot.paramIndex = i;
				m_memoryMap[offset] = slot;
			}
			break; // Only handle the first array param
		}
	}
}

int AssemblyTranslator::computeFlatElementCount(awst::WType const* _type)
{
	if (!_type)
		return 1;
	if (_type->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(_type);
		if (refArr && refArr->arraySize())
			return *refArr->arraySize() * computeFlatElementCount(refArr->elementType());
	}
	if (_type->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		auto const* arc4Arr = dynamic_cast<awst::ARC4StaticArray const*>(_type);
		if (arc4Arr)
			return arc4Arr->arraySize() * computeFlatElementCount(arc4Arr->elementType());
	}
	return 1;
}

int AssemblyTranslator::computeARC4ByteSize(awst::WType const* _type)
{
	if (!_type)
		return 32;
	if (auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(_type))
		return uintN->n() / 8;
	if (auto const* arr = dynamic_cast<awst::ARC4StaticArray const*>(_type))
		return arr->arraySize() * computeARC4ByteSize(arr->elementType());
	if (auto const* s = dynamic_cast<awst::ARC4Struct const*>(_type))
	{
		int total = 0;
		for (auto const& [name, fieldType]: s->fields())
			total += computeARC4ByteSize(fieldType);
		return total;
	}
	if (auto const* bytesType = dynamic_cast<awst::BytesWType const*>(_type))
	{
		if (bytesType->length())
			return *bytesType->length();
	}
	return 32; // default
}

void AssemblyTranslator::initializeCalldataMap(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params
)
{
	uint64_t offset = 4; // Skip 4-byte function selector
	for (auto const& [name, type]: _params)
	{
		int elementCount = computeFlatElementCount(type);

		// Store the EVM calldata base offset for this parameter
		m_localConstants[name] = offset;

		// Map each 32-byte element to its parameter and flat index
		for (int i = 0; i < elementCount; ++i)
		{
			CalldataElement elem;
			elem.paramName = name;
			elem.flatIndex = i;
			elem.paramType = type;
			m_calldataMap[offset + static_cast<uint64_t>(i) * 32] = elem;
		}

		offset += static_cast<uint64_t>(elementCount) * 32;
	}
}

std::shared_ptr<awst::Expression> AssemblyTranslator::accessFlatElement(
	std::shared_ptr<awst::Expression> _base,
	awst::WType const* _type,
	int _flatIndex,
	awst::SourceLocation const& _loc
)
{
	// Handle ReferenceArray
	if (_type && _type->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(_type);
		if (!refArr || !refArr->arraySize())
			return _base;

		int innerSize = computeFlatElementCount(refArr->elementType());
		int outerIndex = _flatIndex / innerSize;
		int innerFlatIndex = _flatIndex % innerSize;

		auto index = std::make_shared<awst::IntegerConstant>();
		index->sourceLocation = _loc;
		index->wtype = awst::WType::uint64Type();
		index->value = std::to_string(outerIndex);

		auto indexExpr = std::make_shared<awst::IndexExpression>();
		indexExpr->sourceLocation = _loc;
		indexExpr->base = _base;
		indexExpr->index = std::move(index);
		indexExpr->wtype = refArr->elementType();

		if (innerSize == 1)
			return indexExpr;

		return accessFlatElement(indexExpr, refArr->elementType(), innerFlatIndex, _loc);
	}

	// Handle ARC4StaticArray
	if (_type && _type->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		auto const* arc4Arr = dynamic_cast<awst::ARC4StaticArray const*>(_type);
		if (!arc4Arr)
			return _base;

		int innerSize = computeFlatElementCount(arc4Arr->elementType());
		int outerIndex = _flatIndex / innerSize;
		int innerFlatIndex = _flatIndex % innerSize;

		auto index = std::make_shared<awst::IntegerConstant>();
		index->sourceLocation = _loc;
		index->wtype = awst::WType::uint64Type();
		index->value = std::to_string(outerIndex);

		auto indexExpr = std::make_shared<awst::IndexExpression>();
		indexExpr->sourceLocation = _loc;
		indexExpr->base = _base;
		indexExpr->index = std::move(index);
		indexExpr->wtype = arc4Arr->elementType();

		if (innerSize == 1)
		{
			// For leaf ARC4 elements (like arc4.uint256), decode to native biguint
			auto decode = std::make_shared<awst::ARC4Decode>();
			decode->sourceLocation = _loc;
			decode->wtype = awst::WType::biguintType();
			decode->value = indexExpr;
			return decode;
		}

		return accessFlatElement(indexExpr, arc4Arr->elementType(), innerFlatIndex, _loc);
	}

	// Scalar — _flatIndex should be 0
	return _base;
}

std::optional<uint64_t> AssemblyTranslator::resolveConstantOffset(
	std::shared_ptr<awst::Expression> const& _expr
)
{
	if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(_expr.get()))
	{
		try
		{
			return std::stoull(intConst->value);
		}
		catch (...)
		{
			return std::nullopt;
		}
	}

	// Check if it's a variable reference with a known constant value
	if (auto const* varExpr = dynamic_cast<awst::VarExpression const*>(_expr.get()))
	{
		auto it = m_localConstants.find(varExpr->name);
		if (it != m_localConstants.end())
			return it->second;
	}

	// Handle BigUInt binary operations with constant operands
	if (auto const* binOp = dynamic_cast<awst::BigUIntBinaryOperation const*>(_expr.get()))
	{
		auto left = resolveConstantOffset(binOp->left);
		auto right = resolveConstantOffset(binOp->right);
		if (left && right)
		{
			if (binOp->op == awst::BigUIntBinaryOperator::Add)
				return *left + *right;
			if (binOp->op == awst::BigUIntBinaryOperator::Sub)
				return *left - *right;
			if (binOp->op == awst::BigUIntBinaryOperator::Mult)
				return *left * *right;
			if (binOp->op == awst::BigUIntBinaryOperator::Mod && *right > 0)
				return *left % *right;
			if (binOp->op == awst::BigUIntBinaryOperator::FloorDiv && *right > 0)
				return *left / *right;
		}
		// For Mod where the right operand is too large for uint64_t (e.g. 2^256),
		// if the left operand resolves to uint64_t, the mod is a no-op since left < right.
		if (binOp->op == awst::BigUIntBinaryOperator::Mod && left && !right)
		{
			auto const* rc = dynamic_cast<awst::IntegerConstant const*>(binOp->right.get());
			if (rc && rc->value.length() > 18)
				return *left;
		}
	}

	return std::nullopt;
}

std::optional<std::pair<std::string, uint64_t>> AssemblyTranslator::decomposeVarOffset(
	std::shared_ptr<awst::Expression> const& _expr
)
{
	// Direct VarExpression → (name, 0)
	if (auto const* var = dynamic_cast<awst::VarExpression const*>(_expr.get()))
		return std::make_pair(var->name, uint64_t(0));

	if (auto const* binOp = dynamic_cast<awst::BigUIntBinaryOperation const*>(_expr.get()))
	{
		// Unwrap Mod(expr, TWO_POW_256) from wrapping arithmetic
		if (binOp->op == awst::BigUIntBinaryOperator::Mod)
		{
			auto const* rc = dynamic_cast<awst::IntegerConstant const*>(binOp->right.get());
			if (rc && rc->value.length() > 18)
				return decomposeVarOffset(binOp->left);
		}
		// Add(VarExpr, IntConst) or Add(IntConst, VarExpr) → (name, offset)
		if (binOp->op == awst::BigUIntBinaryOperator::Add)
		{
			auto const* leftVar = dynamic_cast<awst::VarExpression const*>(binOp->left.get());
			if (leftVar)
			{
				auto rightConst = resolveConstantOffset(binOp->right);
				if (rightConst)
					return std::make_pair(leftVar->name, *rightConst);
			}
			auto const* rightVar = dynamic_cast<awst::VarExpression const*>(binOp->right.get());
			if (rightVar)
			{
				auto leftConst = resolveConstantOffset(binOp->left);
				if (leftConst)
					return std::make_pair(rightVar->name, *leftConst);
			}
		}
	}

	return std::nullopt;
}

// ─── Source location helper ─────────────────────────────────────────────────

awst::SourceLocation AssemblyTranslator::makeLoc(
	solidity::langutil::DebugData::ConstPtr const& _debugData
)
{
	awst::SourceLocation loc;
	loc.file = m_sourceFile;
	if (_debugData)
	{
		auto const& nativeLoc = _debugData->nativeLocation;
		loc.line = nativeLoc.start >= 0 ? nativeLoc.start : 0;
		loc.endLine = nativeLoc.end >= 0 ? nativeLoc.end : 0;
	}
	return loc;
}

// ─── AWST helper ────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyTranslator::ensureBiguint(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc
)
{
	if (!_expr)
		return _expr;

	// Already biguint — no conversion needed
	if (_expr->wtype == awst::WType::biguintType())
		return _expr;

	if (_expr->wtype == awst::WType::boolType())
	{
		// bool → biguint: (expr ? 1 : 0)
		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = _loc;
		one->wtype = awst::WType::biguintType();
		one->value = "1";

		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";

		auto cond = std::make_shared<awst::ConditionalExpression>();
		cond->sourceLocation = _loc;
		cond->wtype = awst::WType::biguintType();
		cond->condition = std::move(_expr);
		cond->trueExpr = std::move(one);
		cond->falseExpr = std::move(zero);
		return cond;
	}

	if (_expr->wtype == awst::WType::uint64Type())
	{
		// uint64 → biguint: itob(expr) then ReinterpretCast bytes→biguint
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

	if (_expr->wtype->kind() == awst::WTypeKind::Bytes)
	{
		// bytes → biguint: ReinterpretCast
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(_expr);
		return cast;
	}

	// Unknown type — return as-is (may produce downstream errors)
	return _expr;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::ensureBool(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc
)
{
	if (!_expr)
		return _expr;

	// Already bool — no conversion needed
	if (_expr->wtype == awst::WType::boolType())
		return _expr;

	// Yul uses non-zero = true. Convert biguint/uint64 to bool via != 0
	if (_expr->wtype == awst::WType::biguintType())
	{
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";

		auto cmp = std::make_shared<awst::NumericComparisonExpression>();
		cmp->sourceLocation = _loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(_expr);
		cmp->op = awst::NumericComparison::Ne;
		cmp->rhs = std::move(zero);
		return cmp;
	}

	if (_expr->wtype == awst::WType::uint64Type())
	{
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::uint64Type();
		zero->value = "0";

		auto cmp = std::make_shared<awst::NumericComparisonExpression>();
		cmp->sourceLocation = _loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(_expr);
		cmp->op = awst::NumericComparison::Ne;
		cmp->rhs = std::move(zero);
		return cmp;
	}

	return _expr;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::makeBigUIntBinOp(
	std::shared_ptr<awst::Expression> _left,
	awst::BigUIntBinaryOperator _op,
	std::shared_ptr<awst::Expression> _right,
	awst::SourceLocation const& _loc
)
{
	auto node = std::make_shared<awst::BigUIntBinaryOperation>();
	node->sourceLocation = _loc;
	node->wtype = awst::WType::biguintType();
	node->left = ensureBiguint(std::move(_left), _loc);
	node->op = _op;
	node->right = ensureBiguint(std::move(_right), _loc);
	return node;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::makeTwoPow256(
	awst::SourceLocation const& _loc
)
{
	auto c = std::make_shared<awst::IntegerConstant>();
	c->sourceLocation = _loc;
	c->wtype = awst::WType::biguintType();
	c->value = "115792089237316195423570985008687907853269984665640564039457584007913129639936";
	return c;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::wrapMod256(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc
)
{
	return makeBigUIntBinOp(std::move(_expr), awst::BigUIntBinaryOperator::Mod, makeTwoPow256(_loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::safeDivMod(
	std::shared_ptr<awst::Expression> _left,
	awst::BigUIntBinaryOperator _op,
	std::shared_ptr<awst::Expression> _right,
	awst::SourceLocation const& _loc
)
{
	// EVM div/mod by zero returns 0; AVM panics.
	// Emit: right != 0 ? left op right : 0
	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = _loc;
	zero->wtype = awst::WType::biguintType();
	zero->value = "0";

	auto zeroForCmp = std::make_shared<awst::IntegerConstant>();
	zeroForCmp->sourceLocation = _loc;
	zeroForCmp->wtype = awst::WType::biguintType();
	zeroForCmp->value = "0";

	auto cond = std::make_shared<awst::NumericComparisonExpression>();
	cond->sourceLocation = _loc;
	cond->wtype = awst::WType::boolType();
	cond->lhs = ensureBiguint(_right, _loc);  // copies shared_ptr
	cond->op = awst::NumericComparison::Ne;
	cond->rhs = std::move(zeroForCmp);

	auto divExpr = makeBigUIntBinOp(_left, _op, _right, _loc);

	auto ternary = std::make_shared<awst::ConditionalExpression>();
	ternary->sourceLocation = _loc;
	ternary->wtype = awst::WType::biguintType();
	ternary->condition = std::move(cond);
	ternary->trueExpr = std::move(divExpr);
	ternary->falseExpr = std::move(zero);
	return ternary;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::safeBtoi(
	std::shared_ptr<awst::Expression> _biguintExpr,
	awst::SourceLocation const& _loc
)
{
	// Safe btoi pattern: concat(bzero(8), bytes(expr)) → extract last 8 bytes → btoi
	// This handles biguint values > 8 bytes from b&/b|/b^ padding.
	auto cast = std::make_shared<awst::ReinterpretCast>();
	cast->sourceLocation = _loc;
	cast->wtype = awst::WType::bytesType();
	cast->expr = std::move(_biguintExpr);

	auto eight = std::make_shared<awst::IntegerConstant>();
	eight->sourceLocation = _loc;
	eight->wtype = awst::WType::uint64Type();
	eight->value = "8";

	auto bzeroCall = std::make_shared<awst::IntrinsicCall>();
	bzeroCall->sourceLocation = _loc;
	bzeroCall->wtype = awst::WType::bytesType();
	bzeroCall->opCode = "bzero";
	bzeroCall->stackArgs.push_back(eight);

	auto cat = std::make_shared<awst::IntrinsicCall>();
	cat->sourceLocation = _loc;
	cat->wtype = awst::WType::bytesType();
	cat->opCode = "concat";
	cat->stackArgs.push_back(std::move(bzeroCall));
	cat->stackArgs.push_back(std::move(cast));

	auto lenCall = std::make_shared<awst::IntrinsicCall>();
	lenCall->sourceLocation = _loc;
	lenCall->wtype = awst::WType::uint64Type();
	lenCall->opCode = "len";
	lenCall->stackArgs.push_back(cat);

	auto eight2 = std::make_shared<awst::IntegerConstant>();
	eight2->sourceLocation = _loc;
	eight2->wtype = awst::WType::uint64Type();
	eight2->value = "8";

	auto start = std::make_shared<awst::IntrinsicCall>();
	start->sourceLocation = _loc;
	start->wtype = awst::WType::uint64Type();
	start->opCode = "-";
	start->stackArgs.push_back(std::move(lenCall));
	start->stackArgs.push_back(eight2);

	auto extractU64 = std::make_shared<awst::IntrinsicCall>();
	extractU64->sourceLocation = _loc;
	extractU64->wtype = awst::WType::uint64Type();
	extractU64->opCode = "extract_uint64";
	extractU64->stackArgs.push_back(cat);
	extractU64->stackArgs.push_back(std::move(start));

	return extractU64;
}

// ─── Expression translation ─────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyTranslator::translateExpression(
	solidity::yul::Expression const& _expr
)
{
	return std::visit(
		[this](auto const& _node) -> std::shared_ptr<awst::Expression> {
			using T = std::decay_t<decltype(_node)>;
			if constexpr (std::is_same_v<T, solidity::yul::FunctionCall>)
				return translateFunctionCall(_node);
			else if constexpr (std::is_same_v<T, solidity::yul::Identifier>)
				return translateIdentifier(_node);
			else if constexpr (std::is_same_v<T, solidity::yul::Literal>)
				return translateLiteral(_node);
			else
			{
				Logger::instance().error("unsupported Yul expression type in assembly");
				return nullptr;
			}
		},
		_expr
	);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::translateLiteral(
	solidity::yul::Literal const& _lit
)
{
	auto loc = makeLoc(_lit.debugData);

	if (_lit.kind == solidity::yul::LiteralKind::Number)
	{
		auto node = std::make_shared<awst::IntegerConstant>();
		node->sourceLocation = loc;
		node->wtype = awst::WType::biguintType();

		// Convert u256 to decimal string
		auto const& val = _lit.value.value();
		std::ostringstream oss;
		oss << val;
		node->value = oss.str();
		return node;
	}
	else if (_lit.kind == solidity::yul::LiteralKind::Boolean)
	{
		auto node = std::make_shared<awst::BoolConstant>();
		node->sourceLocation = loc;
		node->wtype = awst::WType::boolType();
		node->value = (_lit.value.value() != 0);
		return node;
	}

	Logger::instance().error("unsupported Yul literal kind", loc);
	return nullptr;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::translateIdentifier(
	solidity::yul::Identifier const& _id
)
{
	auto loc = makeLoc(_id.debugData);
	std::string name = _id.name.str();

	// Handle .offset / .length suffix on calldata parameter references
	// e.g., proofPayload.offset → calldata byte offset of proofPayload
	auto dotPos = name.rfind('.');
	if (dotPos != std::string::npos)
	{
		std::string suffix = name.substr(dotPos + 1);
		std::string baseName = name.substr(0, dotPos);

		if (suffix == "offset")
		{
			auto it = m_localConstants.find(baseName);
			if (it != m_localConstants.end())
			{
				auto node = std::make_shared<awst::IntegerConstant>();
				node->sourceLocation = loc;
				node->wtype = awst::WType::biguintType();
				node->value = std::to_string(it->second);
				return node;
			}
		}
		else if (suffix == "length")
		{
			// .length for calldata arrays/bytes — emit len(param)
			auto paramIt = m_locals.find(baseName);
			if (paramIt != m_locals.end())
			{
				auto paramVar = std::make_shared<awst::VarExpression>();
				paramVar->sourceLocation = loc;
				paramVar->name = baseName;
				paramVar->wtype = paramIt->second;

				auto lenCall = std::make_shared<awst::IntrinsicCall>();
				lenCall->sourceLocation = loc;
				lenCall->wtype = awst::WType::uint64Type();
				lenCall->opCode = "len";
				lenCall->stackArgs.push_back(std::move(paramVar));
				return lenCall;
			}
		}
	}

	// Check if this is an external constant (e.g., Solidity `uint constant M00 = ...`)
	auto constIt = m_constants.find(name);
	if (constIt != m_constants.end())
	{
		auto node = std::make_shared<awst::IntegerConstant>();
		node->sourceLocation = loc;
		node->wtype = awst::WType::biguintType();
		node->value = constIt->second;
		return node;
	}

	auto node = std::make_shared<awst::VarExpression>();
	node->sourceLocation = loc;
	node->name = name;

	auto it = m_locals.find(name);
	if (it != m_locals.end())
		node->wtype = it->second;
	else
		node->wtype = awst::WType::biguintType(); // Default: all assembly vars are uint256

	return node;
}

// ─── Function call translation ──────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyTranslator::translateFunctionCall(
	solidity::yul::FunctionCall const& _call
)
{
	auto loc = makeLoc(_call.debugData);
	std::string funcName = _call.functionName.name.str();

	// Before translating args, check for Yul-level patterns that need raw AST access.
	// mload(add(add(bytes_param, 32), offset)) → extract3(param, offset, 32)
	if (funcName == "mload" && _call.arguments.size() == 1)
	{
		auto result = tryHandleBytesMemoryRead(_call.arguments[0], loc);
		if (result)
			return result;
	}

	// Translate all arguments (stored in source order by the Yul parser)
	std::vector<std::shared_ptr<awst::Expression>> args;
	for (auto const& arg: _call.arguments)
		args.push_back(translateExpression(arg));

	// Builtin dispatch
	if (funcName == "mulmod")
		return handleMulmod(args, loc);
	if (funcName == "addmod")
		return handleAddmod(args, loc);
	if (funcName == "add")
		return handleAdd(args, loc);
	if (funcName == "mul")
		return handleMul(args, loc);
	if (funcName == "mod")
		return handleMod(args, loc);
	if (funcName == "sub")
		return handleSub(args, loc);
	if (funcName == "mload")
		return handleMload(args, loc);
	if (funcName == "iszero")
		return handleIszero(args, loc);
	if (funcName == "eq")
		return handleEq(args, loc);
	if (funcName == "lt")
		return handleLt(args, loc);
	if (funcName == "gt")
		return handleGt(args, loc);
	if (funcName == "and")
		return handleAnd(args, loc);
	if (funcName == "or")
		return handleOr(args, loc);
	if (funcName == "not")
		return handleNot(args, loc);
	if (funcName == "xor")
		return handleXor(args, loc);
	if (funcName == "div")
		return handleDiv(args, loc);
	if (funcName == "shl")
		return handleShl(args, loc);
	if (funcName == "shr")
		return handleShr(args, loc);
	if (funcName == "byte")
		return handleByte(args, loc);
	if (funcName == "signextend")
		return handleSignextend(args, loc);
	if (funcName == "sdiv")
		return handleSdiv(args, loc);
	if (funcName == "smod")
		return handleSmod(args, loc);
	if (funcName == "slt")
		return handleSlt(args, loc);
	if (funcName == "sgt")
		return handleSgt(args, loc);
	if (funcName == "sar")
		return handleSar(args, loc);
	if (funcName == "tload")
		return handleTload(args, loc);
	if (funcName == "sload")
		return handleSload(args, loc);
	if (funcName == "gas")
		return handleGas(loc);
	if (funcName == "extcodesize")
	{
		// extcodesize(addr) → on AVM, return 1 (treat all addresses as having code)
		Logger::instance().warning("extcodesize() stubbed as 1 (no AVM equivalent)", loc);
		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = loc;
		one->wtype = awst::WType::biguintType();
		one->value = "1";
		return one;
	}
	if (funcName == "address")
	{
		// address() → global CurrentApplicationAddress, cast to biguint
		auto addr = std::make_shared<awst::IntrinsicCall>();
		addr->sourceLocation = loc;
		addr->wtype = awst::WType::bytesType();
		addr->opCode = "global";
		addr->immediates.push_back("CurrentApplicationAddress");

		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(addr);
		return cast;
	}
	if (funcName == "timestamp")
		return handleTimestamp(loc);
	if (funcName == "chainid")
	{
		// chainid() has no AVM equivalent — return 0 (Algorand has no chain ID)
		// Use itob(0) to produce an 8-byte zeros value that stays as bytes
		// through the puya backend's optimizer (prevents folding to uint64).
		Logger::instance().debug("chainid() has no AVM equivalent, returning 0", loc);
		auto zero_u64 = std::make_shared<awst::IntegerConstant>();
		zero_u64->sourceLocation = loc;
		zero_u64->wtype = awst::WType::uint64Type();
		zero_u64->value = "0";

		auto itob_call = std::make_shared<awst::IntrinsicCall>();
		itob_call->sourceLocation = loc;
		itob_call->wtype = awst::WType::bytesType();
		itob_call->opCode = "itob";
		itob_call->stackArgs.push_back(std::move(zero_u64));

		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(itob_call);
		return cast;
	}
	if (funcName == "calldataload")
		return handleCalldataload(args, loc);
	if (funcName == "keccak256")
		return handleKeccak256(args, loc);
	if (funcName == "returndatasize")
		return handleReturndatasize(loc);
	if (funcName == "returndatacopy")
	{
		// returndatacopy(destOffset, offset, size) — no-op on AVM (no return data)
		return std::make_shared<awst::VoidConstant>();
	}
	if (funcName == "pop")
	{
		// pop(x) — discard value, no-op
		return std::make_shared<awst::VoidConstant>();
	}
	if (funcName == "tstore")
	{
		// tstore in expression context — should be a statement
		Logger::instance().warning("tstore() in expression context, treating as no-op", loc);
		return std::make_shared<awst::VoidConstant>();
	}
	if (funcName == "call" || funcName == "staticcall")
	{
		// call/staticcall in expression context (e.g., `let success := call(...)`)
		// is handled by the variable declaration / assignment translators.
		// In pure expression context, we can't do the full pattern match.
		Logger::instance().warning(
			funcName + " in pure expression context; use let/assign form for precompile support", loc
		);
		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = loc;
		one->wtype = awst::WType::biguintType();
		one->value = "1";
		return one;
	}

	// Check for user-defined assembly function
	auto asmIt = m_asmFunctions.find(funcName);
	if (asmIt != m_asmFunctions.end())
	{
		// For now, inline assembly function calls are not supported as pure
		// expressions (they need side-effect handling). This path should be
		// reached only via ExpressionStatement, which uses handleUserFunctionCall.
		Logger::instance().warning(
			"assembly function '" + funcName + "' called in expression context; "
			"this may not produce correct results",
			loc
		);
		return std::make_shared<awst::VoidConstant>();
	}

	// delegatecall → stub: return 1 (success)
	if (funcName == "delegatecall")
	{
		Logger::instance().warning("delegatecall() stubbed as success (1)", loc);
		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = loc;
		one->wtype = awst::WType::biguintType();
		one->value = "1";
		return one;
	}

	// create2(value, offset, size, salt) → stub: return 0 (no EVM contract deployment on AVM)
	if (funcName == "create2")
	{
		Logger::instance().warning(
			"create2() has no AVM equivalent (requires inner app creation txn), returning zero address",
			loc
		);
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";
		return zero;
	}

	Logger::instance().error(
		"unsupported Yul builtin function: " + funcName, loc
	);
	return nullptr;
}

// ─── Builtin handlers ───────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyTranslator::handleMulmod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// mulmod(a, b, c) = (a * b) % c
	if (_args.size() != 3)
	{
		Logger::instance().error("mulmod requires 3 arguments", _loc);
		return nullptr;
	}
	auto product = makeBigUIntBinOp(
		_args[0], awst::BigUIntBinaryOperator::Mult, _args[1], _loc
	);
	return makeBigUIntBinOp(
		std::move(product), awst::BigUIntBinaryOperator::Mod, _args[2], _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleAddmod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// addmod(a, b, c) = (a + b) % c
	if (_args.size() != 3)
	{
		Logger::instance().error("addmod requires 3 arguments", _loc);
		return nullptr;
	}
	auto sum = makeBigUIntBinOp(
		_args[0], awst::BigUIntBinaryOperator::Add, _args[1], _loc
	);
	return makeBigUIntBinOp(
		std::move(sum), awst::BigUIntBinaryOperator::Mod, _args[2], _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleAdd(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("add requires 2 arguments", _loc);
		return nullptr;
	}
	// EVM add wraps modulo 2^256
	auto sum = makeBigUIntBinOp(
		_args[0], awst::BigUIntBinaryOperator::Add, _args[1], _loc
	);
	return wrapMod256(std::move(sum), _loc);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleMul(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("mul requires 2 arguments", _loc);
		return nullptr;
	}
	// EVM mul wraps modulo 2^256
	auto product = makeBigUIntBinOp(
		_args[0], awst::BigUIntBinaryOperator::Mult, _args[1], _loc
	);
	return wrapMod256(std::move(product), _loc);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleMod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("mod requires 2 arguments", _loc);
		return nullptr;
	}
	// EVM: mod(a, 0) = 0. AVM: b% by 0 panics.
	// Emit: b != 0 ? a % b : 0
	return safeDivMod(
		_args[0], awst::BigUIntBinaryOperator::Mod, _args[1], _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleSub(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("sub requires 2 arguments", _loc);
		return nullptr;
	}
	// EVM sub wraps modulo 2^256: result = (a + 2^256 - b) mod 2^256
	// This avoids AVM biguint underflow when a < b
	auto aPlusPow = makeBigUIntBinOp(
		_args[0], awst::BigUIntBinaryOperator::Add, makeTwoPow256(_loc), _loc
	);
	auto diff = makeBigUIntBinOp(
		std::move(aPlusPow), awst::BigUIntBinaryOperator::Sub, _args[1], _loc
	);
	return wrapMod256(std::move(diff), _loc);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleIszero(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("iszero requires 1 argument", _loc);
		return nullptr;
	}
	// iszero(x): if x is already bool, emit Not; otherwise x == 0
	if (_args[0]->wtype == awst::WType::boolType())
	{
		auto notExpr = std::make_shared<awst::Not>();
		notExpr->sourceLocation = _loc;
		notExpr->wtype = awst::WType::boolType();
		notExpr->expr = _args[0];
		return notExpr;
	}

	auto arg = ensureBiguint(_args[0], _loc);

	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = _loc;
	zero->wtype = awst::WType::biguintType();
	zero->value = "0";

	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = std::move(arg);
	cmp->op = awst::NumericComparison::Eq;
	cmp->rhs = std::move(zero);
	return cmp;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleEq(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("eq requires 2 arguments", _loc);
		return nullptr;
	}
	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = ensureBiguint(_args[0], _loc);
	cmp->op = awst::NumericComparison::Eq;
	cmp->rhs = ensureBiguint(_args[1], _loc);
	return cmp;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleLt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("lt requires 2 arguments", _loc);
		return nullptr;
	}
	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = ensureBiguint(_args[0], _loc);
	cmp->op = awst::NumericComparison::Lt;
	cmp->rhs = ensureBiguint(_args[1], _loc);
	return cmp;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleGt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("gt requires 2 arguments", _loc);
		return nullptr;
	}
	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = ensureBiguint(_args[0], _loc);
	cmp->op = awst::NumericComparison::Gt;
	cmp->rhs = ensureBiguint(_args[1], _loc);
	return cmp;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleAnd(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("and requires 2 arguments", _loc);
		return nullptr;
	}
	// Bitwise AND on biguint: use b& opcode
	auto call = std::make_shared<awst::IntrinsicCall>();
	call->sourceLocation = _loc;
	call->wtype = awst::WType::bytesType();
	call->opCode = "b&";
	// Convert both operands to biguint first, then to bytes
	auto lhsCast = std::make_shared<awst::ReinterpretCast>();
	lhsCast->sourceLocation = _loc;
	lhsCast->wtype = awst::WType::bytesType();
	lhsCast->expr = ensureBiguint(_args[0], _loc);
	auto rhsCast = std::make_shared<awst::ReinterpretCast>();
	rhsCast->sourceLocation = _loc;
	rhsCast->wtype = awst::WType::bytesType();
	rhsCast->expr = ensureBiguint(_args[1], _loc);
	call->stackArgs.push_back(std::move(lhsCast));
	call->stackArgs.push_back(std::move(rhsCast));
	// Reinterpret result back to biguint
	auto result = std::make_shared<awst::ReinterpretCast>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::biguintType();
	result->expr = std::move(call);
	return result;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleOr(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("or requires 2 arguments", _loc);
		return nullptr;
	}
	auto call = std::make_shared<awst::IntrinsicCall>();
	call->sourceLocation = _loc;
	call->wtype = awst::WType::bytesType();
	call->opCode = "b|";
	auto lhsCast = std::make_shared<awst::ReinterpretCast>();
	lhsCast->sourceLocation = _loc;
	lhsCast->wtype = awst::WType::bytesType();
	lhsCast->expr = ensureBiguint(_args[0], _loc);
	auto rhsCast = std::make_shared<awst::ReinterpretCast>();
	rhsCast->sourceLocation = _loc;
	rhsCast->wtype = awst::WType::bytesType();
	rhsCast->expr = ensureBiguint(_args[1], _loc);
	call->stackArgs.push_back(std::move(lhsCast));
	call->stackArgs.push_back(std::move(rhsCast));
	auto result = std::make_shared<awst::ReinterpretCast>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::biguintType();
	result->expr = std::move(call);
	return result;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleNot(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("not requires 1 argument", _loc);
		return nullptr;
	}
	// EVM `not` operates on 256-bit values. AVM `b~` operates on actual byte length.
	// Pad input to 32 bytes so b~ produces a 256-bit result (e.g. not(0) = MAX_UINT256).
	auto padded = padTo32Bytes(ensureBiguint(_args[0], _loc), _loc);
	auto call = std::make_shared<awst::IntrinsicCall>();
	call->sourceLocation = _loc;
	call->wtype = awst::WType::bytesType();
	call->opCode = "b~";
	call->stackArgs.push_back(std::move(padded));
	auto result = std::make_shared<awst::ReinterpretCast>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::biguintType();
	result->expr = std::move(call);
	return result;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleXor(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("xor requires 2 arguments", _loc);
		return nullptr;
	}
	// Bitwise XOR on biguint: use b^ opcode
	// Coerce bool operands to biguint first (Yul: all values are uint256)
	auto lhs = ensureBiguint(_args[0], _loc);
	auto rhs = ensureBiguint(_args[1], _loc);

	auto call = std::make_shared<awst::IntrinsicCall>();
	call->sourceLocation = _loc;
	call->wtype = awst::WType::bytesType();
	call->opCode = "b^";
	// Convert both operands to bytes first
	auto lhsCast = std::make_shared<awst::ReinterpretCast>();
	lhsCast->sourceLocation = _loc;
	lhsCast->wtype = awst::WType::bytesType();
	lhsCast->expr = std::move(lhs);
	auto rhsCast = std::make_shared<awst::ReinterpretCast>();
	rhsCast->sourceLocation = _loc;
	rhsCast->wtype = awst::WType::bytesType();
	rhsCast->expr = std::move(rhs);
	call->stackArgs.push_back(std::move(lhsCast));
	call->stackArgs.push_back(std::move(rhsCast));
	// Reinterpret result back to biguint
	auto result = std::make_shared<awst::ReinterpretCast>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::biguintType();
	result->expr = std::move(call);
	return result;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleSload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("sload requires 1 argument", _loc);
		return nullptr;
	}
	// sload has no AVM equivalent — EVM raw storage slot access.
	// Return 0 with a warning.
	Logger::instance().warning("sload() has no AVM equivalent (EVM raw storage), returning 0", _loc);
	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = _loc;
	zero->wtype = awst::WType::biguintType();
	zero->value = "0";
	return zero;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleGas(
	awst::SourceLocation const& _loc
)
{
	// gas() has no AVM equivalent — return a large constant (used only as
	// the gas argument to staticcall, which is pattern-matched away)
	Logger::instance().debug("gas() has no AVM equivalent, returning max uint64", _loc);
	auto node = std::make_shared<awst::IntegerConstant>();
	node->sourceLocation = _loc;
	node->wtype = awst::WType::biguintType();
	node->value = "0";
	return node;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleTimestamp(
	awst::SourceLocation const& _loc
)
{
	// timestamp() → global LatestTimestamp (uint64) → itob → reinterpret as biguint
	auto tsCall = std::make_shared<awst::IntrinsicCall>();
	tsCall->sourceLocation = _loc;
	tsCall->wtype = awst::WType::uint64Type();
	tsCall->opCode = "global";
	tsCall->immediates.push_back("LatestTimestamp");

	auto itobCall = std::make_shared<awst::IntrinsicCall>();
	itobCall->sourceLocation = _loc;
	itobCall->wtype = awst::WType::bytesType();
	itobCall->opCode = "itob";
	itobCall->stackArgs.push_back(std::move(tsCall));

	auto biguintCast = std::make_shared<awst::ReinterpretCast>();
	biguintCast->sourceLocation = _loc;
	biguintCast->wtype = awst::WType::biguintType();
	biguintCast->expr = std::move(itobCall);
	return biguintCast;
}

// ─── New Yul builtins for Uniswap V4 ────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyTranslator::buildPowerOf2(
	std::shared_ptr<awst::Expression> _shift,
	awst::SourceLocation const& _loc
)
{
	// Construct 2^shift using setbit(bzero(32), 255-shift, 1)
	// since AVM has no bexp opcode
	auto shiftAmt = _shift;

	// Convert to uint64 if needed (shift amount must be uint64 for subtraction)
	if (shiftAmt->wtype != awst::WType::uint64Type())
	{
		// Cast biguint → bytes first
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = awst::WType::bytesType();
		cast->expr = std::move(shiftAmt);

		// Safe btoi: extract last 8 bytes to avoid btoi overflow (> 8 bytes fails)
		// Pattern: concat(bzero(8), bytes) → extract3(result, len-8, 8) → btoi
		auto eight = std::make_shared<awst::IntegerConstant>();
		eight->sourceLocation = _loc;
		eight->wtype = awst::WType::uint64Type();
		eight->value = "8";

		auto bzeroCall = std::make_shared<awst::IntrinsicCall>();
		bzeroCall->sourceLocation = _loc;
		bzeroCall->wtype = awst::WType::bytesType();
		bzeroCall->opCode = "bzero";
		bzeroCall->stackArgs.push_back(eight);

		auto cat = std::make_shared<awst::IntrinsicCall>();
		cat->sourceLocation = _loc;
		cat->wtype = awst::WType::bytesType();
		cat->opCode = "concat";
		cat->stackArgs.push_back(std::move(bzeroCall));
		cat->stackArgs.push_back(std::move(cast));

		auto lenCall = std::make_shared<awst::IntrinsicCall>();
		lenCall->sourceLocation = _loc;
		lenCall->wtype = awst::WType::uint64Type();
		lenCall->opCode = "len";
		lenCall->stackArgs.push_back(cat);

		auto eight2 = std::make_shared<awst::IntegerConstant>();
		eight2->sourceLocation = _loc;
		eight2->wtype = awst::WType::uint64Type();
		eight2->value = "8";

		auto start = std::make_shared<awst::IntrinsicCall>();
		start->sourceLocation = _loc;
		start->wtype = awst::WType::uint64Type();
		start->opCode = "-";
		start->stackArgs.push_back(std::move(lenCall));
		start->stackArgs.push_back(eight2);

		auto eight3 = std::make_shared<awst::IntegerConstant>();
		eight3->sourceLocation = _loc;
		eight3->wtype = awst::WType::uint64Type();
		eight3->value = "8";

		auto extract = std::make_shared<awst::IntrinsicCall>();
		extract->sourceLocation = _loc;
		extract->wtype = awst::WType::bytesType();
		extract->opCode = "extract3";
		extract->stackArgs.push_back(cat);
		extract->stackArgs.push_back(std::move(start));
		extract->stackArgs.push_back(eight3);

		auto btoi = std::make_shared<awst::IntrinsicCall>();
		btoi->sourceLocation = _loc;
		btoi->wtype = awst::WType::uint64Type();
		btoi->opCode = "btoi";
		btoi->stackArgs.push_back(std::move(extract));
		shiftAmt = std::move(btoi);
	}

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

	// Clamp shift amount to 0..255 — EVM shifts mod 256 implicitly,
	// but puya optimizer may strip wrapMod256 from intermediates
	auto twoFiftySix = std::make_shared<awst::IntegerConstant>();
	twoFiftySix->sourceLocation = _loc;
	twoFiftySix->wtype = awst::WType::uint64Type();
	twoFiftySix->value = "256";

	auto clampedShift = std::make_shared<awst::UInt64BinaryOperation>();
	clampedShift->sourceLocation = _loc;
	clampedShift->wtype = awst::WType::uint64Type();
	clampedShift->left = std::move(shiftAmt);
	clampedShift->right = std::move(twoFiftySix);
	clampedShift->op = awst::UInt64BinaryOperator::Mod;

	// 255 - shift: setbit uses MSB-first ordering, so bit (255-n) = 2^n
	auto twoFiftyFive = std::make_shared<awst::IntegerConstant>();
	twoFiftyFive->sourceLocation = _loc;
	twoFiftyFive->wtype = awst::WType::uint64Type();
	twoFiftyFive->value = "255";

	auto bitIdx = std::make_shared<awst::UInt64BinaryOperation>();
	bitIdx->sourceLocation = _loc;
	bitIdx->wtype = awst::WType::uint64Type();
	bitIdx->left = std::move(twoFiftyFive);
	bitIdx->right = std::move(clampedShift);
	bitIdx->op = awst::UInt64BinaryOperator::Sub;

	// setbit(bzero(32), 255-shift, 1) → bytes with only bit `shift` set
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

	return castToBigUInt;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleDiv(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("div requires 2 arguments", _loc);
		return nullptr;
	}
	// EVM: div(a, 0) = 0. AVM: b/ by 0 panics.
	// Emit: b != 0 ? a / b : 0
	return safeDivMod(
		_args[0], awst::BigUIntBinaryOperator::FloorDiv, _args[1], _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleShl(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// shl(shift, value) → value * 2^shift
	// NOTE: Yul shl argument order is (shift, value), NOT (value, shift)
	if (_args.size() != 2)
	{
		Logger::instance().error("shl requires 2 arguments", _loc);
		return nullptr;
	}
	auto power = buildPowerOf2(_args[0], _loc);
	auto product = makeBigUIntBinOp(
		_args[1], awst::BigUIntBinaryOperator::Mult, std::move(power), _loc
	);
	return wrapMod256(std::move(product), _loc);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleShr(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// shr(shift, value) → value / 2^shift
	// NOTE: Yul shr argument order is (shift, value), NOT (value, shift)
	if (_args.size() != 2)
	{
		Logger::instance().error("shr requires 2 arguments", _loc);
		return nullptr;
	}
	auto power = buildPowerOf2(_args[0], _loc);
	return makeBigUIntBinOp(
		_args[1], awst::BigUIntBinaryOperator::FloorDiv, std::move(power), _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleByte(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// byte(n, x) → extract byte n from 32-byte big-endian padded x
	// Implementation: pad x to 32 bytes, then extract3(padded, n, 1)
	if (_args.size() != 2)
	{
		Logger::instance().error("byte requires 2 arguments", _loc);
		return nullptr;
	}

	// Pad x to 32 bytes big-endian
	auto padded = padTo32Bytes(_args[1], _loc);

	// Convert n to uint64 for extract3
	auto nExpr = _args[0];
	if (nExpr->wtype != awst::WType::uint64Type())
	{
		nExpr = safeBtoi(std::move(nExpr), _loc);
	}

	// extract3(padded, n, 1)
	auto one = std::make_shared<awst::IntegerConstant>();
	one->sourceLocation = _loc;
	one->wtype = awst::WType::uint64Type();
	one->value = "1";

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(std::move(padded));
	extract->stackArgs.push_back(std::move(nExpr));
	extract->stackArgs.push_back(std::move(one));

	// Cast bytes → biguint for Yul semantics (all values are uint256)
	auto castResult = std::make_shared<awst::ReinterpretCast>();
	castResult->sourceLocation = _loc;
	castResult->wtype = awst::WType::biguintType();
	castResult->expr = std::move(extract);
	return castResult;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleSignextend(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// signextend(b, x) — sign-extend x from byte b (0-indexed from low byte).
	// If bit 7 of byte b is set, fill higher bytes with 0xFF, else 0x00.
	//
	// Implementation for two's complement in 256-bit biguint:
	//   bitPos = (b + 1) * 8  (total bits that are significant)
	//   signBit = (x >> (bitPos - 1)) & 1
	//   if signBit:
	//     mask = (2^256 - 1) - (2^bitPos - 1)  // all ones above bitPos
	//     result = x | mask
	//   else:
	//     mask = 2^bitPos - 1   // keep only lower bitPos bits
	//     result = x & mask
	//
	// For simplicity and since V4 uses signextend only in specific patterns,
	// we implement the full logic using conditional expression.
	if (_args.size() != 2)
	{
		Logger::instance().error("signextend requires 2 arguments", _loc);
		return nullptr;
	}

	// Ensure x is biguint
	auto x = ensureBiguint(_args[1], _loc);

	// For constant b values, we can optimize
	auto bConst = resolveConstantOffset(_args[0]);
	if (!bConst.has_value())
	{
		Logger::instance().warning("signextend with non-constant b, returning x unchanged", _loc);
		return x;
	}

	uint64_t b = *bConst;
	if (b >= 31)
	{
		// signextend from byte 31 or higher = no-op for 256-bit values
		return x;
	}

	uint64_t bitPos = (b + 1) * 8;

	// Build: signBit = (x >> (bitPos - 1)) & 1
	// Using shr pattern: x / 2^(bitPos-1) mod 2
	auto shiftAmt = std::make_shared<awst::IntegerConstant>();
	shiftAmt->sourceLocation = _loc;
	shiftAmt->wtype = awst::WType::biguintType();
	shiftAmt->value = "1"; // 2^(bitPos-1) via pow2

	auto shiftConst = std::make_shared<awst::IntegerConstant>();
	shiftConst->sourceLocation = _loc;
	shiftConst->wtype = awst::WType::uint64Type();
	shiftConst->value = std::to_string(bitPos - 1);

	auto pow2shift = buildPowerOf2(std::move(shiftConst), _loc);

	// x / 2^(bitPos-1)
	auto shifted = makeBigUIntBinOp(x, awst::BigUIntBinaryOperator::FloorDiv, pow2shift, _loc);

	// ... mod 2
	auto two = std::make_shared<awst::IntegerConstant>();
	two->sourceLocation = _loc;
	two->wtype = awst::WType::biguintType();
	two->value = "2";

	auto signBit = makeBigUIntBinOp(shifted, awst::BigUIntBinaryOperator::Mod, two, _loc);

	// signBit != 0  (i.e., sign bit is set)
	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = _loc;
	zero->wtype = awst::WType::biguintType();
	zero->value = "0";

	auto isNeg = std::make_shared<awst::NumericComparisonExpression>();
	isNeg->sourceLocation = _loc;
	isNeg->wtype = awst::WType::boolType();
	isNeg->lhs = signBit;
	isNeg->op = awst::NumericComparison::Ne;
	isNeg->rhs = zero;

	// lowMask = 2^bitPos - 1
	auto bitPosConst = std::make_shared<awst::IntegerConstant>();
	bitPosConst->sourceLocation = _loc;
	bitPosConst->wtype = awst::WType::uint64Type();
	bitPosConst->value = std::to_string(bitPos);

	auto pow2BitPos = buildPowerOf2(std::move(bitPosConst), _loc);

	auto oneBI = std::make_shared<awst::IntegerConstant>();
	oneBI->sourceLocation = _loc;
	oneBI->wtype = awst::WType::biguintType();
	oneBI->value = "1";

	auto lowMask = makeBigUIntBinOp(pow2BitPos, awst::BigUIntBinaryOperator::Sub, oneBI, _loc);

	// highMask = ~lowMask in 256 bits = (2^256 - 1) - lowMask
	// Use MAX_UINT256 = 2^256 - 1
	auto maxU256 = std::make_shared<awst::IntegerConstant>();
	maxU256->sourceLocation = _loc;
	maxU256->wtype = awst::WType::biguintType();
	maxU256->value = "115792089237316195423570985008687907853269984665640564039457584007913129639935";

	auto highMask = makeBigUIntBinOp(maxU256, awst::BigUIntBinaryOperator::Sub, lowMask, _loc);

	// Negative case: x | highMask (set all bits above bitPos)
	auto xCastNeg = std::make_shared<awst::ReinterpretCast>();
	xCastNeg->sourceLocation = _loc;
	xCastNeg->wtype = awst::WType::bytesType();
	xCastNeg->expr = x;

	auto highMaskBytes = std::make_shared<awst::ReinterpretCast>();
	highMaskBytes->sourceLocation = _loc;
	highMaskBytes->wtype = awst::WType::bytesType();
	highMaskBytes->expr = highMask;

	auto orCall = std::make_shared<awst::IntrinsicCall>();
	orCall->sourceLocation = _loc;
	orCall->wtype = awst::WType::bytesType();
	orCall->opCode = "b|";
	orCall->stackArgs.push_back(std::move(xCastNeg));
	orCall->stackArgs.push_back(std::move(highMaskBytes));

	auto negResult = std::make_shared<awst::ReinterpretCast>();
	negResult->sourceLocation = _loc;
	negResult->wtype = awst::WType::biguintType();
	negResult->expr = std::move(orCall);

	// Positive case: x & lowMask (clear all bits above bitPos)
	// Re-create lowMask (can't reuse shared_ptr after move)
	auto bitPosConst2 = std::make_shared<awst::IntegerConstant>();
	bitPosConst2->sourceLocation = _loc;
	bitPosConst2->wtype = awst::WType::uint64Type();
	bitPosConst2->value = std::to_string(bitPos);

	auto pow2BitPos2 = buildPowerOf2(std::move(bitPosConst2), _loc);

	auto oneBI2 = std::make_shared<awst::IntegerConstant>();
	oneBI2->sourceLocation = _loc;
	oneBI2->wtype = awst::WType::biguintType();
	oneBI2->value = "1";

	auto lowMask2 = makeBigUIntBinOp(pow2BitPos2, awst::BigUIntBinaryOperator::Sub, oneBI2, _loc);

	auto xCastPos = std::make_shared<awst::ReinterpretCast>();
	xCastPos->sourceLocation = _loc;
	xCastPos->wtype = awst::WType::bytesType();
	xCastPos->expr = x;

	auto lowMask2Bytes = std::make_shared<awst::ReinterpretCast>();
	lowMask2Bytes->sourceLocation = _loc;
	lowMask2Bytes->wtype = awst::WType::bytesType();
	lowMask2Bytes->expr = lowMask2;

	auto andCall = std::make_shared<awst::IntrinsicCall>();
	andCall->sourceLocation = _loc;
	andCall->wtype = awst::WType::bytesType();
	andCall->opCode = "b&";
	andCall->stackArgs.push_back(std::move(xCastPos));
	andCall->stackArgs.push_back(std::move(lowMask2Bytes));

	auto posResult = std::make_shared<awst::ReinterpretCast>();
	posResult->sourceLocation = _loc;
	posResult->wtype = awst::WType::biguintType();
	posResult->expr = std::move(andCall);

	// Conditional: isNeg ? negResult : posResult
	auto cond = std::make_shared<awst::ConditionalExpression>();
	cond->sourceLocation = _loc;
	cond->wtype = awst::WType::biguintType();
	cond->condition = std::move(isNeg);
	cond->trueExpr = std::move(negResult);
	cond->falseExpr = std::move(posResult);
	return cond;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleTload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// tload(slot) — load from transient storage
	// Map to global state read with slot-derived key
	if (_args.size() != 1)
	{
		Logger::instance().error("tload requires 1 argument", _loc);
		return nullptr;
	}

	// Try to resolve constant slot for named transient vars
	auto slotConst = resolveConstantOffset(_args[0]);

	// Create the box key: sha256("__t_" + padTo32Bytes(slot))
	// For constant slots, we can use a readable name
	std::shared_ptr<awst::Expression> keyExpr;
	if (slotConst.has_value())
	{
		// Use a readable key like "__t_<slot_number>"
		auto keyBytes = std::make_shared<awst::BytesConstant>();
		keyBytes->sourceLocation = _loc;
		keyBytes->wtype = awst::WType::bytesType();
		std::string keyStr = "__t_" + std::to_string(*slotConst);
		keyBytes->value.assign(keyStr.begin(), keyStr.end());
		keyExpr = std::move(keyBytes);
	}
	else
	{
		// Dynamic slot: key = sha256("__t_" || padTo32Bytes(slot))
		auto prefix = std::make_shared<awst::BytesConstant>();
		prefix->sourceLocation = _loc;
		prefix->wtype = awst::WType::bytesType();
		std::string pfx = "__t_";
		prefix->value.assign(pfx.begin(), pfx.end());

		auto slotPadded = padTo32Bytes(_args[0], _loc);

		auto concat = std::make_shared<awst::IntrinsicCall>();
		concat->sourceLocation = _loc;
		concat->wtype = awst::WType::bytesType();
		concat->opCode = "concat";
		concat->stackArgs.push_back(std::move(prefix));
		concat->stackArgs.push_back(std::move(slotPadded));

		auto sha = std::make_shared<awst::IntrinsicCall>();
		sha->sourceLocation = _loc;
		sha->wtype = awst::WType::bytesType();
		sha->opCode = "sha256";
		sha->stackArgs.push_back(std::move(concat));

		keyExpr = std::move(sha);
	}

	// Emit: app_global_get_ex(0, key) → get value or default 0
	// Since we want to return 0 for unset slots (matching EVM tload behavior),
	// use app_global_get which returns 0 for missing keys.
	auto appId = std::make_shared<awst::IntegerConstant>();
	appId->sourceLocation = _loc;
	appId->wtype = awst::WType::uint64Type();
	appId->value = "0";

	auto globalGet = std::make_shared<awst::IntrinsicCall>();
	globalGet->sourceLocation = _loc;
	globalGet->wtype = awst::WType::uint64Type();
	globalGet->opCode = "app_global_get";
	globalGet->stackArgs.push_back(std::move(keyExpr));

	// app_global_get returns uint64 at runtime for integer values.
	// Convert uint64 → bytes via itob so downstream b+/b- see bytes operands.
	auto itob = std::make_shared<awst::IntrinsicCall>();
	itob->sourceLocation = _loc;
	itob->wtype = awst::WType::bytesType();
	itob->opCode = "itob";
	itob->stackArgs.push_back(std::move(globalGet));

	// Cast bytes → biguint
	auto castResult = std::make_shared<awst::ReinterpretCast>();
	castResult->sourceLocation = _loc;
	castResult->wtype = awst::WType::biguintType();
	castResult->expr = std::move(itob);
	return castResult;
}

void AssemblyTranslator::handleTstore(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// tstore(slot, value) — store to transient storage
	// Map to global state write with slot-derived key
	if (_args.size() != 2)
	{
		Logger::instance().error("tstore requires 2 arguments", _loc);
		return;
	}

	auto slotConst = resolveConstantOffset(_args[0]);

	// Create the key (same as tload)
	std::shared_ptr<awst::Expression> keyExpr;
	if (slotConst.has_value())
	{
		auto keyBytes = std::make_shared<awst::BytesConstant>();
		keyBytes->sourceLocation = _loc;
		keyBytes->wtype = awst::WType::bytesType();
		std::string keyStr = "__t_" + std::to_string(*slotConst);
		keyBytes->value.assign(keyStr.begin(), keyStr.end());
		keyExpr = std::move(keyBytes);
	}
	else
	{
		auto prefix = std::make_shared<awst::BytesConstant>();
		prefix->sourceLocation = _loc;
		prefix->wtype = awst::WType::bytesType();
		std::string pfx = "__t_";
		prefix->value.assign(pfx.begin(), pfx.end());

		auto slotPadded = padTo32Bytes(_args[0], _loc);

		auto concat = std::make_shared<awst::IntrinsicCall>();
		concat->sourceLocation = _loc;
		concat->wtype = awst::WType::bytesType();
		concat->opCode = "concat";
		concat->stackArgs.push_back(std::move(prefix));
		concat->stackArgs.push_back(std::move(slotPadded));

		auto sha = std::make_shared<awst::IntrinsicCall>();
		sha->sourceLocation = _loc;
		sha->wtype = awst::WType::bytesType();
		sha->opCode = "sha256";
		sha->stackArgs.push_back(std::move(concat));

		keyExpr = std::move(sha);
	}

	// Cast value to bytes for storage
	auto valueCast = std::make_shared<awst::ReinterpretCast>();
	valueCast->sourceLocation = _loc;
	valueCast->wtype = awst::WType::bytesType();
	valueCast->expr = ensureBiguint(_args[1], _loc);

	// app_global_put(key, value)
	auto globalPut = std::make_shared<awst::IntrinsicCall>();
	globalPut->sourceLocation = _loc;
	globalPut->wtype = awst::WType::voidType();
	globalPut->opCode = "app_global_put";
	globalPut->stackArgs.push_back(std::move(keyExpr));
	globalPut->stackArgs.push_back(std::move(valueCast));

	auto exprStmt = std::make_shared<awst::ExpressionStatement>();
	exprStmt->sourceLocation = _loc;
	exprStmt->expr = std::move(globalPut);
	_out.push_back(std::move(exprStmt));
}

// ─── Signed integer helpers ──────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyTranslator::isNegative256(
	std::shared_ptr<awst::Expression> _val,
	awst::SourceLocation const& _loc,
	awst::WType const* _origType
)
{
	// Check sign bit in two's complement.
	// For biguint (256-bit): sign bit at position 255, threshold = 2^255
	// For uint64 (64-bit): sign bit at position 63, threshold = 2^63
	// This matters when uint64 variables hold two's complement values after
	// coercion back from biguint (e.g., signextend result coerced to uint64).
	auto halfMax = std::make_shared<awst::IntegerConstant>();
	halfMax->sourceLocation = _loc;
	halfMax->wtype = awst::WType::biguintType();
	if (_origType && _origType == awst::WType::uint64Type())
		halfMax->value = "9223372036854775808"; // 2^63
	else
		halfMax->value = "57896044618658097711785492504343953926634992332820282019728792003956564819968"; // 2^255

	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = _val;
	cmp->op = awst::NumericComparison::Gte;
	cmp->rhs = std::move(halfMax);
	return cmp;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::negate256(
	std::shared_ptr<awst::Expression> _val,
	awst::SourceLocation const& _loc
)
{
	// Two's complement negate: (~val + 1) mod 2^256
	// Equivalent: (2^256 - val) mod 2^256

	// For biguint, we do: MAX_UINT256 - val + 1
	auto maxU256 = std::make_shared<awst::IntegerConstant>();
	maxU256->sourceLocation = _loc;
	maxU256->wtype = awst::WType::biguintType();
	maxU256->value = "115792089237316195423570985008687907853269984665640564039457584007913129639935"; // 2^256 - 1

	auto sub = makeBigUIntBinOp(maxU256, awst::BigUIntBinaryOperator::Sub, _val, _loc);

	auto one = std::make_shared<awst::IntegerConstant>();
	one->sourceLocation = _loc;
	one->wtype = awst::WType::biguintType();
	one->value = "1";

	return makeBigUIntBinOp(sub, awst::BigUIntBinaryOperator::Add, one, _loc);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleSdiv(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// sdiv(a, b) — signed division in two's complement
	// Sign of result = sign of a XOR sign of b
	// |result| = |a| / |b|
	// If b == 0, result = 0 (EVM convention)
	if (_args.size() != 2)
	{
		Logger::instance().error("sdiv requires 2 arguments", _loc);
		return nullptr;
	}

	// Ensure args are biguint (may be uint64 or bytes from other ops)
	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	auto aNeg = isNegative256(a, _loc);
	auto bNeg = isNegative256(b, _loc);

	// |a| = aNeg ? negate(a) : a
	auto absA = std::make_shared<awst::ConditionalExpression>();
	absA->sourceLocation = _loc;
	absA->wtype = awst::WType::biguintType();
	absA->condition = aNeg;
	absA->trueExpr = negate256(a, _loc);
	absA->falseExpr = a;

	// |b| = bNeg ? negate(b) : b
	auto absB = std::make_shared<awst::ConditionalExpression>();
	absB->sourceLocation = _loc;
	absB->wtype = awst::WType::biguintType();
	absB->condition = bNeg;
	absB->trueExpr = negate256(b, _loc);
	absB->falseExpr = b;

	// |a| / |b|
	auto quotient = makeBigUIntBinOp(absA, awst::BigUIntBinaryOperator::FloorDiv, absB, _loc);

	// resultNeg = aNeg XOR bNeg
	auto aNeg2 = isNegative256(a, _loc);
	auto bNeg2 = isNegative256(b, _loc);
	auto aNegInt = ensureBiguint(aNeg2, _loc);
	auto bNegInt = ensureBiguint(bNeg2, _loc);
	auto xorResult = std::make_shared<awst::NumericComparisonExpression>();
	xorResult->sourceLocation = _loc;
	xorResult->wtype = awst::WType::boolType();
	xorResult->lhs = aNegInt;
	xorResult->op = awst::NumericComparison::Ne;
	xorResult->rhs = bNegInt;

	// result = resultNeg ? negate(quotient) : quotient
	auto result = std::make_shared<awst::ConditionalExpression>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::biguintType();
	result->condition = std::move(xorResult);
	result->trueExpr = negate256(quotient, _loc);
	result->falseExpr = quotient;
	return result;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleSmod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// smod(a, b) — signed modulo: sign of result = sign of a
	// |result| = |a| % |b|
	if (_args.size() != 2)
	{
		Logger::instance().error("smod requires 2 arguments", _loc);
		return nullptr;
	}

	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	auto aNeg = isNegative256(a, _loc);

	// |a|
	auto absA = std::make_shared<awst::ConditionalExpression>();
	absA->sourceLocation = _loc;
	absA->wtype = awst::WType::biguintType();
	absA->condition = aNeg;
	absA->trueExpr = negate256(a, _loc);
	absA->falseExpr = a;

	// |b|
	auto bNeg = isNegative256(b, _loc);
	auto absB = std::make_shared<awst::ConditionalExpression>();
	absB->sourceLocation = _loc;
	absB->wtype = awst::WType::biguintType();
	absB->condition = bNeg;
	absB->trueExpr = negate256(b, _loc);
	absB->falseExpr = b;

	// |a| % |b|
	auto remainder = makeBigUIntBinOp(absA, awst::BigUIntBinaryOperator::Mod, absB, _loc);

	// result = aNeg ? negate(remainder) : remainder
	auto aNeg2 = isNegative256(a, _loc);
	auto result = std::make_shared<awst::ConditionalExpression>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::biguintType();
	result->condition = std::move(aNeg2);
	result->trueExpr = negate256(remainder, _loc);
	result->falseExpr = remainder;
	return result;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleSlt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// slt(a, b) — signed less-than in two's complement
	if (_args.size() != 2)
	{
		Logger::instance().error("slt requires 2 arguments", _loc);
		return nullptr;
	}

	// Capture original types before ensureBiguint conversion, so we can use
	// the correct sign-bit threshold (bit 63 for uint64, bit 255 for biguint).
	auto const* origTypeA = _args[0]->wtype;
	auto const* origTypeB = _args[1]->wtype;
	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	// Special case: slt(x, 0) = isNegative(x)
	// The general case uses ConditionalExpression with (a < b) unsigned, which
	// puya's optimizer constant-folds to false when b=0 (no unsigned biguint < 0),
	// collapsing the entire slt expression. Emit the sign-bit check directly.
	if (auto* bConst = dynamic_cast<awst::IntegerConstant*>(b.get()))
	{
		if (bConst->value == "0")
		{
			return ensureBiguint(isNegative256(a, _loc, origTypeA), _loc);
		}
	}

	// Special case: slt(0, x) = x > 0 && x < signBitThreshold (positive non-zero)
	if (auto* aConst = dynamic_cast<awst::IntegerConstant*>(a.get()))
	{
		if (aConst->value == "0")
		{
			auto zero = std::make_shared<awst::IntegerConstant>();
			zero->sourceLocation = _loc;
			zero->wtype = awst::WType::biguintType();
			zero->value = "0";
			auto signThreshold = std::make_shared<awst::IntegerConstant>();
			signThreshold->sourceLocation = _loc;
			signThreshold->wtype = awst::WType::biguintType();
			if (origTypeB && origTypeB == awst::WType::uint64Type())
				signThreshold->value = "9223372036854775808"; // 2^63
			else
				signThreshold->value = "57896044618658097711785492504343953926634992332820282019728792003956564819968"; // 2^255
			// x > 0
			auto gtZero = std::make_shared<awst::NumericComparisonExpression>();
			gtZero->sourceLocation = _loc;
			gtZero->wtype = awst::WType::boolType();
			gtZero->lhs = b;
			gtZero->op = awst::NumericComparison::Gt;
			gtZero->rhs = std::move(zero);
			// x < signBitThreshold
			auto ltPow = std::make_shared<awst::NumericComparisonExpression>();
			ltPow->sourceLocation = _loc;
			ltPow->wtype = awst::WType::boolType();
			ltPow->lhs = b;
			ltPow->op = awst::NumericComparison::Lt;
			ltPow->rhs = std::move(signThreshold);
			// AND
			auto andExpr = std::make_shared<awst::BooleanBinaryOperation>();
			andExpr->sourceLocation = _loc;
			andExpr->wtype = awst::WType::boolType();
			andExpr->left = std::move(gtZero);
			andExpr->op = awst::BinaryBooleanOperator::And;
			andExpr->right = std::move(ltPow);
			return ensureBiguint(andExpr, _loc);
		}
	}

	// General case: compare signs, then unsigned comparison
	auto aNeg = isNegative256(a, _loc, origTypeA);
	auto bNeg = isNegative256(b, _loc, origTypeB);
	auto aNeg2 = isNegative256(a, _loc, origTypeA);

	// signsMatch = (aNeg == bNeg) via biguint comparison
	auto aNegInt = ensureBiguint(aNeg, _loc);
	auto bNegInt = ensureBiguint(bNeg, _loc);
	auto signsMatch = std::make_shared<awst::NumericComparisonExpression>();
	signsMatch->sourceLocation = _loc;
	signsMatch->wtype = awst::WType::boolType();
	signsMatch->lhs = aNegInt;
	signsMatch->op = awst::NumericComparison::Eq;
	signsMatch->rhs = bNegInt;

	// unsignedLt = a < b
	auto unsignedLt = std::make_shared<awst::NumericComparisonExpression>();
	unsignedLt->sourceLocation = _loc;
	unsignedLt->wtype = awst::WType::boolType();
	unsignedLt->lhs = a;
	unsignedLt->op = awst::NumericComparison::Lt;
	unsignedLt->rhs = b;

	// signsMatch ? (a < b) : aNeg
	auto result = std::make_shared<awst::ConditionalExpression>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::boolType();
	result->condition = signsMatch;
	result->trueExpr = unsignedLt;
	result->falseExpr = aNeg2;

	// Convert bool to biguint (Yul semantics: slt returns 0 or 1 as uint256)
	return ensureBiguint(result, _loc);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleSgt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// sgt(a, b) = slt(b, a) — just swap arguments
	if (_args.size() != 2)
	{
		Logger::instance().error("sgt requires 2 arguments", _loc);
		return nullptr;
	}
	std::vector<std::shared_ptr<awst::Expression>> swapped = {_args[1], _args[0]};
	return handleSlt(swapped, _loc);
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleSar(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// sar(shift, value) — arithmetic right shift (preserves sign)
	// If value is positive: same as shr(shift, value)
	// If value is negative: shr(shift, value) | ~(shr(shift, MAX_UINT256))
	//   i.e., fill shifted-in bits with 1s instead of 0s
	//
	// Simpler: for positive, shr works. For negative, negate → shr → negate.
	// Actually: sar(n, x) for negative x = ~(~x >> n) = negate(shr(n, negate(x) - 1)) - 1
	// That's complex. Let's use the conditional approach:
	//
	// If not negative: result = shr(shift, value) = value / 2^shift
	// If negative: result = (value / 2^shift) | ((2^256 - 1) - (2^(256-shift) - 1))
	//            = (value / 2^shift) | (mask with top `shift` bits set)
	//
	// Easiest correct implementation:
	// isNeg = value >= 2^255
	// shr_result = value / 2^shift
	// If isNeg: fill = (2^256 - 1) << (256 - shift) [all ones in top shift bits]
	//         = (2^256 - 1) - (2^(256-shift) - 1) = 2^256 - 2^(256-shift)
	//   result = shr_result | fill
	// Else: result = shr_result

	if (_args.size() != 2)
	{
		Logger::instance().error("sar requires 2 arguments", _loc);
		return nullptr;
	}

	// Ensure value is biguint for sign checking
	auto val = ensureBiguint(_args[1], _loc);
	std::vector<std::shared_ptr<awst::Expression>> coercedArgs = {_args[0], val};

	// shr_result = value / 2^shift
	auto shrResult = handleShr(coercedArgs, _loc);

	auto valNeg = isNegative256(val, _loc);

	// For negative case: fill top bits with 1s
	// fillMask = MAX_UINT256 * 2^(256 - shift) mod 2^256
	// Simpler: fillMask = MAX_UINT256 - (2^(256 - shift) - 1) = MAX_UINT256 - 2^(256-shift) + 1
	// Or: ~(2^(256-shift) - 1) in 256 bits

	// We need (256 - shift) as uint64
	auto shiftAmt = _args[0];
	std::shared_ptr<awst::Expression> shiftU64;
	if (shiftAmt->wtype != awst::WType::uint64Type())
		shiftU64 = safeBtoi(shiftAmt, _loc);
	else
		shiftU64 = shiftAmt;

	auto twoFiftySix = std::make_shared<awst::IntegerConstant>();
	twoFiftySix->sourceLocation = _loc;
	twoFiftySix->wtype = awst::WType::uint64Type();
	twoFiftySix->value = "256";

	auto complementShift = std::make_shared<awst::UInt64BinaryOperation>();
	complementShift->sourceLocation = _loc;
	complementShift->wtype = awst::WType::uint64Type();
	complementShift->left = std::move(twoFiftySix);
	complementShift->right = std::move(shiftU64);
	complementShift->op = awst::UInt64BinaryOperator::Sub;

	auto pow2Complement = buildPowerOf2(complementShift, _loc);

	// fillMask = MAX_UINT256 - pow2Complement + 1
	auto maxU256 = std::make_shared<awst::IntegerConstant>();
	maxU256->sourceLocation = _loc;
	maxU256->wtype = awst::WType::biguintType();
	maxU256->value = "115792089237316195423570985008687907853269984665640564039457584007913129639935";

	auto sub1 = makeBigUIntBinOp(maxU256, awst::BigUIntBinaryOperator::Sub, pow2Complement, _loc);

	auto one = std::make_shared<awst::IntegerConstant>();
	one->sourceLocation = _loc;
	one->wtype = awst::WType::biguintType();
	one->value = "1";

	auto fillMask = makeBigUIntBinOp(sub1, awst::BigUIntBinaryOperator::Add, one, _loc);

	// negResult = shr_result | fillMask (using b|)
	auto shrBytes = std::make_shared<awst::ReinterpretCast>();
	shrBytes->sourceLocation = _loc;
	shrBytes->wtype = awst::WType::bytesType();
	shrBytes->expr = shrResult;

	auto fillBytes = std::make_shared<awst::ReinterpretCast>();
	fillBytes->sourceLocation = _loc;
	fillBytes->wtype = awst::WType::bytesType();
	fillBytes->expr = fillMask;

	auto orCall = std::make_shared<awst::IntrinsicCall>();
	orCall->sourceLocation = _loc;
	orCall->wtype = awst::WType::bytesType();
	orCall->opCode = "b|";
	orCall->stackArgs.push_back(std::move(shrBytes));
	orCall->stackArgs.push_back(std::move(fillBytes));

	auto negResult = std::make_shared<awst::ReinterpretCast>();
	negResult->sourceLocation = _loc;
	negResult->wtype = awst::WType::biguintType();
	negResult->expr = std::move(orCall);

	// posResult = shr (re-compute to avoid sharing)
	auto posResult = handleShr(coercedArgs, _loc);

	auto result = std::make_shared<awst::ConditionalExpression>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::biguintType();
	result->condition = valNeg;
	result->trueExpr = negResult;
	result->falseExpr = posResult;
	return result;
}

void AssemblyTranslator::handleSstore(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// sstore(slot, value) — raw EVM persistent storage write.
	// No direct AVM equivalent. Stub as no-op with warning.
	Logger::instance().warning("sstore() has no AVM equivalent (EVM raw storage), ignoring", _loc);
	(void)_args;
	(void)_out;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleCalldataload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("calldataload requires 1 argument", _loc);
		return nullptr;
	}

	auto offset = resolveConstantOffset(_args[0]);
	if (!offset)
	{
		Logger::instance().error(
			"calldataload with non-constant offset not supported", _loc
		);
		return nullptr;
	}

	auto it = m_calldataMap.find(*offset);
	if (it != m_calldataMap.end())
	{
		auto const& elem = it->second;

		auto base = std::make_shared<awst::VarExpression>();
		base->sourceLocation = _loc;
		base->name = elem.paramName;
		base->wtype = m_locals.count(elem.paramName)
			? m_locals[elem.paramName]
			: awst::WType::biguintType();

		// For bytes/string parameters, calldataload reads 32 bytes at a relative offset.
		// Extract 32 bytes from the parameter and convert to biguint.
		if (elem.paramType
			&& (elem.paramType == awst::WType::bytesType()
				|| elem.paramType == awst::WType::stringType()))
		{
			uint64_t relativeOffset = *offset - m_localConstants[elem.paramName];

			auto offArg = std::make_shared<awst::IntegerConstant>();
			offArg->sourceLocation = _loc;
			offArg->wtype = awst::WType::uint64Type();
			offArg->value = std::to_string(relativeOffset);

			auto lenArg = std::make_shared<awst::IntegerConstant>();
			lenArg->sourceLocation = _loc;
			lenArg->wtype = awst::WType::uint64Type();
			lenArg->value = "32";

			auto extractCall = std::make_shared<awst::IntrinsicCall>();
			extractCall->sourceLocation = _loc;
			extractCall->wtype = awst::WType::bytesType();
			extractCall->opCode = "extract3";
			extractCall->stackArgs.push_back(std::move(base));
			extractCall->stackArgs.push_back(std::move(offArg));
			extractCall->stackArgs.push_back(std::move(lenArg));

			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(extractCall);
			return cast;
		}

		return accessFlatElement(std::move(base), elem.paramType, elem.flatIndex, _loc);
	}

	Logger::instance().error(
		"calldataload at unknown offset " + std::to_string(*offset), _loc
	);
	return nullptr;
}

std::optional<uint64_t> AssemblyTranslator::resolveConstantYulValue(
	solidity::yul::Expression const& _expr
)
{
	if (auto const* lit = std::get_if<solidity::yul::Literal>(&_expr))
	{
		if (lit->kind == solidity::yul::LiteralKind::Number)
		{
			auto const& val = lit->value.value();
			try
			{
				std::ostringstream oss;
				oss << val;
				return std::stoull(oss.str());
			}
			catch (...)
			{
				return std::nullopt;
			}
		}
	}

	// Check identifiers against local constants and external constants
	if (auto const* id = std::get_if<solidity::yul::Identifier>(&_expr))
	{
		std::string name = id->name.str();

		// Handle .offset / .length suffix on calldata parameter references
		// e.g., _pubSignals.offset → calldata byte offset of _pubSignals
		auto dotPos = name.rfind('.');
		if (dotPos != std::string::npos)
		{
			std::string suffix = name.substr(dotPos + 1);
			std::string baseName = name.substr(0, dotPos);
			if (suffix == "offset")
			{
				auto it = m_localConstants.find(baseName);
				if (it != m_localConstants.end())
					return it->second;
			}
			else if (suffix == "length")
			{
				// .length for calldata arrays: element count * 32
				// For bytes/string: not known at compile time
				// Return nullopt for now (handled dynamically if needed)
			}
		}

		auto it = m_localConstants.find(name);
		if (it != m_localConstants.end())
			return it->second;

		auto cit = m_constants.find(name);
		if (cit != m_constants.end())
		{
			try
			{
				return std::stoull(cit->second);
			}
			catch (...)
			{
				return std::nullopt;
			}
		}
	}

	// Handle function calls: add, sub, mul, mload
	if (auto const* call = std::get_if<solidity::yul::FunctionCall>(&_expr))
	{
		std::string name = call->functionName.name.str();
		if (call->arguments.size() == 2)
		{
			auto left = resolveConstantYulValue(call->arguments[0]);
			auto right = resolveConstantYulValue(call->arguments[1]);
			if (left && right)
			{
				if (name == "add")
					return *left + *right;
				if (name == "sub")
					return *left - *right;
				if (name == "mul")
					return *left * *right;
			}
		}

		// mload(offset) → look up the constant value stored at that memory offset
		if (name == "mload" && call->arguments.size() == 1)
		{
			auto offset = resolveConstantYulValue(call->arguments[0]);
			if (offset)
			{
				auto it = m_memoryMap.find(*offset);
				if (it != m_memoryMap.end() && !it->second.isParam)
				{
					auto cit = m_localConstants.find(it->second.varName);
					if (cit != m_localConstants.end())
						return cit->second;
				}
			}
		}
	}

	return std::nullopt;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleKeccak256(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("keccak256 requires 2 arguments (offset, length)", _loc);
		return nullptr;
	}

	auto length = resolveConstantOffset(_args[1]);

	// Check for struct (WTuple) parameter FIRST, before constant offset resolution.
	// initializeCalldataMap stores calldata offsets in m_localConstants, which makes
	// struct parameters resolve to false-positive "constant" memory offsets.
	// If the offset expression is a VarExpression with WTuple type, always use the
	// struct hash path regardless of whether the offset resolves to a constant.
	auto const* varExprForTuple = dynamic_cast<awst::VarExpression const*>(_args[0].get());
	if (varExprForTuple && length)
	{
		auto it = m_locals.find(varExprForTuple->name);
		if (it != m_locals.end() && it->second && it->second->kind() == awst::WTypeKind::WTuple)
		{
			auto const* tupleType = dynamic_cast<awst::WTuple const*>(it->second);
			if (tupleType)
			{
				int numFields = static_cast<int>(tupleType->types().size());
				int expectedLen = numFields * 32;
				if (static_cast<int>(*length) == expectedLen)
				{
					// Concatenate all struct fields, each padded to 32 bytes
					std::shared_ptr<awst::Expression> data;
					for (int i = 0; i < numFields; ++i)
					{
						auto field = std::make_shared<awst::TupleItemExpression>();
						field->sourceLocation = _loc;
						field->wtype = tupleType->types()[static_cast<size_t>(i)];
						field->base = _args[0];
						field->index = i;

						auto padded = padTo32Bytes(std::move(field), _loc);

						if (!data)
							data = std::move(padded);
						else
						{
							auto concat = std::make_shared<awst::IntrinsicCall>();
							concat->sourceLocation = _loc;
							concat->wtype = awst::WType::bytesType();
							concat->opCode = "concat";
							concat->stackArgs.push_back(std::move(data));
							concat->stackArgs.push_back(std::move(padded));
							data = std::move(concat);
						}
					}

					auto keccak = std::make_shared<awst::IntrinsicCall>();
					keccak->sourceLocation = _loc;
					keccak->wtype = awst::WType::bytesType();
					keccak->opCode = "keccak256";
					keccak->stackArgs.push_back(std::move(data));

					auto castResult = std::make_shared<awst::ReinterpretCast>();
					castResult->sourceLocation = _loc;
					castResult->wtype = awst::WType::biguintType();
					castResult->expr = std::move(keccak);
					return castResult;
				}
			}
		}
	}

	auto offset = resolveConstantOffset(_args[0]);

	if (!offset && length)
	{
		// Offset is a variable — check if it references a struct (WTuple) parameter.
		// Pattern: keccak256(structVar, numFields*32) hashes struct fields concatenated.
		auto const* varExpr = dynamic_cast<awst::VarExpression const*>(_args[0].get());
		if (varExpr)
		{
			auto it = m_locals.find(varExpr->name);
			if (it != m_locals.end() && it->second && it->second->kind() == awst::WTypeKind::WTuple)
			{
				auto const* tupleType = dynamic_cast<awst::WTuple const*>(it->second);
				if (tupleType)
				{
					int numFields = static_cast<int>(tupleType->types().size());
					int expectedLen = numFields * 32;
					if (static_cast<int>(*length) == expectedLen)
					{
						// Concatenate all struct fields, each padded to 32 bytes
						std::shared_ptr<awst::Expression> data;
						for (int i = 0; i < numFields; ++i)
						{
							auto field = std::make_shared<awst::TupleItemExpression>();
							field->sourceLocation = _loc;
							field->wtype = tupleType->types()[static_cast<size_t>(i)];
							field->base = _args[0];
							field->index = i;

							auto padded = padTo32Bytes(std::move(field), _loc);

							if (!data)
								data = std::move(padded);
							else
							{
								auto concat = std::make_shared<awst::IntrinsicCall>();
								concat->sourceLocation = _loc;
								concat->wtype = awst::WType::bytesType();
								concat->opCode = "concat";
								concat->stackArgs.push_back(std::move(data));
								concat->stackArgs.push_back(std::move(padded));
								data = std::move(concat);
							}
						}

						// keccak256 the concatenated bytes
						auto keccak = std::make_shared<awst::IntrinsicCall>();
						keccak->sourceLocation = _loc;
						keccak->wtype = awst::WType::bytesType();
						keccak->opCode = "keccak256";
						keccak->stackArgs.push_back(std::move(data));

						auto castResult = std::make_shared<awst::ReinterpretCast>();
						castResult->sourceLocation = _loc;
						castResult->wtype = awst::WType::biguintType();
						castResult->expr = std::move(keccak);
						return castResult;
					}
				}
			}
		}

		// Try variable-offset memory map: offset decomposes as VarExpr + constant
		auto decomposed = decomposeVarOffset(_args[0]);
		if (decomposed && length)
		{
			auto const& [baseName, baseOffset] = *decomposed;
			auto vit = m_varMemoryMap.find(baseName);
			if (vit != m_varMemoryMap.end())
			{
				int numSlots = static_cast<int>(*length / 0x20);
				if (numSlots > 0)
				{
					// Concatenate stored runtime variables from variable-offset map
					std::shared_ptr<awst::Expression> data;
					for (int i = 0; i < numSlots; ++i)
					{
						uint64_t slotOff = baseOffset + static_cast<uint64_t>(i) * 0x20;
						auto sit = vit->second.find(slotOff);
						std::shared_ptr<awst::Expression> slotVal;
						if (sit != vit->second.end())
						{
							auto var = std::make_shared<awst::VarExpression>();
							var->sourceLocation = _loc;
							var->name = sit->second.varName;
							var->wtype = awst::WType::biguintType();
							slotVal = padTo32Bytes(std::move(var), _loc);
						}
						else
						{
							// Slot not found — use zero
							auto zero = std::make_shared<awst::IntegerConstant>();
							zero->sourceLocation = _loc;
							zero->wtype = awst::WType::biguintType();
							zero->value = "0";
							slotVal = padTo32Bytes(std::move(zero), _loc);
						}
						if (!data)
							data = std::move(slotVal);
						else
						{
							auto concat = std::make_shared<awst::IntrinsicCall>();
							concat->sourceLocation = _loc;
							concat->wtype = awst::WType::bytesType();
							concat->opCode = "concat";
							concat->stackArgs.push_back(std::move(data));
							concat->stackArgs.push_back(std::move(slotVal));
							data = std::move(concat);
						}
					}

					auto keccak = std::make_shared<awst::IntrinsicCall>();
					keccak->sourceLocation = _loc;
					keccak->wtype = awst::WType::bytesType();
					keccak->opCode = "keccak256";
					keccak->stackArgs.push_back(std::move(data));

					auto castResult = std::make_shared<awst::ReinterpretCast>();
					castResult->sourceLocation = _loc;
					castResult->wtype = awst::WType::biguintType();
					castResult->expr = std::move(keccak);
					return castResult;
				}
			}
		}

		Logger::instance().error("keccak256 with non-constant offset/length not supported", _loc);
		return nullptr;
	}

	if (offset && !length)
	{
		// Offset is constant but length is dynamic.
		// Pattern: keccak256(begin, add(paramLength, 0x20)) from deriveMapping(string/bytes).
		// This hashes: param_bytes + last_mstored_32bytes (slot value).
		// Check if offset = calldataParam + 0x20 (pointing to string data area).
		for (auto const& [cdOffset, elem] : m_calldataMap)
		{
			if (*offset == cdOffset + 0x20 && m_lastMstoreValue)
			{
				// Build: keccak256(concat(param_bytes, padTo32(lastMstoreValue)))
				auto paramVar = std::make_shared<awst::VarExpression>();
				paramVar->sourceLocation = _loc;
				paramVar->name = elem.paramName;
				auto paramType = m_locals.find(elem.paramName);
				if (paramType != m_locals.end() && paramType->second)
					paramVar->wtype = paramType->second;
				else
					paramVar->wtype = awst::WType::bytesType();

				std::shared_ptr<awst::Expression> paramBytes;
				if (paramVar->wtype != awst::WType::bytesType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = _loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(paramVar);
					paramBytes = std::move(cast);
				}
				else
					paramBytes = std::move(paramVar);

				auto slotPadded = padTo32Bytes(m_lastMstoreValue, _loc);

				auto concat = std::make_shared<awst::IntrinsicCall>();
				concat->sourceLocation = _loc;
				concat->wtype = awst::WType::bytesType();
				concat->opCode = "concat";
				concat->stackArgs.push_back(std::move(paramBytes));
				concat->stackArgs.push_back(std::move(slotPadded));

				auto keccak = std::make_shared<awst::IntrinsicCall>();
				keccak->sourceLocation = _loc;
				keccak->wtype = awst::WType::bytesType();
				keccak->opCode = "keccak256";
				keccak->stackArgs.push_back(std::move(concat));

				auto castResult = std::make_shared<awst::ReinterpretCast>();
				castResult->sourceLocation = _loc;
				castResult->wtype = awst::WType::biguintType();
				castResult->expr = std::move(keccak);
				return castResult;
			}
		}
	}

	if (!offset || !length)
	{
		Logger::instance().error("keccak256 with non-constant offset/length not supported", _loc);
		return nullptr;
	}

	int numSlots = static_cast<int>(*length / 0x20);
	if (*length == 0)
	{
		// keccak256("") = 0xc5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470
		// Hash empty bytes
		auto emptyBytes = std::make_shared<awst::BytesConstant>();
		emptyBytes->sourceLocation = _loc;
		emptyBytes->wtype = awst::WType::bytesType();
		// empty value

		auto keccak = std::make_shared<awst::IntrinsicCall>();
		keccak->sourceLocation = _loc;
		keccak->wtype = awst::WType::bytesType();
		keccak->opCode = "keccak256";
		keccak->stackArgs.push_back(std::move(emptyBytes));

		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = _loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(keccak);
		return cast;
	}
	if (numSlots <= 0)
	{
		// Non-zero length but less than 32 bytes — partial slot
		// Handle by reading from memory slot at offset
		Logger::instance().warning("keccak256 with sub-32-byte input, using partial slot", _loc);
		auto slotIt = m_memoryMap.find(*offset);
		if (slotIt != m_memoryMap.end())
		{
			auto slotVar = std::make_shared<awst::VarExpression>();
			slotVar->sourceLocation = _loc;
			slotVar->name = slotIt->second.varName;
			slotVar->wtype = awst::WType::biguintType();

			// Cast biguint to bytes for keccak256
			auto castToBytes = std::make_shared<awst::ReinterpretCast>();
			castToBytes->sourceLocation = _loc;
			castToBytes->wtype = awst::WType::bytesType();
			castToBytes->expr = std::move(slotVar);

			auto keccak = std::make_shared<awst::IntrinsicCall>();
			keccak->sourceLocation = _loc;
			keccak->wtype = awst::WType::bytesType();
			keccak->opCode = "keccak256";
			keccak->stackArgs.push_back(std::move(castToBytes));

			auto castResult = std::make_shared<awst::ReinterpretCast>();
			castResult->sourceLocation = _loc;
			castResult->wtype = awst::WType::biguintType();
			castResult->expr = std::move(keccak);
			return castResult;
		}
		// Check if offset = calldataParam + 0x20 (string/bytes data region)
		// Pattern: keccak256(add(param, 0x20), mload(param)) hashes string data
		for (auto const& [cdOffset, elem] : m_calldataMap)
		{
			if (*offset == cdOffset + 0x20)
			{
				// Found: offset points to the string data area of a calldata parameter.
				// On AVM, the parameter IS the string bytes. Hash them directly.
				auto paramVar = std::make_shared<awst::VarExpression>();
				paramVar->sourceLocation = _loc;
				paramVar->name = elem.paramName;
				// The parameter might be bytes or biguint — need bytes for keccak
				auto paramType = m_locals.find(elem.paramName);
				if (paramType != m_locals.end() && paramType->second)
					paramVar->wtype = paramType->second;
				else
					paramVar->wtype = awst::WType::bytesType();

				std::shared_ptr<awst::Expression> hashInput;
				if (paramVar->wtype != awst::WType::bytesType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = _loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(paramVar);
					hashInput = std::move(cast);
				}
				else
					hashInput = std::move(paramVar);

				auto keccak = std::make_shared<awst::IntrinsicCall>();
				keccak->sourceLocation = _loc;
				keccak->wtype = awst::WType::bytesType();
				keccak->opCode = "keccak256";
				keccak->stackArgs.push_back(std::move(hashInput));

				auto castResult = std::make_shared<awst::ReinterpretCast>();
				castResult->sourceLocation = _loc;
				castResult->wtype = awst::WType::biguintType();
				castResult->expr = std::move(keccak);
				return castResult;
			}
		}
		Logger::instance().error("keccak256 with sub-32-byte length and unknown memory slot", _loc);
		return nullptr;
	}

	// Check if the offset range falls within m_calldataMap (function parameters).
	// When keccak256 hashes a struct parameter (e.g., PoolKey), the Yul optimizer
	// may eliminate the abi_encode buffer copy, making the keccak256 offset point
	// directly to the calldata offset. We detect this and extract struct fields
	// from the ARC4-encoded parameter, padding each to 32 bytes for EVM ABI compat.
	auto firstSlotIt = m_calldataMap.find(*offset);
	if (firstSlotIt != m_calldataMap.end())
	{
		auto const& elem = firstSlotIt->second;
		auto const* structType = dynamic_cast<awst::ARC4Struct const*>(elem.paramType);
		if (structType && numSlots == static_cast<int>(structType->fields().size()))
		{
			// ARC4Struct parameter: extract each field from raw bytes, pad to 32 bytes
			auto base = std::make_shared<awst::VarExpression>();
			base->sourceLocation = _loc;
			base->name = elem.paramName;
			base->wtype = m_locals.count(elem.paramName)
				? m_locals[elem.paramName]
				: elem.paramType;

			// Cast struct to raw bytes for field extraction
			auto structBytes = std::make_shared<awst::ReinterpretCast>();
			structBytes->sourceLocation = _loc;
			structBytes->wtype = awst::WType::bytesType();
			structBytes->expr = base;

			std::shared_ptr<awst::Expression> data;
			int fieldByteOffset = 0;
			for (auto const& [fieldName, fieldType]: structType->fields())
			{
				int fieldSize = computeARC4ByteSize(fieldType);

				// extract3(structBytes, fieldByteOffset, fieldSize)
				auto offExpr = std::make_shared<awst::IntegerConstant>();
				offExpr->sourceLocation = _loc;
				offExpr->wtype = awst::WType::uint64Type();
				offExpr->value = std::to_string(fieldByteOffset);

				auto lenExpr = std::make_shared<awst::IntegerConstant>();
				lenExpr->sourceLocation = _loc;
				lenExpr->wtype = awst::WType::uint64Type();
				lenExpr->value = std::to_string(fieldSize);

				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = _loc;
				extract->wtype = awst::WType::bytesType();
				extract->opCode = "extract3";
				extract->stackArgs.push_back(structBytes);
				extract->stackArgs.push_back(std::move(offExpr));
				extract->stackArgs.push_back(std::move(lenExpr));

				// Cast to biguint, then pad to 32 bytes
				auto asBiguint = std::make_shared<awst::ReinterpretCast>();
				asBiguint->sourceLocation = _loc;
				asBiguint->wtype = awst::WType::biguintType();
				asBiguint->expr = std::move(extract);

				auto padded = padTo32Bytes(std::move(asBiguint), _loc);

				if (!data)
					data = std::move(padded);
				else
				{
					auto concat = std::make_shared<awst::IntrinsicCall>();
					concat->sourceLocation = _loc;
					concat->wtype = awst::WType::bytesType();
					concat->opCode = "concat";
					concat->stackArgs.push_back(std::move(data));
					concat->stackArgs.push_back(std::move(padded));
					data = std::move(concat);
				}
				fieldByteOffset += fieldSize;
			}

			auto keccak = std::make_shared<awst::IntrinsicCall>();
			keccak->sourceLocation = _loc;
			keccak->wtype = awst::WType::bytesType();
			keccak->opCode = "keccak256";
			keccak->stackArgs.push_back(std::move(data));

			auto castResult = std::make_shared<awst::ReinterpretCast>();
			castResult->sourceLocation = _loc;
			castResult->wtype = awst::WType::biguintType();
			castResult->expr = std::move(keccak);
			return castResult;
		}
	}

	// Concatenate all memory slots using extracted helper
	auto data = concatSlots(*offset, 0, numSlots, _loc);

	// Apply keccak256
	auto keccak = std::make_shared<awst::IntrinsicCall>();
	keccak->sourceLocation = _loc;
	keccak->wtype = awst::WType::bytesType();
	keccak->opCode = "keccak256";
	keccak->stackArgs.push_back(std::move(data));

	// Convert bytes result to biguint (for Yul's uint256 type)
	auto castResult = std::make_shared<awst::ReinterpretCast>();
	castResult->sourceLocation = _loc;
	castResult->wtype = awst::WType::biguintType();
	castResult->expr = std::move(keccak);

	return castResult;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleReturndatasize(
	awst::SourceLocation const& _loc
)
{
	// On AVM there is no return data concept — return 0
	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = _loc;
	zero->wtype = awst::WType::biguintType();
	zero->value = "0";
	return zero;
}

void AssemblyTranslator::handleRevert(
	std::vector<std::shared_ptr<awst::Expression>> const& /* _args */,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// revert(offset, length) — on AVM, assert(false, "revert")
	auto assertExpr = std::make_shared<awst::AssertExpression>();
	assertExpr->sourceLocation = _loc;
	assertExpr->wtype = awst::WType::voidType();

	auto falseLit = std::make_shared<awst::BoolConstant>();
	falseLit->sourceLocation = _loc;
	falseLit->wtype = awst::WType::boolType();
	falseLit->value = false;

	assertExpr->condition = std::move(falseLit);
	assertExpr->errorMessage = "revert";

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = _loc;
	stmt->expr = std::move(assertExpr);
	_out.push_back(std::move(stmt));
}

// ─── Precompile helper methods ──────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyTranslator::readMemSlot(
	uint64_t _offset,
	awst::SourceLocation const& _loc
)
{
	auto it = m_memoryMap.find(_offset);
	if (it != m_memoryMap.end())
	{
		auto const& slot = it->second;
		if (slot.isParam)
		{
			auto base = std::make_shared<awst::VarExpression>();
			base->sourceLocation = _loc;
			base->name = slot.varName;
			base->wtype = m_arrayParamType;
			auto index = std::make_shared<awst::IntegerConstant>();
			index->sourceLocation = _loc;
			index->wtype = awst::WType::uint64Type();
			index->value = std::to_string(slot.paramIndex);
			auto indexExpr = std::make_shared<awst::IndexExpression>();
			indexExpr->sourceLocation = _loc;
			indexExpr->wtype = awst::WType::biguintType();
			indexExpr->base = std::move(base);
			indexExpr->index = std::move(index);
			return indexExpr;
		}
		else
		{
			auto var = std::make_shared<awst::VarExpression>();
			var->sourceLocation = _loc;
			var->name = slot.varName;
			var->wtype = awst::WType::biguintType();
			return var;
		}
	}
	Logger::instance().warning(
		"precompile input at offset 0x" +
		([&] { std::ostringstream oss; oss << std::hex << _offset; return oss.str(); })() +
		" not found, using zero", _loc
	);
	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = _loc;
	zero->wtype = awst::WType::biguintType();
	zero->value = "0";
	return zero;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::padTo32Bytes(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc
)
{
	auto cast = std::make_shared<awst::ReinterpretCast>();
	cast->sourceLocation = _loc;
	cast->wtype = awst::WType::bytesType();
	cast->expr = std::move(_expr);

	auto zeroBytes = std::make_shared<awst::IntrinsicCall>();
	zeroBytes->sourceLocation = _loc;
	zeroBytes->wtype = awst::WType::bytesType();
	zeroBytes->opCode = "bzero";
	auto sz = std::make_shared<awst::IntegerConstant>();
	sz->sourceLocation = _loc;
	sz->wtype = awst::WType::uint64Type();
	sz->value = "32";
	zeroBytes->stackArgs.push_back(sz);

	auto concatPad = std::make_shared<awst::IntrinsicCall>();
	concatPad->sourceLocation = _loc;
	concatPad->wtype = awst::WType::bytesType();
	concatPad->opCode = "concat";
	concatPad->stackArgs.push_back(std::move(zeroBytes));
	concatPad->stackArgs.push_back(std::move(cast));

	auto lenCall = std::make_shared<awst::IntrinsicCall>();
	lenCall->sourceLocation = _loc;
	lenCall->wtype = awst::WType::uint64Type();
	lenCall->opCode = "len";
	lenCall->stackArgs.push_back(concatPad);

	auto n32 = std::make_shared<awst::IntegerConstant>();
	n32->sourceLocation = _loc;
	n32->wtype = awst::WType::uint64Type();
	n32->value = "32";

	auto startOff = std::make_shared<awst::IntrinsicCall>();
	startOff->sourceLocation = _loc;
	startOff->wtype = awst::WType::uint64Type();
	startOff->opCode = "-";
	startOff->stackArgs.push_back(std::move(lenCall));
	startOff->stackArgs.push_back(n32);

	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(concatPad);
	extract->stackArgs.push_back(std::move(startOff));
	auto n32e = std::make_shared<awst::IntegerConstant>();
	n32e->sourceLocation = _loc;
	n32e->wtype = awst::WType::uint64Type();
	n32e->value = "32";
	extract->stackArgs.push_back(n32e);

	return extract;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::concatSlots(
	uint64_t _baseOffset, int _startSlot, int _count,
	awst::SourceLocation const& _loc
)
{
	std::shared_ptr<awst::Expression> result;
	for (int i = 0; i < _count; ++i)
	{
		uint64_t off = _baseOffset + static_cast<uint64_t>(_startSlot + i) * 0x20;
		auto slotBytes = padTo32Bytes(readMemSlot(off, _loc), _loc);
		if (!result)
			result = std::move(slotBytes);
		else
		{
			auto concat = std::make_shared<awst::IntrinsicCall>();
			concat->sourceLocation = _loc;
			concat->wtype = awst::WType::bytesType();
			concat->opCode = "concat";
			concat->stackArgs.push_back(std::move(result));
			concat->stackArgs.push_back(std::move(slotBytes));
			result = std::move(concat);
		}
	}
	return result;
}

std::string AssemblyTranslator::getOrCreateMemoryVar(
	uint64_t _offset,
	awst::SourceLocation const& _loc
)
{
	(void)_loc;
	auto it = m_memoryMap.find(_offset);
	if (it != m_memoryMap.end() && !it->second.isParam)
		return it->second.varName;

	std::string varName = "mem_0x" + ([&] {
		std::ostringstream oss;
		oss << std::hex << _offset;
		return oss.str();
	})();
	MemorySlot slot;
	slot.varName = varName;
	m_memoryMap[_offset] = slot;
	return varName;
}

void AssemblyTranslator::storeResultToMemory(
	std::shared_ptr<awst::Expression> _result,
	uint64_t _outputOffset, int _outputSlots,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	bool _isBoolResult
)
{
	if (_isBoolResult)
	{
		// Bool result → store as biguint (1 or 0) at outputOffset
		std::string varName = getOrCreateMemoryVar(_outputOffset, _loc);
		m_locals[varName] = awst::WType::biguintType();

		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = _loc;
		one->wtype = awst::WType::biguintType();
		one->value = "1";
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";

		auto cond = std::make_shared<awst::ConditionalExpression>();
		cond->sourceLocation = _loc;
		cond->wtype = awst::WType::biguintType();
		cond->condition = std::move(_result);
		cond->trueExpr = std::move(one);
		cond->falseExpr = std::move(zero);

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = varName;
		target->wtype = awst::WType::biguintType();

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = std::move(cond);
		_out.push_back(std::move(assign));
		return;
	}

	if (_outputSlots == 1)
	{
		// Single slot: result is already biguint or bytes — store directly
		std::string varName = getOrCreateMemoryVar(_outputOffset, _loc);
		m_locals[varName] = awst::WType::biguintType();

		// If result is bytes, cast to biguint
		std::shared_ptr<awst::Expression> storeVal = std::move(_result);
		if (storeVal->wtype == awst::WType::bytesType())
		{
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(storeVal);
			storeVal = std::move(cast);
		}

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = varName;
		target->wtype = awst::WType::biguintType();

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = std::move(storeVal);
		_out.push_back(std::move(assign));
		return;
	}

	// Multi-slot: store result bytes in a temporary, then extract 32-byte chunks
	std::string resultVar = "__precompile_result";
	m_locals[resultVar] = awst::WType::bytesType();

	auto resultTarget = std::make_shared<awst::VarExpression>();
	resultTarget->sourceLocation = _loc;
	resultTarget->name = resultVar;
	resultTarget->wtype = awst::WType::bytesType();

	auto assignResult = std::make_shared<awst::AssignmentStatement>();
	assignResult->sourceLocation = _loc;
	assignResult->target = resultTarget;
	assignResult->value = std::move(_result);
	_out.push_back(std::move(assignResult));

	for (int i = 0; i < _outputSlots; ++i)
	{
		uint64_t outOff = _outputOffset + static_cast<uint64_t>(i) * 0x20;
		std::string varName = getOrCreateMemoryVar(outOff, _loc);
		m_locals[varName] = awst::WType::biguintType();

		auto resultRead = std::make_shared<awst::VarExpression>();
		resultRead->sourceLocation = _loc;
		resultRead->name = resultVar;
		resultRead->wtype = awst::WType::bytesType();

		auto extractSlot = std::make_shared<awst::IntrinsicCall>();
		extractSlot->sourceLocation = _loc;
		extractSlot->wtype = awst::WType::bytesType();
		extractSlot->opCode = "extract3";
		extractSlot->stackArgs.push_back(resultRead);

		auto slotStart = std::make_shared<awst::IntegerConstant>();
		slotStart->sourceLocation = _loc;
		slotStart->wtype = awst::WType::uint64Type();
		slotStart->value = std::to_string(i * 32);
		extractSlot->stackArgs.push_back(slotStart);

		auto slotLen = std::make_shared<awst::IntegerConstant>();
		slotLen->sourceLocation = _loc;
		slotLen->wtype = awst::WType::uint64Type();
		slotLen->value = "32";
		extractSlot->stackArgs.push_back(slotLen);

		auto castSlot = std::make_shared<awst::ReinterpretCast>();
		castSlot->sourceLocation = _loc;
		castSlot->wtype = awst::WType::biguintType();
		castSlot->expr = std::move(extractSlot);

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = varName;
		target->wtype = awst::WType::biguintType();

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = std::move(castSlot);
		_out.push_back(std::move(assign));
	}
}

// ─── Unified precompile dispatch ────────────────────────────────────────────

void AssemblyTranslator::handlePrecompileCall(
	solidity::yul::FunctionCall const& _call,
	std::string const& _assignTarget,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	bool _isCall
)
{
	// call(gas, addr, value, inOff, inSize, outOff, outSize) — 7 args
	// staticcall(gas, addr, inOff, inSize, outOff, outSize) — 6 args
	size_t expectedArgs = _isCall ? 7 : 6;
	if (_call.arguments.size() != expectedArgs)
	{
		Logger::instance().error(
			(_isCall ? std::string("call") : std::string("staticcall")) +
			" requires " + std::to_string(expectedArgs) + " arguments", _loc
		);
		return;
	}

	// Normalize argument positions: call has extra `value` at position 2
	int argBase = _isCall ? 3 : 2;

	// Try to resolve the precompile address (arg index 1)
	auto precompileAddr = resolveConstantYulValue(_call.arguments[1]);
	if (!precompileAddr)
	{
		Logger::instance().error(
			(_isCall ? std::string("call") : std::string("staticcall")) +
			" with non-constant address not supported", _loc
		);
		return;
	}

	// Resolve input/output memory offsets and sizes
	auto inputOffset = resolveConstantYulValue(_call.arguments[argBase]);
	auto inputSize = resolveConstantYulValue(_call.arguments[argBase + 1]);
	auto outputOffset = resolveConstantYulValue(_call.arguments[argBase + 2]);
	auto outputSize = resolveConstantYulValue(_call.arguments[argBase + 3]);

	if (!inputOffset || !inputSize || !outputOffset || !outputSize)
	{
		Logger::instance().warning(
			"precompile call with non-constant memory offsets/sizes — stubbing as success", _loc
		);
		// Set success variable — use the variable's declared type (bool for Solidity bool)
		if (!_assignTarget.empty())
		{
			auto localIt = m_locals.find(_assignTarget);
			auto* varType = (localIt != m_locals.end()) ? localIt->second : awst::WType::biguintType();

			auto assignStmt = std::make_shared<awst::AssignmentStatement>();
			assignStmt->sourceLocation = _loc;
			auto varExpr = std::make_shared<awst::VarExpression>();
			varExpr->sourceLocation = _loc;
			varExpr->wtype = varType;
			varExpr->name = _assignTarget;
			assignStmt->target = std::move(varExpr);

			if (varType == awst::WType::boolType())
			{
				auto val = std::make_shared<awst::BoolConstant>();
				val->sourceLocation = _loc;
				val->wtype = awst::WType::boolType();
				val->value = true;
				assignStmt->value = std::move(val);
			}
			else
			{
				auto val = std::make_shared<awst::IntegerConstant>();
				val->sourceLocation = _loc;
				val->wtype = awst::WType::biguintType();
				val->value = "1";
				assignStmt->value = std::move(val);
			}
			_out.push_back(std::move(assignStmt));
		}
		return;
	}

	bool success = true;

	switch (*precompileAddr)
	{
	case 1: // ecRecover
		Logger::instance().debug("precompile 0x01: ecRecover", _loc);
		handleEcRecover(*inputOffset, *inputSize, *outputOffset, *outputSize, _loc, _out);
		break;

	case 2: // SHA-256
		Logger::instance().debug("precompile 0x02: SHA-256", _loc);
		handleSha256Precompile(*inputOffset, *inputSize, *outputOffset, *outputSize, _loc, _out);
		break;

	case 3: // RIPEMD-160
		Logger::instance().error(
			"precompile 0x03 (RIPEMD-160) not yet supported on AVM", _loc
		);
		success = false;
		break;

	case 4: // Identity (data copy)
		Logger::instance().debug("precompile 0x04: Identity", _loc);
		handleIdentityPrecompile(*inputOffset, *inputSize, *outputOffset, *outputSize, _loc, _out);
		break;

	case 5: // ModExp
		Logger::instance().debug("precompile 0x05: ModExp (square-and-multiply)", _loc);
		handleModExp(*inputOffset, *inputSize, *outputOffset, *outputSize, _loc, _out);
		break;

	case 6: // ecAdd
		Logger::instance().debug("precompile 0x06: ecAdd → AVM ec_add BN254g1", _loc);
		handleEcAdd(*inputOffset, *outputOffset, _loc, _out);
		break;

	case 7: // ecMul
		Logger::instance().debug("precompile 0x07: ecMul → AVM ec_scalar_mul BN254g1", _loc);
		handleEcMul(*inputOffset, *outputOffset, _loc, _out);
		break;

	case 8: // ecPairing
		Logger::instance().debug("precompile 0x08: ecPairing → AVM ec_pairing_check BN254g1", _loc);
		handleEcPairing(*inputOffset, *inputSize, *outputOffset, _loc, _out);
		break;

	case 9: // BLAKE2f
		Logger::instance().error(
			"precompile 0x09 (BLAKE2f) not yet supported on AVM", _loc
		);
		success = false;
		break;

	case 10: // KZG point evaluation
		Logger::instance().error(
			"precompile 0x0a (KZG point evaluation) not applicable on Algorand", _loc
		);
		success = false;
		break;

	default:
		Logger::instance().warning(
			(_isCall ? std::string("call") : std::string("staticcall")) +
			" to non-precompile address " + std::to_string(*precompileAddr) +
			" not implemented — stubbing as no-op", _loc
		);
		success = true;
		break;
	}

	// Set the success variable — use the variable's declared type (bool for Solidity bool)
	if (!_assignTarget.empty())
	{
		auto localIt = m_locals.find(_assignTarget);
		auto* varType = (localIt != m_locals.end()) ? localIt->second : awst::WType::biguintType();
		// Only set m_locals if not already tracked (preserve Solidity-declared type)
		if (localIt == m_locals.end())
			m_locals[_assignTarget] = varType;

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = _assignTarget;
		target->wtype = varType;

		std::shared_ptr<awst::Expression> val;
		if (varType == awst::WType::boolType())
		{
			auto boolVal = std::make_shared<awst::BoolConstant>();
			boolVal->sourceLocation = _loc;
			boolVal->wtype = awst::WType::boolType();
			boolVal->value = success;
			val = std::move(boolVal);
		}
		else
		{
			auto intVal = std::make_shared<awst::IntegerConstant>();
			intVal->sourceLocation = _loc;
			intVal->wtype = awst::WType::biguintType();
			intVal->value = success ? "1" : "0";
			val = std::move(intVal);
		}

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = std::move(val);
		_out.push_back(std::move(assign));
	}
}

// ─── BN254 precompile handlers ──────────────────────────────────────────────

void AssemblyTranslator::handleEcAdd(
	uint64_t _inputOffset, uint64_t _outputOffset,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// ecAdd: 4 input slots (x1,y1,x2,y2), 2 output slots (rx,ry)
	auto ecCall = std::make_shared<awst::IntrinsicCall>();
	ecCall->sourceLocation = _loc;
	ecCall->wtype = awst::WType::bytesType();
	ecCall->opCode = "ec_add";
	ecCall->immediates.push_back("BN254g1");
	ecCall->stackArgs.push_back(concatSlots(_inputOffset, 0, 2, _loc)); // point A
	ecCall->stackArgs.push_back(concatSlots(_inputOffset, 2, 2, _loc)); // point B
	storeResultToMemory(std::move(ecCall), _outputOffset, 2, _loc, _out);
}

void AssemblyTranslator::handleEcMul(
	uint64_t _inputOffset, uint64_t _outputOffset,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// ecMul: 3 input slots (x,y,s), 2 output slots (rx,ry)
	auto ecCall = std::make_shared<awst::IntrinsicCall>();
	ecCall->sourceLocation = _loc;
	ecCall->wtype = awst::WType::bytesType();
	ecCall->opCode = "ec_scalar_mul";
	ecCall->immediates.push_back("BN254g1");
	ecCall->stackArgs.push_back(concatSlots(_inputOffset, 0, 2, _loc)); // point A
	ecCall->stackArgs.push_back(concatSlots(_inputOffset, 2, 1, _loc)); // scalar
	storeResultToMemory(std::move(ecCall), _outputOffset, 2, _loc, _out);
}

void AssemblyTranslator::handleEcPairing(
	uint64_t _inputOffset, uint64_t _inputSize,
	uint64_t _outputOffset,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// ecPairing: variable inputs (6 words per pair), 1 output (bool)
	int inputSlots = static_cast<int>(_inputSize / 0x20);
	int numPairs = inputSlots / 6;

	auto ecCall = std::make_shared<awst::IntrinsicCall>();
	ecCall->sourceLocation = _loc;
	ecCall->wtype = awst::WType::boolType();
	ecCall->opCode = "ec_pairing_check";
	ecCall->immediates.push_back("BN254g1");

	if (numPairs > 0)
	{
		// Helper to concatenate two padded slots at absolute offsets
		auto concatTwoAbsSlots = [&](uint64_t offA, uint64_t offB) -> std::shared_ptr<awst::Expression>
		{
			auto a = padTo32Bytes(readMemSlot(offA, _loc), _loc);
			auto b = padTo32Bytes(readMemSlot(offB, _loc), _loc);
			auto c = std::make_shared<awst::IntrinsicCall>();
			c->sourceLocation = _loc;
			c->wtype = awst::WType::bytesType();
			c->opCode = "concat";
			c->stackArgs.push_back(std::move(a));
			c->stackArgs.push_back(std::move(b));
			return c;
		};

		std::shared_ptr<awst::Expression> g1Points;
		std::shared_ptr<awst::Expression> g2Points;
		for (int p = 0; p < numPairs; ++p)
		{
			uint64_t pairBase = _inputOffset + static_cast<uint64_t>(p * 6) * 0x20;
			// G1 point: 2 words (x, y) — same ordering in EVM and AVM
			auto g1 = concatSlots(_inputOffset, p * 6, 2, _loc);
			// G2 point: EVM stores as (x_im, x_re, y_im, y_re)
			// AVM expects (x_re, x_im, y_re, y_im) — swap within each coordinate pair
			auto g2_x = concatTwoAbsSlots(
				pairBase + 3 * 0x20, pairBase + 2 * 0x20 // x_re, x_im
			);
			auto g2_y = concatTwoAbsSlots(
				pairBase + 5 * 0x20, pairBase + 4 * 0x20 // y_re, y_im
			);
			auto g2 = std::make_shared<awst::IntrinsicCall>();
			g2->sourceLocation = _loc;
			g2->wtype = awst::WType::bytesType();
			g2->opCode = "concat";
			g2->stackArgs.push_back(std::move(g2_x));
			g2->stackArgs.push_back(std::move(g2_y));

			if (!g1Points)
				g1Points = std::move(g1);
			else
			{
				auto c = std::make_shared<awst::IntrinsicCall>();
				c->sourceLocation = _loc;
				c->wtype = awst::WType::bytesType();
				c->opCode = "concat";
				c->stackArgs.push_back(std::move(g1Points));
				c->stackArgs.push_back(std::move(g1));
				g1Points = std::move(c);
			}
			if (!g2Points)
				g2Points = std::move(g2);
			else
			{
				auto c = std::make_shared<awst::IntrinsicCall>();
				c->sourceLocation = _loc;
				c->wtype = awst::WType::bytesType();
				c->opCode = "concat";
				c->stackArgs.push_back(std::move(g2Points));
				c->stackArgs.push_back(std::move(g2));
				g2Points = std::move(c);
			}
		}
		if (g1Points) ecCall->stackArgs.push_back(std::move(g1Points));
		if (g2Points) ecCall->stackArgs.push_back(std::move(g2Points));
	}

	storeResultToMemory(std::move(ecCall), _outputOffset, 1, _loc, _out, /*_isBoolResult=*/true);
}

// ─── New precompile handlers ────────────────────────────────────────────────

void AssemblyTranslator::handleEcRecover(
	uint64_t _inputOffset, uint64_t /*_inputSize*/,
	uint64_t _outputOffset, uint64_t /*_outputSize*/,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Input (128 bytes = 4 slots): msgHash(0), v(1), r(2), s(3)
	// Output (32 bytes = 1 slot): left-padded 20-byte Ethereum address

	// 1. Read input slots as 32-byte padded values
	auto msgHash = padTo32Bytes(readMemSlot(_inputOffset, _loc), _loc);
	auto vBiguint = readMemSlot(_inputOffset + 0x20, _loc);
	auto r = padTo32Bytes(readMemSlot(_inputOffset + 0x40, _loc), _loc);
	auto s = padTo32Bytes(readMemSlot(_inputOffset + 0x60, _loc), _loc);

	// 2. Compute recovery_id = v - 27 as uint64
	auto twentySeven = std::make_shared<awst::IntegerConstant>();
	twentySeven->sourceLocation = _loc;
	twentySeven->wtype = awst::WType::biguintType();
	twentySeven->value = "27";

	auto vMinus27 = makeBigUIntBinOp(
		std::move(vBiguint), awst::BigUIntBinaryOperator::Sub,
		std::move(twentySeven), _loc
	);

	// Cast biguint → bytes → btoi → uint64
	auto vBytes = std::make_shared<awst::ReinterpretCast>();
	vBytes->sourceLocation = _loc;
	vBytes->wtype = awst::WType::bytesType();
	vBytes->expr = std::move(vMinus27);

	auto recoveryId = std::make_shared<awst::IntrinsicCall>();
	recoveryId->sourceLocation = _loc;
	recoveryId->wtype = awst::WType::uint64Type();
	recoveryId->opCode = "btoi";
	recoveryId->stackArgs.push_back(std::move(vBytes));

	// 3. Call ecdsa_pk_recover Secp256k1
	// Returns (bytes, bytes) — pubkey_x and pubkey_y, each 32 bytes
	auto tupleType = std::make_unique<awst::WTuple>(
		std::vector<awst::WType const*>{awst::WType::bytesType(), awst::WType::bytesType()}
	);
	awst::WType const* tupleTypePtr = tupleType.get();
	m_ownedTypes.push_back(std::move(tupleType));

	auto ecdsaRecover = std::make_shared<awst::IntrinsicCall>();
	ecdsaRecover->sourceLocation = _loc;
	ecdsaRecover->wtype = tupleTypePtr;
	ecdsaRecover->opCode = "ecdsa_pk_recover";
	ecdsaRecover->immediates.push_back("Secp256k1");
	ecdsaRecover->stackArgs.push_back(std::move(msgHash));
	ecdsaRecover->stackArgs.push_back(std::move(recoveryId));
	ecdsaRecover->stackArgs.push_back(std::move(r));
	ecdsaRecover->stackArgs.push_back(std::move(s));

	// Store the tuple result in a temporary
	std::string tupleVar = "__ecdsa_result";
	m_locals[tupleVar] = tupleTypePtr;

	auto tupleTarget = std::make_shared<awst::VarExpression>();
	tupleTarget->sourceLocation = _loc;
	tupleTarget->name = tupleVar;
	tupleTarget->wtype = tupleTypePtr;

	auto assignTuple = std::make_shared<awst::AssignmentStatement>();
	assignTuple->sourceLocation = _loc;
	assignTuple->target = tupleTarget;
	assignTuple->value = std::move(ecdsaRecover);
	_out.push_back(std::move(assignTuple));

	// 4. Extract pubkey_x (index 0) and pubkey_y (index 1)
	auto tupleRead0 = std::make_shared<awst::VarExpression>();
	tupleRead0->sourceLocation = _loc;
	tupleRead0->name = tupleVar;
	tupleRead0->wtype = tupleTypePtr;

	auto pubkeyX = std::make_shared<awst::TupleItemExpression>();
	pubkeyX->sourceLocation = _loc;
	pubkeyX->wtype = awst::WType::bytesType();
	pubkeyX->base = std::move(tupleRead0);
	pubkeyX->index = 0;

	auto tupleRead1 = std::make_shared<awst::VarExpression>();
	tupleRead1->sourceLocation = _loc;
	tupleRead1->name = tupleVar;
	tupleRead1->wtype = tupleTypePtr;

	auto pubkeyY = std::make_shared<awst::TupleItemExpression>();
	pubkeyY->sourceLocation = _loc;
	pubkeyY->wtype = awst::WType::bytesType();
	pubkeyY->base = std::move(tupleRead1);
	pubkeyY->index = 1;

	// 5. concat(pubkey_x, pubkey_y) → 64 bytes
	auto pubkeyConcat = std::make_shared<awst::IntrinsicCall>();
	pubkeyConcat->sourceLocation = _loc;
	pubkeyConcat->wtype = awst::WType::bytesType();
	pubkeyConcat->opCode = "concat";
	pubkeyConcat->stackArgs.push_back(std::move(pubkeyX));
	pubkeyConcat->stackArgs.push_back(std::move(pubkeyY));

	// 6. keccak256(concat) → 32 bytes
	auto hash = std::make_shared<awst::IntrinsicCall>();
	hash->sourceLocation = _loc;
	hash->wtype = awst::WType::bytesType();
	hash->opCode = "keccak256";
	hash->stackArgs.push_back(std::move(pubkeyConcat));

	// 7. extract3(hash, 12, 20) → last 20 bytes (Ethereum address)
	auto off12 = std::make_shared<awst::IntegerConstant>();
	off12->sourceLocation = _loc;
	off12->wtype = awst::WType::uint64Type();
	off12->value = "12";
	auto len20 = std::make_shared<awst::IntegerConstant>();
	len20->sourceLocation = _loc;
	len20->wtype = awst::WType::uint64Type();
	len20->value = "20";

	auto addr = std::make_shared<awst::IntrinsicCall>();
	addr->sourceLocation = _loc;
	addr->wtype = awst::WType::bytesType();
	addr->opCode = "extract3";
	addr->stackArgs.push_back(std::move(hash));
	addr->stackArgs.push_back(std::move(off12));
	addr->stackArgs.push_back(std::move(len20));

	// 8. Left-pad to 32 bytes: concat(bzero(12), addr)
	auto pad12 = std::make_shared<awst::IntrinsicCall>();
	pad12->sourceLocation = _loc;
	pad12->wtype = awst::WType::bytesType();
	pad12->opCode = "bzero";
	auto twelve = std::make_shared<awst::IntegerConstant>();
	twelve->sourceLocation = _loc;
	twelve->wtype = awst::WType::uint64Type();
	twelve->value = "12";
	pad12->stackArgs.push_back(std::move(twelve));

	auto paddedAddr = std::make_shared<awst::IntrinsicCall>();
	paddedAddr->sourceLocation = _loc;
	paddedAddr->wtype = awst::WType::bytesType();
	paddedAddr->opCode = "concat";
	paddedAddr->stackArgs.push_back(std::move(pad12));
	paddedAddr->stackArgs.push_back(std::move(addr));

	// 9. Cast to biguint and store
	auto addrBiguint = std::make_shared<awst::ReinterpretCast>();
	addrBiguint->sourceLocation = _loc;
	addrBiguint->wtype = awst::WType::biguintType();
	addrBiguint->expr = std::move(paddedAddr);

	storeResultToMemory(std::move(addrBiguint), _outputOffset, 1, _loc, _out);
}

void AssemblyTranslator::handleSha256Precompile(
	uint64_t _inputOffset, uint64_t _inputSize,
	uint64_t _outputOffset, uint64_t /*_outputSize*/,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Concatenate input memory slots
	int numFullSlots = static_cast<int>(_inputSize / 0x20);
	uint64_t remainder = _inputSize % 0x20;

	std::shared_ptr<awst::Expression> inputData;
	if (numFullSlots > 0)
		inputData = concatSlots(_inputOffset, 0, numFullSlots, _loc);

	// Handle partial last slot
	if (remainder > 0)
	{
		uint64_t partialOff = _inputOffset + static_cast<uint64_t>(numFullSlots) * 0x20;
		auto partialSlot = padTo32Bytes(readMemSlot(partialOff, _loc), _loc);

		// Truncate to just the remainder bytes: extract3(padded, 0, remainder)
		auto offZero = std::make_shared<awst::IntegerConstant>();
		offZero->sourceLocation = _loc;
		offZero->wtype = awst::WType::uint64Type();
		offZero->value = "0";
		auto remLen = std::make_shared<awst::IntegerConstant>();
		remLen->sourceLocation = _loc;
		remLen->wtype = awst::WType::uint64Type();
		remLen->value = std::to_string(remainder);

		auto truncated = std::make_shared<awst::IntrinsicCall>();
		truncated->sourceLocation = _loc;
		truncated->wtype = awst::WType::bytesType();
		truncated->opCode = "extract3";
		truncated->stackArgs.push_back(std::move(partialSlot));
		truncated->stackArgs.push_back(std::move(offZero));
		truncated->stackArgs.push_back(std::move(remLen));

		if (!inputData)
			inputData = std::move(truncated);
		else
		{
			auto concat = std::make_shared<awst::IntrinsicCall>();
			concat->sourceLocation = _loc;
			concat->wtype = awst::WType::bytesType();
			concat->opCode = "concat";
			concat->stackArgs.push_back(std::move(inputData));
			concat->stackArgs.push_back(std::move(truncated));
			inputData = std::move(concat);
		}
	}

	if (!inputData)
	{
		// Empty input: sha256 of empty bytes
		inputData = std::make_shared<awst::BytesConstant>();
		auto* bc = static_cast<awst::BytesConstant*>(inputData.get());
		bc->sourceLocation = _loc;
		bc->wtype = awst::WType::bytesType();
	}

	// If inputSize is a multiple of 32, also truncate to exact size
	// (concatSlots may include full 32-byte slots when input is exact)
	if (remainder == 0 && numFullSlots > 0)
	{
		// No truncation needed — concatSlots gives exactly numFullSlots*32 = inputSize
	}

	// Apply sha256
	auto sha256Call = std::make_shared<awst::IntrinsicCall>();
	sha256Call->sourceLocation = _loc;
	sha256Call->wtype = awst::WType::bytesType();
	sha256Call->opCode = "sha256";
	sha256Call->stackArgs.push_back(std::move(inputData));

	// Convert to biguint and store at output
	auto castResult = std::make_shared<awst::ReinterpretCast>();
	castResult->sourceLocation = _loc;
	castResult->wtype = awst::WType::biguintType();
	castResult->expr = std::move(sha256Call);

	storeResultToMemory(std::move(castResult), _outputOffset, 1, _loc, _out);
}

void AssemblyTranslator::handleModExp(
	uint64_t _inputOffset, uint64_t /*_inputSize*/,
	uint64_t _outputOffset, uint64_t /*_outputSize*/,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// ModExp input format (EIP-198):
	//   slot 0 (offset+0x00): Bsize (length of base in bytes)
	//   slot 1 (offset+0x20): Esize (length of exponent in bytes)
	//   slot 2 (offset+0x40): Msize (length of modulus in bytes)
	//   Then Bsize bytes of base, Esize bytes of exp, Msize bytes of mod
	//   packed contiguously starting at offset+0x60.
	//
	// Common Solidity usage: Bsize=Esize=Msize=32 (one slot each), so:
	//   slot 3 (offset+0x60): base
	//   slot 4 (offset+0x80): exp
	//   slot 5 (offset+0xa0): mod
	//   Output: one 32-byte slot with base^exp % mod
	//
	// We currently only support the fixed 32/32/32 case.
	// TODO: For variable-length inputs, parse Bsize/Esize/Msize dynamically.

	// Read Bsize, Esize, Msize to verify they're the common 32/32/32 pattern
	auto bsizeOpt = resolveConstantOffset(readMemSlot(_inputOffset, _loc));
	auto esizeOpt = resolveConstantOffset(readMemSlot(_inputOffset + 0x20, _loc));
	auto msizeOpt = resolveConstantOffset(readMemSlot(_inputOffset + 0x40, _loc));

	if (bsizeOpt && esizeOpt && msizeOpt
		&& *bsizeOpt != 32 && *esizeOpt != 32 && *msizeOpt != 32)
	{
		Logger::instance().warning(
			"modexp with non-32-byte operands (Bsize=" + std::to_string(*bsizeOpt) +
			", Esize=" + std::to_string(*esizeOpt) +
			", Msize=" + std::to_string(*msizeOpt) +
			"); proceeding with slot-based reads which may be incorrect", _loc
		);
	}

	// Read base, exp, mod from slots 3, 4, 5
	auto base = readMemSlot(_inputOffset + 0x60, _loc);
	auto exp = readMemSlot(_inputOffset + 0x80, _loc);
	auto mod = readMemSlot(_inputOffset + 0xa0, _loc);

	// Implement modular exponentiation via square-and-multiply:
	//
	//   __modexp_result = 1
	//   __modexp_base = base % mod
	//   __modexp_exp = exp
	//   while __modexp_exp > 0:
	//       if __modexp_exp & 1 != 0:       // exp is odd
	//           __modexp_result = (__modexp_result * __modexp_base) % mod
	//       __modexp_exp = __modexp_exp / 2
	//       __modexp_base = (__modexp_base * __modexp_base) % mod
	//   // result is in __modexp_result

	std::string resultVar = "__modexp_result";
	std::string baseVar = "__modexp_base";
	std::string expVar = "__modexp_exp";
	std::string modVar = "__modexp_mod";

	m_locals[resultVar] = awst::WType::biguintType();
	m_locals[baseVar] = awst::WType::biguintType();
	m_locals[expVar] = awst::WType::biguintType();
	m_locals[modVar] = awst::WType::biguintType();

	// Helper: make a VarExpression
	auto makeVar = [&](std::string const& name) -> std::shared_ptr<awst::VarExpression>
	{
		auto v = std::make_shared<awst::VarExpression>();
		v->sourceLocation = _loc;
		v->name = name;
		v->wtype = awst::WType::biguintType();
		return v;
	};

	// Helper: make a biguint constant
	auto makeConst = [&](std::string const& value) -> std::shared_ptr<awst::IntegerConstant>
	{
		auto c = std::make_shared<awst::IntegerConstant>();
		c->sourceLocation = _loc;
		c->wtype = awst::WType::biguintType();
		c->value = value;
		return c;
	};

	// Helper: make an assignment statement
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

	// __modexp_mod = mod
	_out.push_back(makeAssign(modVar, std::move(mod)));

	// __modexp_result = 1
	_out.push_back(makeAssign(resultVar, makeConst("1")));

	// __modexp_base = base % mod
	_out.push_back(makeAssign(baseVar,
		makeBigUIntBinOp(std::move(base), awst::BigUIntBinaryOperator::Mod, makeVar(modVar), _loc)
	));

	// __modexp_exp = exp
	_out.push_back(makeAssign(expVar, std::move(exp)));

	// Build loop: while __modexp_exp > 0
	auto loop = std::make_shared<awst::WhileLoop>();
	loop->sourceLocation = _loc;

	// Condition: __modexp_exp > 0
	auto cond = std::make_shared<awst::NumericComparisonExpression>();
	cond->sourceLocation = _loc;
	cond->wtype = awst::WType::boolType();
	cond->lhs = makeVar(expVar);
	cond->op = awst::NumericComparison::Gt;
	cond->rhs = makeConst("0");
	loop->condition = std::move(cond);

	// Loop body
	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = _loc;

	// 1. if __modexp_exp & 1 != 0:
	//        __modexp_result = (__modexp_result * __modexp_base) % __modexp_mod
	{
		// __modexp_exp & 1: use BigUIntBinaryOperation with BitAnd
		auto expAnd1 = makeBigUIntBinOp(
			makeVar(expVar), awst::BigUIntBinaryOperator::BitAnd, makeConst("1"), _loc
		);

		// expAnd1 != 0
		auto isOdd = std::make_shared<awst::NumericComparisonExpression>();
		isOdd->sourceLocation = _loc;
		isOdd->wtype = awst::WType::boolType();
		isOdd->lhs = std::move(expAnd1);
		isOdd->op = awst::NumericComparison::Ne;
		isOdd->rhs = makeConst("0");

		// result = (result * base) % mod
		auto product = makeBigUIntBinOp(
			makeVar(resultVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar), _loc
		);
		auto modResult = makeBigUIntBinOp(
			std::move(product), awst::BigUIntBinaryOperator::Mod, makeVar(modVar), _loc
		);

		auto ifBlock = std::make_shared<awst::Block>();
		ifBlock->sourceLocation = _loc;
		ifBlock->body.push_back(makeAssign(resultVar, std::move(modResult)));

		auto ifStmt = std::make_shared<awst::IfElse>();
		ifStmt->sourceLocation = _loc;
		ifStmt->condition = std::move(isOdd);
		ifStmt->ifBranch = std::move(ifBlock);

		body->body.push_back(std::move(ifStmt));
	}

	// 2. __modexp_exp = __modexp_exp / 2
	body->body.push_back(makeAssign(expVar,
		makeBigUIntBinOp(makeVar(expVar), awst::BigUIntBinaryOperator::FloorDiv, makeConst("2"), _loc)
	));

	// 3. __modexp_base = (__modexp_base * __modexp_base) % __modexp_mod
	{
		auto squared = makeBigUIntBinOp(
			makeVar(baseVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar), _loc
		);
		auto modSquared = makeBigUIntBinOp(
			std::move(squared), awst::BigUIntBinaryOperator::Mod, makeVar(modVar), _loc
		);
		body->body.push_back(makeAssign(baseVar, std::move(modSquared)));
	}

	loop->loopBody = std::move(body);
	_out.push_back(std::move(loop));

	// Store __modexp_result at the output offset
	storeResultToMemory(makeVar(resultVar), _outputOffset, 1, _loc, _out);
}

void AssemblyTranslator::handleIdentityPrecompile(
	uint64_t _inputOffset, uint64_t _inputSize,
	uint64_t _outputOffset, uint64_t /*_outputSize*/,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Identity precompile: copy input memory slots to output slots
	int numSlots = static_cast<int>(_inputSize / 0x20);
	for (int i = 0; i < numSlots; ++i)
	{
		uint64_t inOff = _inputOffset + static_cast<uint64_t>(i) * 0x20;
		uint64_t outOff = _outputOffset + static_cast<uint64_t>(i) * 0x20;

		auto value = readMemSlot(inOff, _loc);
		std::string varName = getOrCreateMemoryVar(outOff, _loc);
		m_locals[varName] = awst::WType::biguintType();

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = varName;
		target->wtype = awst::WType::biguintType();

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = std::move(value);
		_out.push_back(std::move(assign));
	}
}

std::shared_ptr<awst::Expression> AssemblyTranslator::handleMload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (_args.size() != 1)
	{
		Logger::instance().error("mload requires 1 argument", _loc);
		return nullptr;
	}

	auto offset = resolveConstantOffset(_args[0]);
	if (!offset)
	{
		Logger::instance().warning(
			"mload with non-constant offset not supported — returning 0 (EVM memory model)", _loc
		);
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";
		return zero;
	}

	auto it = m_memoryMap.find(*offset);
	if (it != m_memoryMap.end())
	{
		auto const& slot = it->second;
		if (slot.isParam)
		{
			// Access array parameter element: param[index]
			auto base = std::make_shared<awst::VarExpression>();
			base->sourceLocation = _loc;
			base->name = slot.varName;
			base->wtype = m_arrayParamType;

			auto index = std::make_shared<awst::IntegerConstant>();
			index->sourceLocation = _loc;
			index->wtype = awst::WType::uint64Type();
			index->value = std::to_string(slot.paramIndex);

			auto indexExpr = std::make_shared<awst::IndexExpression>();
			indexExpr->sourceLocation = _loc;
			indexExpr->wtype = awst::WType::biguintType();
			indexExpr->base = std::move(base);
			indexExpr->index = std::move(index);
			return indexExpr;
		}
		else
		{
			// Read from scratch variable
			auto var = std::make_shared<awst::VarExpression>();
			var->sourceLocation = _loc;
			var->name = slot.varName;
			var->wtype = awst::WType::biguintType();
			return var;
		}
	}

	// Check if the offset falls within m_calldataMap (function parameters).
	// This handles mload from calldata struct field offsets, e.g., mload(key + 32)
	// where key is a struct parameter at calldata offset 4.
	auto cdIt = m_calldataMap.find(*offset);
	if (cdIt != m_calldataMap.end())
	{
		auto const& elem = cdIt->second;
		auto base = std::make_shared<awst::VarExpression>();
		base->sourceLocation = _loc;
		base->name = elem.paramName;
		base->wtype = m_locals.count(elem.paramName)
			? m_locals[elem.paramName]
			: awst::WType::biguintType();

		return accessFlatElement(std::move(base), elem.paramType, elem.flatIndex, _loc);
	}

	// Unknown memory offset — create a scratch variable on the fly
	std::string varName = "mem_0x" + ([&] {
		std::ostringstream oss;
		oss << std::hex << *offset;
		return oss.str();
	})();

	MemorySlot slot;
	slot.varName = varName;
	m_memoryMap[*offset] = slot;

	auto var = std::make_shared<awst::VarExpression>();
	var->sourceLocation = _loc;
	var->name = varName;
	var->wtype = awst::WType::biguintType();
	return var;
}

std::shared_ptr<awst::Expression> AssemblyTranslator::tryHandleBytesMemoryRead(
	solidity::yul::Expression const& _addrExpr,
	awst::SourceLocation const& _loc
)
{
	// Match: mload(add(add(bytes_param, 32), offset))
	// or:    mload(add(offset, add(bytes_param, 32)))
	//
	// This is the standard Solidity pattern for reading 32 bytes from a
	// bytes memory parameter at a variable byte offset.
	// In EVM: data_ptr + 32 (skip length header) + offset → mload → 32 bytes
	// In AVM: extract3(data, offset, 32) — bytes have no length header

	auto* outerAdd = std::get_if<solidity::yul::FunctionCall>(&_addrExpr);
	if (!outerAdd || outerAdd->functionName.name.str() != "add"
		|| outerAdd->arguments.size() != 2)
		return nullptr;

	// One arg of outer add should be add(bytes_param, 32), the other is the offset
	solidity::yul::FunctionCall const* innerAdd = nullptr;
	solidity::yul::Expression const* offsetExprYul = nullptr;

	auto* call0 = std::get_if<solidity::yul::FunctionCall>(&outerAdd->arguments[0]);
	auto* call1 = std::get_if<solidity::yul::FunctionCall>(&outerAdd->arguments[1]);

	if (call0 && call0->functionName.name.str() == "add" && call0->arguments.size() == 2)
	{
		innerAdd = call0;
		offsetExprYul = &outerAdd->arguments[1];
	}
	else if (call1 && call1->functionName.name.str() == "add" && call1->arguments.size() == 2)
	{
		innerAdd = call1;
		offsetExprYul = &outerAdd->arguments[0];
	}

	if (!innerAdd)
		return nullptr;

	// Inner add should have: (bytes_param, 32) or (32, bytes_param)
	solidity::yul::Expression const* paramExpr = nullptr;

	auto val1 = resolveConstantYulValue(innerAdd->arguments[1]);
	if (val1 && *val1 == 32)
	{
		paramExpr = &innerAdd->arguments[0];
	}
	else
	{
		auto val0 = resolveConstantYulValue(innerAdd->arguments[0]);
		if (val0 && *val0 == 32)
			paramExpr = &innerAdd->arguments[1];
	}

	if (!paramExpr)
		return nullptr;

	// param must be an Identifier referencing a bytes/string parameter
	auto* paramId = std::get_if<solidity::yul::Identifier>(paramExpr);
	if (!paramId)
		return nullptr;

	std::string paramName = paramId->name.str();
	auto paramIt = m_locals.find(paramName);
	if (paramIt == m_locals.end())
		return nullptr;

	auto* paramType = paramIt->second;
	if (paramType != awst::WType::bytesType() && paramType != awst::WType::stringType())
		return nullptr;

	// Pattern matched! Generate: extract3(param, btoi(offset), 32) → cast to biguint

	Logger::instance().debug(
		"mload bytes memory read: extract3(" + paramName + ", offset, 32)", _loc
	);

	// Build param reference
	auto paramVar = std::make_shared<awst::VarExpression>();
	paramVar->sourceLocation = _loc;
	paramVar->name = paramName;
	paramVar->wtype = paramType;

	// Translate the dynamic offset and convert biguint → uint64
	auto offsetExpr = translateExpression(*offsetExprYul);

	auto offsetBytes = std::make_shared<awst::ReinterpretCast>();
	offsetBytes->sourceLocation = _loc;
	offsetBytes->wtype = awst::WType::bytesType();
	offsetBytes->expr = offsetExpr;

	auto offsetU64 = std::make_shared<awst::IntrinsicCall>();
	offsetU64->sourceLocation = _loc;
	offsetU64->wtype = awst::WType::uint64Type();
	offsetU64->opCode = "btoi";
	offsetU64->stackArgs.push_back(std::move(offsetBytes));

	// Length: 32 bytes
	auto lenArg = std::make_shared<awst::IntegerConstant>();
	lenArg->sourceLocation = _loc;
	lenArg->wtype = awst::WType::uint64Type();
	lenArg->value = "32";

	// extract3(param, offset, 32)
	auto extract = std::make_shared<awst::IntrinsicCall>();
	extract->sourceLocation = _loc;
	extract->wtype = awst::WType::bytesType();
	extract->opCode = "extract3";
	extract->stackArgs.push_back(std::move(paramVar));
	extract->stackArgs.push_back(std::move(offsetU64));
	extract->stackArgs.push_back(std::move(lenArg));

	// Cast bytes → biguint (mload returns uint256)
	auto result = std::make_shared<awst::ReinterpretCast>();
	result->sourceLocation = _loc;
	result->wtype = awst::WType::biguintType();
	result->expr = std::move(extract);

	return result;
}

void AssemblyTranslator::handleMstore(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("mstore requires 2 arguments", _loc);
		return;
	}

	// Track last mstore value for dynamic-length keccak256 patterns
	m_lastMstoreValue = _args[1];

	auto offset = resolveConstantOffset(_args[0]);
	if (!offset)
	{
		// Try to decompose as VarExpression + constant offset
		auto decomposed = decomposeVarOffset(_args[0]);
		if (!decomposed)
		{
			Logger::instance().warning(
				"mstore with non-constant offset not supported in assembly translation (skipping)", _loc
			);
			return;
		}
		// Track in variable-offset memory map
		auto const& [baseName, relOffset] = *decomposed;
		std::string varName = "vmem_" + baseName + "_0x" + ([&] {
			std::ostringstream oss;
			oss << std::hex << relOffset;
			return oss.str();
		})();
		MemorySlot slot;
		slot.varName = varName;
		m_varMemoryMap[baseName][relOffset] = slot;

		if (m_locals.find(varName) == m_locals.end())
			m_locals[varName] = awst::WType::biguintType();

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = varName;
		target->wtype = awst::WType::biguintType();

		auto storeValue = _args[1];
		if (storeValue->wtype != awst::WType::biguintType())
		{
			if (storeValue->wtype->kind() == awst::WTypeKind::Bytes
				|| storeValue->wtype == awst::WType::accountType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = _loc;
				cast->wtype = awst::WType::biguintType();
				cast->expr = std::move(storeValue);
				storeValue = std::move(cast);
			}
			else if (storeValue->wtype == awst::WType::uint64Type())
			{
				auto itob = std::make_shared<awst::IntrinsicCall>();
				itob->sourceLocation = _loc;
				itob->wtype = awst::WType::bytesType();
				itob->opCode = "itob";
				itob->stackArgs.push_back(std::move(storeValue));
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = _loc;
				cast->wtype = awst::WType::biguintType();
				cast->expr = std::move(itob);
				storeValue = std::move(cast);
			}
		}

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = std::move(storeValue);
		_out.push_back(std::move(assign));
		return;
	}

	// Find or create the scratch variable for this offset
	std::string varName;
	auto it = m_memoryMap.find(*offset);
	if (it != m_memoryMap.end())
	{
		if (it->second.isParam)
		{
			// Param slot is being overwritten — shadow it with a scratch variable.
			// After this, mload at this offset will read the scratch var, not the param.
			varName = "mem_0x" + ([&] {
				std::ostringstream oss;
				oss << std::hex << *offset;
				return oss.str();
			})();
			MemorySlot slot;
			slot.varName = varName;
			slot.isParam = false;
			slot.paramIndex = -1;
			it->second = slot;
		}
		else
		{
			varName = it->second.varName;
		}
	}
	else
	{
		varName = "mem_0x" + ([&] {
			std::ostringstream oss;
			oss << std::hex << *offset;
			return oss.str();
		})();
		MemorySlot slot;
		slot.varName = varName;
		m_memoryMap[*offset] = slot;
	}

	// Register as local if not already
	if (m_locals.find(varName) == m_locals.end())
		m_locals[varName] = awst::WType::biguintType();

	// Track constant values stored to memory (especially free memory pointer)
	auto storedVal = resolveConstantOffset(_args[1]);
	if (storedVal)
		m_localConstants[varName] = *storedVal;

	auto target = std::make_shared<awst::VarExpression>();
	target->sourceLocation = _loc;
	target->name = varName;
	target->wtype = awst::WType::biguintType();

	// Coerce value to biguint to match memory slot type (EVM mstore = 256-bit write)
	auto storeValue = _args[1];
	if (storeValue->wtype != awst::WType::biguintType())
	{
		if (storeValue->wtype->kind() == awst::WTypeKind::Bytes
			|| storeValue->wtype == awst::WType::accountType())
		{
			// bytes/account → biguint via ReinterpretCast
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(storeValue);
			storeValue = std::move(cast);
		}
		else if (storeValue->wtype == awst::WType::uint64Type())
		{
			// uint64 → bytes via itob → biguint via ReinterpretCast
			auto itob = std::make_shared<awst::IntrinsicCall>();
			itob->sourceLocation = _loc;
			itob->wtype = awst::WType::bytesType();
			itob->opCode = "itob";
			itob->stackArgs.push_back(std::move(storeValue));

			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(itob);
			storeValue = std::move(cast);
		}
	}

	auto assign = std::make_shared<awst::AssignmentStatement>();
	assign->sourceLocation = _loc;
	assign->target = std::move(target);
	assign->value = std::move(storeValue);
	_out.push_back(std::move(assign));
}

void AssemblyTranslator::handleReturn(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (_args.size() != 2)
	{
		Logger::instance().error("return requires 2 arguments", _loc);
		return;
	}

	// return(offset, size): Return the value stored at memory[offset]
	auto offset = resolveConstantOffset(_args[0]);
	if (!offset)
	{
		Logger::instance().error(
			"return with non-constant offset not supported", _loc
		);
		return;
	}

	// Look up what was stored at this offset
	std::shared_ptr<awst::Expression> returnValue;
	auto it = m_memoryMap.find(*offset);
	if (it != m_memoryMap.end())
	{
		auto var = std::make_shared<awst::VarExpression>();
		var->sourceLocation = _loc;
		var->name = it->second.varName;
		var->wtype = awst::WType::biguintType();
		returnValue = std::move(var);
	}
	else
	{
		// Offset not in memory map — return zero
		Logger::instance().warning(
			"return from unknown memory offset; defaulting to zero", _loc
		);
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = m_returnType ? m_returnType : awst::WType::biguintType();
		zero->value = "0";
		returnValue = std::move(zero);
	}

	// Convert to bool if the function's return type is bool
	if (m_returnType == awst::WType::boolType()
		&& returnValue->wtype != awst::WType::boolType())
	{
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";

		auto cmp = std::make_shared<awst::NumericComparisonExpression>();
		cmp->sourceLocation = _loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = std::move(returnValue);
		cmp->op = awst::NumericComparison::Ne;
		cmp->rhs = std::move(zero);
		returnValue = std::move(cmp);
	}

	auto ret = std::make_shared<awst::ReturnStatement>();
	ret->sourceLocation = _loc;
	ret->value = std::move(returnValue);
	_out.push_back(std::move(ret));
}

// ─── Statement translation ─────────────────────────────────────────────────

void AssemblyTranslator::translateStatement(
	solidity::yul::Statement const& _stmt,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	std::visit(
		[this, &_out](auto const& _node) {
			using T = std::decay_t<decltype(_node)>;
			if constexpr (std::is_same_v<T, solidity::yul::VariableDeclaration>)
				translateVariableDeclaration(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Assignment>)
				translateAssignment(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::ExpressionStatement>)
				translateExpressionStatement(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::FunctionDefinition>)
				translateFunctionDefinition(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Block>)
			{
				// Nested block — translate all its statements
				for (auto const& innerStmt: _node.statements)
					translateStatement(innerStmt, _out);
			}
			else if constexpr (std::is_same_v<T, solidity::yul::If>)
			{
				// Yul if (no else)
				auto loc = makeLoc(_node.debugData);

				// Check if body is a revert-only block (common SafeCast/require pattern).
				// Emitting assert(NOT(cond)) directly avoids puya DCE of if(cond){assert(false)}.
				bool isRevertBody = false;
				for (auto const& stmt : _node.body.statements)
				{
					if (auto const* exprStmt = std::get_if<solidity::yul::ExpressionStatement>(&stmt))
					{
						if (auto const* funcCall = std::get_if<solidity::yul::FunctionCall>(&exprStmt->expression))
						{
							if (funcCall->functionName.name.str() == "revert")
								isRevertBody = true;
						}
					}
				}

				if (isRevertBody)
				{
					// Emit assert(NOT(condition)) — avoids DCE of if(cond){assert(false)}
					auto cond = ensureBool(translateExpression(*_node.condition), loc);
					auto notCond = std::make_shared<awst::Not>();
					notCond->sourceLocation = loc;
					notCond->wtype = awst::WType::boolType();
					notCond->expr = std::move(cond);

					auto assertExpr = std::make_shared<awst::AssertExpression>();
					assertExpr->sourceLocation = loc;
					assertExpr->wtype = awst::WType::voidType();
					assertExpr->condition = std::move(notCond);
					assertExpr->errorMessage = "revert";

					auto stmt = std::make_shared<awst::ExpressionStatement>();
					stmt->sourceLocation = loc;
					stmt->expr = std::move(assertExpr);
					_out.push_back(std::move(stmt));
				}
				else
				{
					// Original IfElse path for non-revert if-bodies
					auto ifElse = std::make_shared<awst::IfElse>();
					ifElse->sourceLocation = loc;
					ifElse->condition = ensureBool(translateExpression(*_node.condition), loc);

					auto ifBlock = std::make_shared<awst::Block>();
					ifBlock->sourceLocation = loc;
					for (auto const& innerStmt: _node.body.statements)
						translateStatement(innerStmt, ifBlock->body);
					ifElse->ifBranch = std::move(ifBlock);

					_out.push_back(std::move(ifElse));
				}
			}
			else if constexpr (std::is_same_v<T, solidity::yul::ForLoop>)
			{
				auto loc = makeLoc(_node.debugData);
				// Translate pre block
				for (auto const& preStmt: _node.pre.statements)
					translateStatement(preStmt, _out);

				auto loop = std::make_shared<awst::WhileLoop>();
				loop->sourceLocation = loc;
				loop->condition = ensureBool(translateExpression(*_node.condition), loc);

				auto body = std::make_shared<awst::Block>();
				body->sourceLocation = loc;
				for (auto const& bodyStmt: _node.body.statements)
					translateStatement(bodyStmt, body->body);
				for (auto const& postStmt: _node.post.statements)
					translateStatement(postStmt, body->body);
				loop->loopBody = std::move(body);

				_out.push_back(std::move(loop));
			}
			else if constexpr (std::is_same_v<T, solidity::yul::Break>)
			{
				auto stmt = std::make_shared<awst::LoopExit>();
				stmt->sourceLocation = makeLoc(_node.debugData);
				_out.push_back(std::move(stmt));
			}
			else if constexpr (std::is_same_v<T, solidity::yul::Continue>)
			{
				auto stmt = std::make_shared<awst::LoopContinue>();
				stmt->sourceLocation = makeLoc(_node.debugData);
				_out.push_back(std::move(stmt));
			}
			else if constexpr (std::is_same_v<T, solidity::yul::Leave>)
			{
				// Leave = return from assembly function; handled as a return
				auto ret = std::make_shared<awst::ReturnStatement>();
				ret->sourceLocation = makeLoc(_node.debugData);
				_out.push_back(std::move(ret));
			}
			else if constexpr (std::is_same_v<T, solidity::yul::Switch>)
			{
				auto loc = makeLoc(_node.debugData);
				auto switchExpr = translateExpression(*_node.expression);

				// Build if-else chain from switch cases
				std::shared_ptr<awst::Statement> chain;
				std::shared_ptr<awst::Block> defaultBlock;

				for (auto const& yulCase: _node.cases)
				{
					auto caseBlock = std::make_shared<awst::Block>();
					caseBlock->sourceLocation = makeLoc(yulCase.debugData);
					for (auto const& stmt: yulCase.body.statements)
						translateStatement(stmt, caseBlock->body);

					if (!yulCase.value)
					{
						defaultBlock = std::move(caseBlock);
					}
					else
					{
						auto caseVal = translateLiteral(*yulCase.value);
						auto cmp = std::make_shared<awst::NumericComparisonExpression>();
						cmp->sourceLocation = loc;
						cmp->wtype = awst::WType::boolType();
						cmp->lhs = switchExpr;
						cmp->op = awst::NumericComparison::Eq;
						cmp->rhs = std::move(caseVal);

						auto ifElse = std::make_shared<awst::IfElse>();
						ifElse->sourceLocation = loc;
						ifElse->condition = std::move(cmp);
						ifElse->ifBranch = std::move(caseBlock);

						if (chain)
						{
							// Chain: previous if-else gets this as elseBranch
							auto prevIf = std::dynamic_pointer_cast<awst::IfElse>(chain);
							if (prevIf)
							{
								auto elseBlock = std::make_shared<awst::Block>();
								elseBlock->sourceLocation = loc;
								elseBlock->body.push_back(ifElse);
								prevIf->elseBranch = std::move(elseBlock);
							}
						}
						chain = ifElse;
					}
				}

				// Attach default case
				if (defaultBlock && chain)
				{
					auto lastIf = std::dynamic_pointer_cast<awst::IfElse>(chain);
					if (lastIf)
						lastIf->elseBranch = std::move(defaultBlock);
				}

				if (chain)
					_out.push_back(std::move(chain));
				else if (defaultBlock)
				{
					for (auto& stmt: defaultBlock->body)
						_out.push_back(std::move(stmt));
				}
			}
		},
		_stmt
	);
}

void AssemblyTranslator::translateVariableDeclaration(
	solidity::yul::VariableDeclaration const& _decl,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_decl.debugData);

	// Check for special function call patterns: staticcall, user-defined functions
	if (_decl.value && _decl.variables.size() == 1)
	{
		if (auto const* call = std::get_if<solidity::yul::FunctionCall>(_decl.value.get()))
		{
			std::string callName = call->functionName.name.str();
			if (callName == "staticcall" || callName == "call")
			{
				std::string varName = _decl.variables[0].name.str();
				handlePrecompileCall(*call, varName, loc, _out, /*_isCall=*/callName == "call");
				return;
			}

			// User-defined assembly function called in variable declaration context
			// (e.g., let isValid := checkPairing(...))
			if (m_asmFunctions.count(callName))
			{
				std::string varName = _decl.variables[0].name.str();
				m_locals[varName] = awst::WType::biguintType();

				// Translate arguments
				std::vector<std::shared_ptr<awst::Expression>> args;
				for (auto const& arg: call->arguments)
					args.push_back(translateExpression(arg));

				// Inline the function (writes to return variable)
				handleUserFunctionCall(callName, args, loc, _out);

				// The return variable of the inlined function is the result.
				// Find its name from the function definition's return variables.
				auto const& funcDef = *m_asmFunctions[callName];
				if (!funcDef.returnVariables.empty())
				{
					std::string retName = funcDef.returnVariables[0].name.str();
					auto retVar = std::make_shared<awst::VarExpression>();
					retVar->sourceLocation = loc;
					retVar->name = retName;
					retVar->wtype = awst::WType::biguintType();

					auto target = std::make_shared<awst::VarExpression>();
					target->sourceLocation = loc;
					target->name = varName;
					target->wtype = awst::WType::biguintType();

					auto assign = std::make_shared<awst::AssignmentStatement>();
					assign->sourceLocation = loc;
					assign->target = std::move(target);
					assign->value = std::move(retVar);
					_out.push_back(std::move(assign));
				}
				return;
			}
		}
	}

	for (auto const& var: _decl.variables)
	{
		std::string name = var.name.str();
		m_locals[name] = awst::WType::biguintType();

		// Try to resolve compile-time constant value for tracking
		if (_decl.value)
		{
			auto constVal = resolveConstantYulValue(*_decl.value);
			if (constVal)
				m_localConstants[name] = *constVal;
		}
		else
		{
			m_localConstants[name] = 0;
		}

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = makeLoc(var.debugData);
		target->wtype = awst::WType::biguintType();
		target->name = name;

		std::shared_ptr<awst::Expression> value;
		if (_decl.value)
		{
			value = translateExpression(*_decl.value);
			if (!value)
			{
				// Expression failed to translate (error already logged), use zero fallback
				auto zero = std::make_shared<awst::IntegerConstant>();
				zero->sourceLocation = loc;
				zero->wtype = awst::WType::biguintType();
				zero->value = "0";
				value = std::move(zero);
			}
		}
		else
		{
			// Default: zero
			auto zero = std::make_shared<awst::IntegerConstant>();
			zero->sourceLocation = loc;
			zero->wtype = awst::WType::biguintType();
			zero->value = "0";
			value = std::move(zero);
		}

		// Coerce value to match target (biguint) — Yul values are always 256-bit
		value = ensureBiguint(std::move(value), loc);

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = loc;
		assign->target = std::move(target);
		assign->value = std::move(value);
		_out.push_back(std::move(assign));
	}
}

void AssemblyTranslator::translateAssignment(
	solidity::yul::Assignment const& _assign,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_assign.debugData);

	if (_assign.variableNames.size() != 1)
	{
		Logger::instance().error(
			"multi-variable assignment not yet supported in assembly translation", loc
		);
		return;
	}

	std::string name = _assign.variableNames[0].name.str();

	// Skip ERC-7201 storage slot assignments: $.slot := CONSTANT
	// These set EVM storage base positions, which have no AVM equivalent
	if (name.find(".slot") != std::string::npos)
	{
		return;
	}

	// Check for staticcall pattern: success := staticcall(...)
	if (_assign.value)
	{
		if (auto const* call = std::get_if<solidity::yul::FunctionCall>(_assign.value.get()))
		{
			std::string callName = call->functionName.name.str();
			if (callName == "staticcall" || callName == "call")
			{
				handlePrecompileCall(*call, name, loc, _out, /*_isCall=*/callName == "call");
				return;
			}
		}
	}

	auto target = std::make_shared<awst::VarExpression>();
	target->sourceLocation = loc;
	target->name = name;

	auto it = m_locals.find(name);
	target->wtype = (it != m_locals.end()) ? it->second : awst::WType::biguintType();

	auto value = translateExpression(*_assign.value);
	if (!value)
	{
		// Expression failed to translate (error already logged), use zero fallback
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = loc;
		zero->wtype = target->wtype;
		zero->value = "0";
		value = std::move(zero);
	}

	// Coerce value type to match target type when they differ
	if (target->wtype != value->wtype)
	{
		if (target->wtype == awst::WType::biguintType())
		{
			// Target is biguint — coerce value to biguint
			value = ensureBiguint(std::move(value), loc);
		}
		else if (target->wtype == awst::WType::boolType())
		{
			// Target is bool — coerce value to bool
			value = ensureBool(std::move(value), loc);
		}
		else if (target->wtype->kind() == awst::WTypeKind::Bytes)
		{
			// Target is bytes — coerce biguint to bytes via ReinterpretCast
			if (value->wtype == awst::WType::biguintType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = target->wtype;
				cast->expr = std::move(value);
				value = std::move(cast);
			}
			else
			{
				// uint64/bool → biguint → bytes
				auto biguintVal = ensureBiguint(std::move(value), loc);
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = target->wtype;
				cast->expr = std::move(biguintVal);
				value = std::move(cast);
			}
		}
		else if (target->wtype == awst::WType::uint64Type())
		{
			// Target is uint64 but value is biguint (e.g. from mload).
			// Truncate to uint64 using safeBtoi to keep the variable type consistent
			// across all control flow paths (avoids phi node type mismatches).
			if (value->wtype == awst::WType::biguintType())
			{
				value = safeBtoi(std::move(value), loc);
			}
		}
		else if (target->wtype == awst::WType::accountType())
		{
			// Target is account — coerce biguint/bytes to account
			if (value->wtype == awst::WType::biguintType())
			{
				// biguint → bytes → account
				auto toBytes = std::make_shared<awst::ReinterpretCast>();
				toBytes->sourceLocation = loc;
				toBytes->wtype = awst::WType::bytesType();
				toBytes->expr = std::move(value);

				auto toAccount = std::make_shared<awst::ReinterpretCast>();
				toAccount->sourceLocation = loc;
				toAccount->wtype = awst::WType::accountType();
				toAccount->expr = std::move(toBytes);
				value = std::move(toAccount);
			}
			else if (value->wtype != awst::WType::accountType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = awst::WType::accountType();
				cast->expr = std::move(value);
				value = std::move(cast);
			}
		}
	}

	auto assign = std::make_shared<awst::AssignmentStatement>();
	assign->sourceLocation = loc;
	assign->target = std::move(target);
	assign->value = std::move(value);
	_out.push_back(std::move(assign));
}

void AssemblyTranslator::translateExpressionStatement(
	solidity::yul::ExpressionStatement const& _stmt,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_stmt.debugData);

	// Expression statements in Yul are typically side-effecting calls
	// like mstore(), return(), or user-defined function calls.
	if (auto const* call = std::get_if<solidity::yul::FunctionCall>(&_stmt.expression))
	{
		std::string funcName = call->functionName.name.str();

		// Translate arguments (stored in source order)
		std::vector<std::shared_ptr<awst::Expression>> args;
		for (auto const& arg: call->arguments)
			args.push_back(translateExpression(arg));

		if (funcName == "mstore")
		{
			handleMstore(args, loc, _out);
			return;
		}
		if (funcName == "return")
		{
			handleReturn(args, loc, _out);
			return;
		}
		if (funcName == "staticcall" || funcName == "call")
		{
			handlePrecompileCall(*call, "", loc, _out, /*_isCall=*/funcName == "call");
			return;
		}
		if (funcName == "revert")
		{
			handleRevert(args, loc, _out);
			return;
		}
		if (funcName == "tstore")
		{
			handleTstore(args, loc, _out);
			return;
		}
		if (funcName == "sstore")
		{
			handleSstore(args, loc, _out);
			return;
		}
		if (funcName == "returndatacopy" || funcName == "pop")
		{
			// No-op on AVM (returndatacopy: no return data; pop: discard value)
			return;
		}
		if (funcName == "delegatecall")
		{
			// delegatecall has no AVM equivalent — stub as no-op
			Logger::instance().warning(
				"delegatecall() has no AVM equivalent — stubbed as no-op",
				loc
			);
			return;
		}
		if (funcName == "mcopy")
		{
			// mcopy(dst, src, length): copy memory slot src to dst
			// In our memory-slot model, this is equivalent to mstore(dst, mload(src))
			if (args.size() >= 2)
			{
				auto mloadArgs = std::vector<std::shared_ptr<awst::Expression>>{args[1]};
				auto loadedVal = handleMload(mloadArgs, loc);
				if (loadedVal)
				{
					auto storeArgs = std::vector<std::shared_ptr<awst::Expression>>{args[0], loadedVal};
					handleMstore(storeArgs, loc, _out);
				}
			}
			return;
		}

		// Check for user-defined assembly function call
		auto asmIt = m_asmFunctions.find(funcName);
		if (asmIt != m_asmFunctions.end())
		{
			handleUserFunctionCall(funcName, args, loc, _out);
			return;
		}

		// Other side-effecting calls: wrap as ExpressionStatement
		auto expr = translateExpression(_stmt.expression);
		if (expr)
		{
			auto exprStmt = std::make_shared<awst::ExpressionStatement>();
			exprStmt->sourceLocation = loc;
			exprStmt->expr = std::move(expr);
			_out.push_back(std::move(exprStmt));
		}
	}
	else
	{
		// Non-call expression statement
		auto expr = translateExpression(_stmt.expression);
		if (expr)
		{
			auto exprStmt = std::make_shared<awst::ExpressionStatement>();
			exprStmt->sourceLocation = loc;
			exprStmt->expr = std::move(expr);
			_out.push_back(std::move(exprStmt));
		}
	}
}

void AssemblyTranslator::translateFunctionDefinition(
	solidity::yul::FunctionDefinition const& _def,
	std::vector<std::shared_ptr<awst::Statement>>& /*_out*/
)
{
	// Function definitions are collected in the first pass and inlined at call sites.
	// Nothing to emit here.
	auto loc = makeLoc(_def.debugData);
	Logger::instance().debug(
		"assembly function '" + _def.name.str() + "' collected for inlining", loc
	);
}

// ─── Assembly function inlining ─────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyTranslator::handleUserFunctionCall(
	std::string const& _name,
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto it = m_asmFunctions.find(_name);
	if (it == m_asmFunctions.end())
	{
		Logger::instance().error("unknown assembly function: " + _name, _loc);
		return nullptr;
	}

	auto const& funcDef = *it->second;

	// Inline: bind parameters to arguments via assignment statements
	if (_args.size() != funcDef.parameters.size())
	{
		Logger::instance().error(
			"assembly function '" + _name + "' called with wrong number of arguments", _loc
		);
		return nullptr;
	}

	// Assign parameters and propagate constant values
	for (size_t i = 0; i < funcDef.parameters.size(); ++i)
	{
		std::string paramName = funcDef.parameters[i].name.str();
		// Use the argument's actual type (handles arrays passed to assembly functions)
		awst::WType const* paramType = _args[i]->wtype;
		m_locals[paramName] = paramType;

		// Propagate constant values from arguments
		auto constVal = resolveConstantOffset(_args[i]);
		if (constVal)
			m_localConstants[paramName] = *constVal;

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = paramName;
		target->wtype = paramType;

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = _args[i];
		_out.push_back(std::move(assign));
	}

	// Initialize return variables to zero
	for (auto const& retVar: funcDef.returnVariables)
	{
		std::string retName = retVar.name.str();
		m_locals[retName] = awst::WType::biguintType();

		auto target = std::make_shared<awst::VarExpression>();
		target->sourceLocation = _loc;
		target->name = retName;
		target->wtype = awst::WType::biguintType();

		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";

		auto assign = std::make_shared<awst::AssignmentStatement>();
		assign->sourceLocation = _loc;
		assign->target = std::move(target);
		assign->value = std::move(zero);
		_out.push_back(std::move(assign));
	}

	// Translate the function body inline
	for (auto const& stmt: funcDef.body.statements)
		translateStatement(stmt, _out);

	return nullptr;
}

} // namespace puyasol::builder
