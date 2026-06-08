#include "builder/sol-types/TypeCoercion.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <liblangutil/DebugData.h>
#include <liblangutil/EVMVersion.h>
#include <libyul/backends/evm/EVMDialect.h>
#include <libevmasm/Instruction.h>

#include <algorithm>
#include <array>
#include <optional>
#include <sstream>

namespace puyasol::builder
{
namespace {
// Set once at startup in main.cpp via puyasol::builder::setCompileEVMVersion.
// getFunctionName uses this to resolve BuiltinHandle through the same dialect
// the Solidity front-end used to parse the source, so we don't guess and end
// up returning the wrong builtin name.
std::optional<solidity::langutil::EVMVersion>& globalEVMVersion()
{
	static std::optional<solidity::langutil::EVMVersion> v;
	return v;
}
}
void setCompileEVMVersion(solidity::langutil::EVMVersion _v)
{
	globalEVMVersion() = _v;
}
}

namespace puyasol::builder
{

std::string AssemblyBuilder::getFunctionName(solidity::yul::FunctionName const& _name)
{
	if (auto const* ident = std::get_if<solidity::yul::Identifier>(&_name))
		return ident->name.str();
	if (auto const* builtin = std::get_if<solidity::yul::BuiltinName>(&_name))
	{
		// Use the same EVM version the Solidity front-end parsed with —
		// BuiltinHandle is a dialect-specific index, so iterating over
		// dialects is wrong (the handle either throws in an unrelated
		// dialect or worse, silently resolves to a different builtin).
		using solidity::langutil::EVMVersion;
		EVMVersion const ver = globalEVMVersion().value_or(EVMVersion::cancun());
		try
		{
			auto const& dialect = solidity::yul::EVMDialect::strictAssemblyForEVMObjects(ver, std::nullopt);
			auto const& b = dialect.builtin(builtin->handle);
			return std::string(b.name);
		}
		catch (...)
		{
			// Fallback: try each dialect and pick the first that accepts
			// the handle. This is a last-resort for the rare case where
			// the global isn't set (e.g. unit-test paths).
			static const std::array<EVMVersion, 14> versions = {
				EVMVersion::osaka(),
				EVMVersion::prague(),
				EVMVersion::cancun(),
				EVMVersion::shanghai(),
				EVMVersion::paris(),
				EVMVersion::london(),
				EVMVersion::berlin(),
				EVMVersion::istanbul(),
				EVMVersion::petersburg(),
				EVMVersion::constantinople(),
				EVMVersion::byzantium(),
				EVMVersion::spuriousDragon(),
				EVMVersion::tangerineWhistle(),
				EVMVersion::homestead(),
			};
			for (auto const& v: versions)
			{
				try
				{
					auto const& dialect = solidity::yul::EVMDialect::strictAssemblyForEVMObjects(v, std::nullopt);
					auto const& b = dialect.builtin(builtin->handle);
					return std::string(b.name);
				}
				catch (...)
				{
					continue;
				}
			}
		}
		return "<unknown_builtin>";
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
	std::map<std::string, std::string> const& _storageSlotVars,
	std::map<std::string, BoxKeyedSlot> const& _boxKeyedStructSlots
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
	m_boxKeyedStructSlots = _boxKeyedStructSlots;
	m_arrayParamName.clear();
	m_arrayParamType = nullptr;
	m_arrayParamSize = 0;
	m_haltEmitted = false;

	// Register function parameters as known locals (so they resolve with proper types)
	for (auto const& [name, type]: _params)
		m_locals[name] = type ? type : awst::WType::biguintType();

	initializeCalldataMap(_params);

	// If the Yul block accesses calldata at non-constant offsets (or uses
	// calldatasize), enable the synthetic-calldata blob path. The blob is
	// emitted in the prelude statements below, after array-param init.
	m_useSyntheticCalldata = detectDynamicCalldataAccess(_block);
	m_calldataParams = _params;

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

	// Build direct-call graph between collected Yul functions (only calls to
	// other Yul-user-defined functions count — builtins are irrelevant for
	// recursion detection).
	m_recursiveYulFuncs.clear();
	m_yulFuncSubroutineIds.clear();
	std::map<std::string, std::set<std::string>> yulDirectCalls;
	std::function<void(solidity::yul::Expression const&, std::set<std::string>&)> scanExpr;
	std::function<void(std::vector<solidity::yul::Statement> const&, std::set<std::string>&)> scanStmts;
	scanExpr = [&](solidity::yul::Expression const& _expr, std::set<std::string>& _out)
	{
		if (auto const* call = std::get_if<solidity::yul::FunctionCall>(&_expr))
		{
			std::string n = getFunctionName(call->functionName);
			if (m_asmFunctions.count(n))
				_out.insert(n);
			for (auto const& a: call->arguments)
				scanExpr(a, _out);
		}
	};
	scanStmts = [&](std::vector<solidity::yul::Statement> const& stmts, std::set<std::string>& _out)
	{
		for (auto const& s: stmts)
		{
			if (auto const* fd = std::get_if<solidity::yul::FunctionDefinition>(&s))
				scanStmts(fd->body.statements, _out);
			else if (auto const* blk = std::get_if<solidity::yul::Block>(&s))
				scanStmts(blk->statements, _out);
			else if (auto const* iff = std::get_if<solidity::yul::If>(&s))
			{
				scanExpr(*iff->condition, _out);
				scanStmts(iff->body.statements, _out);
			}
			else if (auto const* sw = std::get_if<solidity::yul::Switch>(&s))
			{
				scanExpr(*sw->expression, _out);
				for (auto const& c: sw->cases)
					scanStmts(c.body.statements, _out);
			}
			else if (auto const* fl = std::get_if<solidity::yul::ForLoop>(&s))
			{
				scanStmts(fl->pre.statements, _out);
				scanExpr(*fl->condition, _out);
				scanStmts(fl->post.statements, _out);
				scanStmts(fl->body.statements, _out);
			}
			else if (auto const* vd = std::get_if<solidity::yul::VariableDeclaration>(&s))
			{
				if (vd->value) scanExpr(*vd->value, _out);
			}
			else if (auto const* as = std::get_if<solidity::yul::Assignment>(&s))
			{
				if (as->value) scanExpr(*as->value, _out);
			}
			else if (auto const* es = std::get_if<solidity::yul::ExpressionStatement>(&s))
			{
				scanExpr(es->expression, _out);
			}
		}
	};
	for (auto const& [name, def]: m_asmFunctions)
	{
		std::set<std::string> callees;
		scanStmts(def->body.statements, callees);
		yulDirectCalls[name] = std::move(callees);
	}
	// Reachable-from-self (each function) → recursive iff reaches itself.
	for (auto const& [name, _]: m_asmFunctions)
	{
		std::set<std::string> visited;
		std::function<bool(std::string const&)> reaches = [&](std::string const& n) -> bool
		{
			auto it = yulDirectCalls.find(n);
			if (it == yulDirectCalls.end()) return false;
			for (auto const& c: it->second)
			{
				if (c == name) return true;
				if (visited.insert(c).second)
					if (reaches(c)) return true;
			}
			return false;
		};
		if (reaches(name))
			m_recursiveYulFuncs.insert(name);
	}
	// Emit a Subroutine for each recursive Yul function.
	for (auto const& name: m_recursiveYulFuncs)
	{
		std::string safeCtx = m_contextName;
		std::replace(safeCtx.begin(), safeCtx.end(), '.', '_');
		std::string subId = m_sourceFile + "." + m_contextName + "::__yul_" + name;
		std::string subName = "__yul_" + safeCtx + "_" + name;
		m_yulFuncSubroutineIds[name] = subId;
		buildRecursiveYulSubroutine(*m_asmFunctions.at(name), subId, subName);
	}

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

	// Flush memory blob back to scratch at block end. Skip if the block
	// already emitted a `return` or `revert` halt intrinsic — any trailing
	// store would be flagged as unreachable code by puya.
	if (!m_haltEmitted)
	{
		awst::SourceLocation loc;
		loc.file = m_sourceFile;
		flushMemoryToScratch(loc, result);
	}

	// Coerce upgraded variables back to their original types at block end.
	// Within the assembly block, variables may be promoted to biguint for 256-bit
	// Yul semantics. After the block, Solidity code expects the original types.
	if (m_haltEmitted)
		m_upgradedLocals.clear();
	for (auto const& [name, origType]: m_upgradedLocals)
	{
		awst::SourceLocation loc;
		loc.file = m_sourceFile;

		// Read the biguint-typed variable
		auto src = awst::makeVarExpression(name, awst::WType::biguintType(), loc);

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

			auto maskConst = awst::makeIntegerConstant(maskStr.str(), loc, awst::WType::biguintType());

			auto andOp = awst::makeBigUIntBinOp(std::move(valueToCast), awst::BigUIntBinaryOperator::BitAnd, std::move(maskConst), loc);
			valueToCast = std::move(andOp);
		}

		// Convert to original type (uint64)
		auto converted = safeBtoi(std::move(valueToCast), loc);

		auto target = awst::makeVarExpression(name, origType, loc);

		auto assign = awst::makeAssignmentStatement(std::move(target), std::move(converted), loc);
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
	slots.push_back(TRANSIENT_SLOT);
	// AVM.sol `Scratch` library slots (flash-accounting deltas) — reserved so the
	// puya backend never reuses them for its own temps.
	for (int i = FLASH_SCRATCH_FIRST; i <= FLASH_SCRATCH_LAST; ++i)
		slots.push_back(i);
	return slots;
}

std::vector<std::shared_ptr<awst::Statement>> AssemblyBuilder::emitFreeMemoryBump(
	int _size, awst::SourceLocation const& _loc, int _uniqueId)
{
	std::vector<std::shared_ptr<awst::Statement>> out;
	if (_size <= 0)
		return out;

	std::string blobTmp = "__fmp_blob_" + std::to_string(_uniqueId);

	auto loadOp = awst::makeLoadSlot(MEMORY_SLOT_FIRST, _loc);
	auto blobTarget = awst::makeVarExpression(blobTmp, awst::WType::bytesType(), _loc);
	out.push_back(awst::makeAssignmentStatement(blobTarget, std::move(loadOp), _loc));

	auto blobRead = awst::makeVarExpression(blobTmp, awst::WType::bytesType(), _loc);
	auto offset58 = awst::makeIntegerConstant("88", _loc);
	auto extractFmp = awst::makeExtractUInt64(
		std::move(blobRead), std::move(offset58), _loc);

	auto sizeConst = awst::makeIntegerConstant(_size, _loc);
	auto newFmp = awst::makeUInt64BinOp(
		std::move(extractFmp), awst::UInt64BinaryOperator::Add,
		std::move(sizeConst), _loc);

	auto itobNew = awst::makeItob(std::move(newFmp), _loc);
	auto concat = awst::makeLeftPad(std::move(itobNew), 24, _loc);

	auto blobRead2 = awst::makeVarExpression(blobTmp, awst::WType::bytesType(), _loc);
	auto offset40 = awst::makeIntegerConstant("64", _loc);
	auto replaceCall = awst::makeReplace3(std::move(blobRead2), std::move(offset40), std::move(concat), _loc);
	auto storeOp = awst::makeStoreSlot(MEMORY_SLOT_FIRST, std::move(replaceCall), _loc);

	out.push_back(awst::makeExpressionStatement(std::move(storeOp), _loc));
	return out;
}

void AssemblyBuilder::initializeMemoryBlob(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	awst::SourceLocation loc;
	loc.file = m_sourceFile;

	// Slot 0 lives directly in scratch (pre-allocated bzero(SLOT_SIZE) in the
	// approval-program preamble); there is no __evm_memory local cache anymore, so
	// this re-stores slot 0 from itself (no-op). MEMORY_VAR stays declared but
	// vestigial — memoryVar()/assignMemoryVar() go straight to scratch slot 0.
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

	// Synthesize the EVM-ABI calldata blob if the Yul accesses calldata
	// at a dynamic offset (or uses calldatasize). Builds a `__cd_blob`
	// local that mirrors Solidity's calldata layout
	// (selector + head section + tail section), letting any
	// calldataload(off) translate to extract3(__cd_blob, off, 32).
	if (m_useSyntheticCalldata)
		buildSyntheticCalldataBlob(_params, _out, loc);

	// Write array parameter elements into the blob at 0x80 + i*0x20
	if (!m_arrayParamName.empty() && m_arrayParamSize > 0)
	{
		for (int64_t i = 0; i < m_arrayParamSize; ++i)
		{
			uint64_t offset = 0x80 + static_cast<uint64_t>(i) * 0x20;

			// Access param[i]
			auto base = awst::makeVarExpression(m_arrayParamName, m_arrayParamType, loc);

			auto index = awst::makeIntegerConstant(i, loc);

			auto indexExpr = awst::makeIndexExpression(std::move(base), std::move(index), awst::WType::biguintType(), loc);

			// Pad to 32 bytes and write into blob
			auto padded = padTo32Bytes(std::move(indexExpr), loc);

			auto offsetConst = awst::makeIntegerConstant(offset, loc);

			auto replace = awst::makeReplace3(memoryVar(loc), std::move(offsetConst), std::move(padded), loc);
			assignMemoryVar(std::move(replace), loc, _out);
		}
	}
}

std::shared_ptr<awst::Expression> AssemblyBuilder::memoryVar(awst::SourceLocation const& _loc)
{
	// Slot 0 is read straight from scratch (no local cache). The local cache made
	// __evm_memory a long-lived L-stack value; in very large split pieces puya
	// miscounted its dig at the flush (storing uint64 to slot 0). Going through
	// scratch loads/stores removes that whole bug class.
	return awst::makeLoadSlot(MEMORY_SLOT_FIRST, _loc);
}

void AssemblyBuilder::assignMemoryVar(
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Write slot 0 straight to scratch (no local cache). See memoryVar().
	_out.push_back(awst::makeExpressionStatement(
		awst::makeStoreSlot(MEMORY_SLOT_FIRST, std::move(_value), _loc), _loc));
}

std::shared_ptr<awst::Expression> AssemblyBuilder::loadMemoryBlob(
	awst::SourceLocation const& _loc,
	int _slot
)
{
	return awst::makeLoadSlot(MEMORY_SLOT_FIRST + _slot, _loc);
}

void AssemblyBuilder::storeMemoryBlob(
	std::shared_ptr<awst::Expression> _blob,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	int _slot
)
{
	auto storeOp = awst::makeStoreSlot(MEMORY_SLOT_FIRST + _slot, std::move(_blob), _loc);
	auto exprStmt = awst::makeExpressionStatement(std::move(storeOp), _loc);
	_out.push_back(std::move(exprStmt));
}

void AssemblyBuilder::flushMemoryToScratch(
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Re-store slot 0 from itself — a no-op now that slot 0 lives directly in
	// scratch (no __evm_memory local cache). Retained as a splitter sync hook.
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
		auto one = awst::makeBiguintConstant("1", _loc);

		auto zero = awst::makeBiguintConstant("0", _loc);

		return awst::makeConditional(
			std::move(_expr), std::move(one), std::move(zero),
			awst::WType::biguintType(), _loc);
	}

	if (_expr->wtype == awst::WType::uint64Type())
	{
		// uint64 → biguint: itob(expr) then ReinterpretCast bytes→biguint
		auto itob = awst::makeItob(std::move(_expr), _loc);
		return awst::makeAsBiguint(std::move(itob), _loc);
	}

	if (_expr->wtype->kind() == awst::WTypeKind::Bytes)
	{
		// bytes → biguint: ReinterpretCast
		auto cast = awst::makeAsBiguint(std::move(_expr), _loc);
		return cast;
	}

	// account → biguint: AVM addresses are 32 raw bytes — reinterpret through
	// `bytes` (biguint is variable-length-bytes-backed). Without this the
	// non-scalar fallback below would zero the value, silently miscompiling
	// every assembly read of an `address` parameter (e.g. Solady's
	// `or(newOwner, shl(255, iszero(newOwner)))` in `_setOwner`, which makes
	// `owner()` read back zero across every Solady-Ownable-derived contract).
	if (_expr->wtype == awst::WType::accountType())
	{
		auto asBytes = awst::makeAsBytes(std::move(_expr), _loc);
		auto asBiguint = awst::makeAsBiguint(std::move(asBytes), _loc);
		return asBiguint;
	}

	// arc4.uintN (e.g. arc4.uint256) — the ABI/storage encoding of an integer:
	// exactly N/8 big-endian bytes, no length prefix, so its raw bytes ARE the
	// value. Reinterpret straight through bytes → biguint (value-preserving).
	// In-expression integers are already biguint (TypeMapper maps >64-bit ints to
	// biguint), so this only fires if an arc4 numeric leaks in from the ABI/storage
	// boundary — previously such a value was silently coerced to 0 by the fallback.
	if (_expr->wtype->kind() == awst::WTypeKind::ARC4UIntN)
	{
		auto asBytes = awst::makeAsBytes(std::move(_expr), _loc);
		return awst::makeAsBiguint(std::move(asBytes), _loc);
	}

	// Anything left — array / struct / tuple / reference-array — has no integer
	// value (e.g. EVM memory-pointer arithmetic like add(array, 0x20), meaningless
	// on the AVM's slot-backed memory model). Hard-error rather than silently
	// coercing to 0, which would miscompile the value with no signal. (Return a
	// placeholder so the rest of the program still compiles and surfaces any
	// further errors in the same run; the logged error fails the build.)
	Logger::instance().error(
		"cannot coerce non-scalar type '" + _expr->wtype->name()
		+ "' to biguint in assembly arithmetic — no integer value",
		_loc
	);
	return awst::makeBiguintConstant("0", _loc);
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
		auto zero = awst::makeBiguintConstant("0", _loc);

		auto cmp = awst::makeNumericCompare(std::move(_expr), awst::NumericComparison::Ne, std::move(zero), _loc);
		return cmp;
	}

