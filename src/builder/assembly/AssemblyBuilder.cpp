#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <liblangutil/DebugData.h>
#include <libyul/backends/evm/EVMDialect.h>
#include <libevmasm/Instruction.h>

#include <sstream>

namespace puyasol::builder
{

std::string AssemblyBuilder::getFunctionName(solidity::yul::FunctionName const& _name)
{
	if (auto const* ident = std::get_if<solidity::yul::Identifier>(&_name))
		return ident->name.str();
	if (auto const* builtin = std::get_if<solidity::yul::BuiltinName>(&_name))
	{
		// Resolve builtin name through the EVM dialect
		auto const& dialect = solidity::yul::EVMDialect::strictAssemblyForEVMObjects(solidity::langutil::EVMVersion{}, std::nullopt);
		return std::string(dialect.builtin(builtin->handle).name);
	}
	return "<unknown>";
}

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
	std::map<std::string, std::string> const& _constants,
	std::map<std::string, unsigned> const& _paramBitWidths,
	std::map<std::string, std::string> const& _storageSlotVars
)
{
	m_returnType = _returnType;
	m_locals.clear();
	m_localConstants.clear();
	m_calldataMap.clear();
	m_asmFunctions.clear();
	m_upgradedLocals.clear();
	m_paramBitWidths = _paramBitWidths;
	m_constants = _constants;
	m_storageSlotVars = _storageSlotVars;
	m_arrayParamName.clear();
	m_arrayParamType = nullptr;
	m_arrayParamSize = 0;

	// Register function parameters as known locals (so they resolve with proper types)
	for (auto const& [name, type]: _params)
		m_locals[name] = type ? type : awst::WType::biguintType();

	initializeCalldataMap(_params);

	// Detect array parameter for blob initialization
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
			break;
		}
	}

	// First pass: collect assembly function definitions (recursively,
	// since Yul allows nested function definitions inside other functions)
	std::function<void(std::vector<solidity::yul::Statement> const&)> collectFunctions =
		[&](std::vector<solidity::yul::Statement> const& stmts)
	{
		for (auto const& stmt: stmts)
		{
			if (auto const* funcDef = std::get_if<solidity::yul::FunctionDefinition>(&stmt))
			{
				m_asmFunctions[funcDef->name.str()] = funcDef;
				collectFunctions(funcDef->body.statements);
			}
			else if (auto const* block = std::get_if<solidity::yul::Block>(&stmt))
			{
				collectFunctions(block->statements);
			}
		}
	};
	collectFunctions(_block.statements);

	// Second pass: translate statements (skipping function definitions)
	std::vector<std::shared_ptr<awst::Statement>> result;

	// Initialize memory blob: load from scratch slot 0, write params into it.
	// The blob is pre-allocated with bzero(SLOT_SIZE) in the approval program preamble.
	// Each assembly block loads it, works on it, and flushes it back.
	initializeMemoryBlob(_params, result);

	for (auto const& stmt: _block.statements)
	{
		if (std::holds_alternative<solidity::yul::FunctionDefinition>(stmt))
			continue; // Already collected in first pass
		buildStatement(stmt, result);
	}

	// Flush memory blob back to scratch at block end
	{
		awst::SourceLocation loc;
		loc.file = m_sourceFile;
		flushMemoryToScratch(loc, result);
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

		// If the Solidity type is narrower than 64 bits, mask to the correct width
		// before converting to uint64. E.g., uint16 a := 0x0f0f0f0f0f → mask to 0x0f0f.
		std::shared_ptr<awst::Expression> valueToCast = src;
		auto bwIt = m_paramBitWidths.find(name);
		if (bwIt != m_paramBitWidths.end() && bwIt->second < 64)
		{
			// mask = (1 << bitWidth) - 1
			solidity::u256 mask = (solidity::u256(1) << bwIt->second) - 1;
			std::ostringstream maskStr;
			maskStr << mask;

			auto maskConst = std::make_shared<awst::IntegerConstant>();
			maskConst->sourceLocation = loc;
			maskConst->wtype = awst::WType::biguintType();
			maskConst->value = maskStr.str();

			auto andOp = std::make_shared<awst::BigUIntBinaryOperation>();
			andOp->sourceLocation = loc;
			andOp->wtype = awst::WType::biguintType();
			andOp->left = std::move(valueToCast);
			andOp->op = awst::BigUIntBinaryOperator::BitAnd;
			andOp->right = std::move(maskConst);
			valueToCast = std::move(andOp);
		}

		// Convert to original type (uint64)
		auto converted = safeBtoi(std::move(valueToCast), loc);

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

// ─── Memory blob model ──────────────────────────────────────────────────────

std::vector<int> AssemblyBuilder::reservedScratchSlots()
{
	std::vector<int> slots;
	for (int i = MEMORY_SLOT_FIRST; i <= MEMORY_SLOT_LAST; ++i)
		slots.push_back(i);
	return slots;
}

void AssemblyBuilder::initializeMemoryBlob(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	awst::SourceLocation loc;
	loc.file = m_sourceFile;

	// Load the memory blob from scratch slot 0 into local variable __evm_memory.
	// The blob is pre-allocated in the approval program preamble with bzero(SLOT_SIZE).
	m_locals[MEMORY_VAR] = awst::WType::bytesType();
	{
		auto blob = loadMemoryBlob(loc, MEMORY_SLOT_FIRST);
		assignMemoryVar(std::move(blob), loc, _out);
	}

	// NOTE: We do NOT write the free memory pointer (FMP) here.
	// The FMP at offset 0x40 is initialized to 0x80 in the approval program preamble
	// (via the blob init in ContractBuilder). Subsequent assembly blocks must NOT
	// re-initialize it, because previous blocks may have advanced it.

	// Set __free_memory_ptr as a local constant for resolveConstantOffset.
	// This is the initial value; actual runtime value may differ after mstore(0x40,...).
	m_localConstants["__free_memory_ptr"] = 0x80;

	// Write array parameter elements into the blob at 0x80 + i*0x20
	if (!m_arrayParamName.empty() && m_arrayParamSize > 0)
	{
		for (int i = 0; i < m_arrayParamSize; ++i)
		{
			uint64_t offset = 0x80 + static_cast<uint64_t>(i) * 0x20;

			// Access param[i]
			auto base = std::make_shared<awst::VarExpression>();
			base->sourceLocation = loc;
			base->name = m_arrayParamName;
			base->wtype = m_arrayParamType;

			auto index = std::make_shared<awst::IntegerConstant>();
			index->sourceLocation = loc;
			index->wtype = awst::WType::uint64Type();
			index->value = std::to_string(i);

			auto indexExpr = std::make_shared<awst::IndexExpression>();
			indexExpr->sourceLocation = loc;
			indexExpr->wtype = awst::WType::biguintType();
			indexExpr->base = std::move(base);
			indexExpr->index = std::move(index);

			// Pad to 32 bytes and write into blob
			auto padded = padTo32Bytes(std::move(indexExpr), loc);

			auto offsetConst = std::make_shared<awst::IntegerConstant>();
			offsetConst->sourceLocation = loc;
			offsetConst->wtype = awst::WType::uint64Type();
			offsetConst->value = std::to_string(offset);

			auto replace = std::make_shared<awst::IntrinsicCall>();
			replace->sourceLocation = loc;
			replace->wtype = awst::WType::bytesType();
			replace->opCode = "replace3";
			replace->stackArgs.push_back(memoryVar(loc));
			replace->stackArgs.push_back(std::move(offsetConst));
			replace->stackArgs.push_back(std::move(padded));
			assignMemoryVar(std::move(replace), loc, _out);
		}
	}
}

std::shared_ptr<awst::Expression> AssemblyBuilder::memoryVar(awst::SourceLocation const& _loc)
{
	auto var = std::make_shared<awst::VarExpression>();
	var->sourceLocation = _loc;
	var->name = MEMORY_VAR;
	var->wtype = awst::WType::bytesType();
	return var;
}

void AssemblyBuilder::assignMemoryVar(
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto target = std::make_shared<awst::VarExpression>();
	target->sourceLocation = _loc;
	target->name = MEMORY_VAR;
	target->wtype = awst::WType::bytesType();

	auto assign = std::make_shared<awst::AssignmentStatement>();
	assign->sourceLocation = _loc;
	assign->target = std::move(target);
	assign->value = std::move(_value);
	_out.push_back(std::move(assign));
}

std::shared_ptr<awst::Expression> AssemblyBuilder::loadMemoryBlob(
	awst::SourceLocation const& _loc,
	int _slot
)
{
	auto loadOp = std::make_shared<awst::IntrinsicCall>();
	loadOp->sourceLocation = _loc;
	loadOp->wtype = awst::WType::bytesType();
	loadOp->opCode = "load";
	loadOp->immediates = {MEMORY_SLOT_FIRST + _slot};
	return loadOp;
}

void AssemblyBuilder::storeMemoryBlob(
	std::shared_ptr<awst::Expression> _blob,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	int _slot
)
{
	auto storeOp = std::make_shared<awst::IntrinsicCall>();
	storeOp->sourceLocation = _loc;
	storeOp->wtype = awst::WType::voidType();
	storeOp->opCode = "store";
	storeOp->immediates = {MEMORY_SLOT_FIRST + _slot};
	storeOp->stackArgs.push_back(std::move(_blob));

	auto exprStmt = std::make_shared<awst::ExpressionStatement>();
	exprStmt->sourceLocation = _loc;
	exprStmt->expr = std::move(storeOp);
	_out.push_back(std::move(exprStmt));
}

void AssemblyBuilder::flushMemoryToScratch(
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Store the local __evm_memory blob back to scratch slot 0
	storeMemoryBlob(memoryVar(_loc), _loc, _out, 0);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::offsetToUint64(
	std::shared_ptr<awst::Expression> _offset,
	awst::SourceLocation const& _loc
)
{
	if (_offset->wtype == awst::WType::uint64Type())
		return _offset;

	// biguint → bytes → btoi (safe for offsets that fit in uint64)
	return safeBtoi(ensureBiguint(std::move(_offset), _loc), _loc);
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
