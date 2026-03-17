#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <liblangutil/DebugData.h>

#include <sstream>

namespace puyasol::builder
{

AssemblyBuilder::AssemblyBuilder(
	TypeMapper& _typeMapper,
	std::string const& _sourceFile,
	std::string const& _contextName
)
	: m_typeMapper(_typeMapper), m_sourceFile(_sourceFile), m_contextName(_contextName)
{
}

// ─── Public entry point ─────────────────────────────────────────────────────

std::vector<std::shared_ptr<awst::Statement>> AssemblyBuilder::buildBlock(
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
		buildStatement(stmt, result);
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

void AssemblyBuilder::initializeMemoryMap(
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

int AssemblyBuilder::computeFlatElementCount(awst::WType const* _type)
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

int AssemblyBuilder::computeARC4ByteSize(awst::WType const* _type)
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

void AssemblyBuilder::initializeCalldataMap(
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

std::shared_ptr<awst::Expression> AssemblyBuilder::accessFlatElement(
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

std::optional<uint64_t> AssemblyBuilder::resolveConstantOffset(
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

std::optional<std::pair<std::string, uint64_t>> AssemblyBuilder::decomposeVarOffset(
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

awst::SourceLocation AssemblyBuilder::makeLoc(
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

std::shared_ptr<awst::Expression> AssemblyBuilder::ensureBiguint(
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

	// Non-scalar type (array, struct, tuple) used in assembly arithmetic.
	// This happens in EVM memory pointer operations like add(array, 0x20)
	// which have no meaning on AVM. Coerce to biguint(0).
	Logger::instance().warning(
		"non-scalar type '" + _expr->wtype->name()
		+ "' in assembly arithmetic, coercing to biguint(0)",
		_loc
	);
	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = _loc;
	zero->wtype = awst::WType::biguintType();
	zero->value = "0";
	return zero;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::ensureBool(
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

std::shared_ptr<awst::Expression> AssemblyBuilder::makeBigUIntBinOp(
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

std::shared_ptr<awst::Expression> AssemblyBuilder::makeTwoPow256(
	awst::SourceLocation const& _loc
)
{
	auto c = std::make_shared<awst::IntegerConstant>();
	c->sourceLocation = _loc;
	c->wtype = awst::WType::biguintType();
	c->value = "115792089237316195423570985008687907853269984665640564039457584007913129639936";
	return c;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::wrapMod256(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc
)
{
	return makeBigUIntBinOp(std::move(_expr), awst::BigUIntBinaryOperator::Mod, makeTwoPow256(_loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::safeDivMod(
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

std::shared_ptr<awst::Expression> AssemblyBuilder::safeBtoi(
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


} // namespace puyasol::builder