	if (_expr->wtype == awst::WType::uint64Type())
	{
		auto zero = awst::makeZero(_loc);

		auto cmp = awst::makeNumericCompare(std::move(_expr), awst::NumericComparison::Ne, std::move(zero), _loc);
		return cmp;
	}

	// Yul `if value {}` admits any uint256, including stack words that we
	// type as fixed-size bytes (e.g., bytes32 EIP-712 hashes, Solady
	// _ERC1967_IMPLEMENTATION_SLOT). Compare to a zero buffer of matching
	// length — same semantics, satisfies puya's bool-only IfElse validator.
	if (_expr->wtype && _expr->wtype->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bw = dynamic_cast<awst::BytesWType const*>(_expr->wtype);
		size_t len = (bw && bw->length()) ? static_cast<size_t>(*bw->length()) : 32u;
		auto zeros = awst::makeBytesConstant(
			std::vector<uint8_t>(len, 0), _loc,
			awst::BytesEncoding::Base16, _expr->wtype);
		auto cmp = awst::makeBytesComparison(std::move(_expr), awst::EqualityComparison::Ne, std::move(zeros), _loc);
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
	auto node = awst::makeBigUIntBinOp(ensureBiguint(std::move(_left), _loc), _op, ensureBiguint(std::move(_right), _loc), _loc);
	return node;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::makeTwoPow256(
	awst::SourceLocation const& _loc
)
{
	auto c = makePow256(_loc);
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
	auto zero = awst::makeBiguintConstant("0", _loc);

	auto zeroForCmp = awst::makeBiguintConstant("0", _loc);

	auto cond = awst::makeNumericCompare(ensureBiguint(_right, _loc), awst::NumericComparison::Ne, std::move(zeroForCmp), _loc);

	auto divExpr = makeBigUIntBinOp(_left, _op, _right, _loc);

	return awst::makeConditional(
		std::move(cond), std::move(divExpr), std::move(zero),
		awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::safeBtoi(
	std::shared_ptr<awst::Expression> _biguintExpr,
	awst::SourceLocation const& _loc
)
{
	// Safe btoi: take the low 8 bytes — handles biguint values > 8 bytes
	// (e.g. from b&/b|/b^ padding).
	return awst::makeBiguintToUInt64(std::move(_biguintExpr), _loc);
}

// ─── Recursive Yul function subroutine sink ─────────────────────────────────

namespace {
std::vector<std::shared_ptr<awst::Subroutine>>& pendingSubroutinesRef()
{
	static std::vector<std::shared_ptr<awst::Subroutine>> s;
	return s;
}
}

std::vector<std::shared_ptr<awst::Subroutine>> AssemblyBuilder::takePendingSubroutines()
{
	return std::move(pendingSubroutinesRef());
}

void AssemblyBuilder::resetPendingSubroutines()
{
	pendingSubroutinesRef().clear();
}

void AssemblyBuilder::buildRecursiveYulSubroutine(
	solidity::yul::FunctionDefinition const& _funcDef,
	std::string const& _subroutineId,
	std::string const& _subroutineName
)
{
	auto loc = makeLoc(_funcDef.debugData);

	if (_funcDef.returnVariables.size() > 1)
	{
		Logger::instance().error(
			"recursive Yul function '" + _funcDef.name.str()
			+ "' with multiple return variables is not yet supported",
			loc);
		return;
	}

	// Save state that translateStatement touches so the outer block can resume.
	auto savedLocals = std::move(m_locals);
	auto savedConstants = std::move(m_localConstants);
	auto savedUpgraded = std::move(m_upgradedLocals);
	auto savedParamBitWidths = m_paramBitWidths;
	auto savedPending = std::move(m_pendingStatements);
	auto savedHalt = m_haltEmitted;
	auto savedInlineDepth = m_inlineDepth;
	auto savedArrayParamName = m_arrayParamName;
	auto savedArrayParamType = m_arrayParamType;
	auto savedArrayParamSize = m_arrayParamSize;
	auto savedReturnType = m_returnType;

	m_locals.clear();
	m_localConstants.clear();
	m_upgradedLocals.clear();
	m_pendingStatements.clear();
	m_haltEmitted = false;
	m_inlineDepth = 0;
	m_arrayParamName.clear();
	m_arrayParamType = nullptr;
	m_arrayParamSize = 0;
	m_returnType = awst::WType::biguintType();

	// Params
	std::vector<awst::SubroutineArgument> subArgs;
	for (auto const& p: _funcDef.parameters)
	{
		std::string pName = p.name.str();
		m_locals[pName] = awst::WType::biguintType();
		awst::SubroutineArgument arg;
		arg.name = pName;
		arg.wtype = awst::WType::biguintType();
		arg.sourceLocation = makeLoc(p.debugData);
		subArgs.push_back(std::move(arg));
	}

	// Return variables as locals
	for (auto const& r: _funcDef.returnVariables)
		m_locals[r.name.str()] = awst::WType::biguintType();

	std::vector<std::shared_ptr<awst::Statement>> bodyStmts;

	// Init return vars to 0 (Yul semantics)
	for (auto const& r: _funcDef.returnVariables)
	{
		auto rLoc = makeLoc(r.debugData);
		auto target = awst::makeVarExpression(r.name.str(), awst::WType::biguintType(), rLoc);
		auto zero = awst::makeZero(rLoc, awst::WType::biguintType());
		auto init = awst::makeAssignmentStatement(std::move(target), std::move(zero), rLoc);
		bodyStmts.push_back(std::move(init));
	}

	// Translate body
	for (auto const& stmt: _funcDef.body.statements)
		buildStatement(stmt, bodyStmts);

	// Final return
	awst::WType const* retType = awst::WType::voidType();
	if (_funcDef.returnVariables.size() == 1)
	{
		retType = awst::WType::biguintType();
		std::string retName = _funcDef.returnVariables[0].name.str();
		auto retVar = awst::makeVarExpression(retName, awst::WType::biguintType(), loc);
		bodyStmts.push_back(awst::makeReturnStatement(std::move(retVar), loc));
	}
	else
	{
		bodyStmts.push_back(awst::makeReturnStatement(nullptr, loc));
	}

	auto block = awst::makeBlock(loc);
	block->body = std::move(bodyStmts);

	auto sub = awst::makeSubroutine(
		_subroutineId, _subroutineName, std::move(subArgs),
		retType, std::move(block), /*pure=*/false, loc);

	pendingSubroutinesRef().push_back(std::move(sub));

	// Restore outer state
	m_locals = std::move(savedLocals);
	m_localConstants = std::move(savedConstants);
	m_upgradedLocals = std::move(savedUpgraded);
	m_paramBitWidths = std::move(savedParamBitWidths);
	m_pendingStatements = std::move(savedPending);
	m_haltEmitted = savedHalt;
	m_inlineDepth = savedInlineDepth;
	m_arrayParamName = std::move(savedArrayParamName);
	m_arrayParamType = savedArrayParamType;
	m_arrayParamSize = savedArrayParamSize;
	m_returnType = savedReturnType;
}

// ─── Expression translation ─────────────────────────────────────────────────


} // namespace puyasol::builder
