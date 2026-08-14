#include "builder/sol-types/TypeCoercion.h"
#include "builder/SourceLocConvert.h"
#include "builder/CompilationSession.h"
#include "builder/BuildArtifacts.h"
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

std::string AssemblyBuilder::getFunctionName(
	solidity::yul::FunctionName const& _name) const
{
	if (auto const* ident = std::get_if<solidity::yul::Identifier>(&_name))
		return ident->name.str();
	if (auto const* builtin = std::get_if<solidity::yul::BuiltinName>(&_name))
	{
		// Use the same EVM version the Solidity front-end parsed with:
		// BuiltinHandle is dialect-specific; a different dialect silently resolves
		// to the wrong builtin or throws.
		using solidity::langutil::EVMVersion;
		EVMVersion const ver =
			m_typeMapper.profile().evmVersion.value_or(EVMVersion::cancun());
		try
		{
			auto const& dialect = solidity::yul::EVMDialect::strictAssemblyForEVMObjects(ver, std::nullopt);
			auto const& b = dialect.builtin(builtin->handle);
			return std::string(b.name);
		}
		catch (...)
		{
			// Fallback: try each dialect in version order. Last-resort when
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
	std::map<std::string, BoxKeyedSlot> const& _boxKeyedStructSlots,
	std::map<std::string, std::string> const& _blobOffsetVars,
	std::map<std::string, std::string> const& _structRefSlotLocals,
	std::map<std::string, StateVarSlot> const& _stateVarSlots,
	std::map<solidity::yul::Identifier const*,
		solidity::frontend::InlineAssemblyAnnotation::ExternalIdentifierInfo> const& _externalRefs,
	std::function<std::string(solidity::frontend::VariableDeclaration const&)> _declName,
	size_t _numCalldataParams
)
{
	m_returnType = _returnType;
	m_locals.clear();
	m_localConstants.clear();
	m_reassignedLocals.clear();
	collectReassignedLocals(_block);
	m_calldataParamNames.clear();
	m_calldataMap.clear();
	m_asmFunctions.clear();
	m_upgradedLocals.clear();
	m_paramBitWidths = _paramBitWidths;
	m_constants = _constants;
	m_storageSlotVars = _storageSlotVars;
	m_boxKeyedStructSlots = _boxKeyedStructSlots;
	m_blobOffsetVars = _blobOffsetVars;
	m_structRefSlotLocals = _structRefSlotLocals;
	m_stateVarSlots = _stateVarSlots;
	m_externalRefs = _externalRefs;
	m_declName = std::move(_declName);
	m_arrayParamName.clear();
	m_arrayParamType = nullptr;
	m_arrayParamSize = 0;
	m_signedShadow.clear();
	m_haltEmitted = false;

	for (auto const& [name, type]: _params)
		m_locals[name] = type ? type : awst::WType::biguintType();

	// The synthetic calldata blob + offset map model the EVM calldata buffer, which holds ONLY
	// the function's real input args — not the external refs / return vars SolInlineAssembly
	// appends to _params. Slice to the leading calldata params so the EVM-ABI head/tail layout
	// (and thus .offset/.length) is correct.
	size_t nCd = std::min(_numCalldataParams, _params.size());
	m_calldataParams.assign(_params.begin(), _params.begin() + nCd);

	initializeCalldataMap(m_calldataParams);

	// Enable synthetic-calldata blob if Yul accesses calldata at non-constant offsets / calldatasize
	// / a dynamic param's .offset|.length. Blob is emitted in the prelude below, after array-param init.
	m_useSyntheticCalldata = detectDynamicCalldataAccess(_block);

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

	// First pass: collect assembly function definitions (Yul allows nesting)
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

	// Build direct-call graph (user-defined Yul calls only; builtins irrelevant for recursion).
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
			if (std::get_if<solidity::yul::FunctionDefinition>(&s))
				continue; // declarations do not execute in the enclosing body
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
	// Functions are declarations, not entry points. Close the graph only from
	// calls made by the executable assembly body; unused recursive helpers must
	// not become speculative AWST roots.
	std::set<std::string> reachableYulFuncs;
	scanStmts(_block.statements, reachableYulFuncs);
	std::vector<std::string> reachableWorklist(
		reachableYulFuncs.begin(), reachableYulFuncs.end());
	for (size_t i = 0; i < reachableWorklist.size(); ++i)
	{
		auto const found = yulDirectCalls.find(reachableWorklist[i]);
		if (found == yulDirectCalls.end())
			continue;
		for (auto const& callee: found->second)
			if (reachableYulFuncs.insert(callee).second)
				reachableWorklist.push_back(callee);
	}
	// Mark each function recursive iff it reaches itself.
	for (auto const& [name, _]: m_asmFunctions)
	{
		if (!reachableYulFuncs.count(name))
			continue;
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
	for (auto const& name: m_recursiveYulFuncs)
	{
		std::string safeCtx = m_contextName;
		std::replace(safeCtx.begin(), safeCtx.end(), '.', '_');
		std::string subId = m_sourceFile + "." + m_contextName + "::__yul_" + name;
		std::string subName = "__yul_" + safeCtx + "_" + name;
		m_yulFuncSubroutineIds[name] = subId;
		buildRecursiveYulSubroutine(*m_asmFunctions.at(name), subId, subName);
	}

	// Second pass: translate statements (skip function definitions already collected)
	std::vector<std::shared_ptr<awst::Statement>> result;
	// Load scratch blob, write params into it; blob pre-allocated in preamble.
	initializeMemoryBlob(_params, result);

	// Signed intN (N<=64) locals: seed a biguint SHADOW with the sign-extended
	// 256-bit word (see m_signedShadow). Reads/writes in the block hit the shadow.
	for (auto const& [name, bits]: m_signedParamBits)
	{
		auto lit = m_locals.find(name);
		if (lit == m_locals.end() || lit->second != awst::WType::uint64Type())
			continue;
		awst::SourceLocation loc;
		loc.file = m_sourceFile;
		std::string shadow = "__asmsx_" + name;
		result.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(shadow, awst::WType::biguintType(), loc),
			TypeCoercion::signExtendToUint256(
				awst::makeVarExpression(name, awst::WType::uint64Type(), loc), bits, loc),
			loc));
		m_locals[shadow] = awst::WType::biguintType();
		m_signedShadow[name] = shadow;
	}

	for (auto const& stmt: _block.statements)
	{
		if (std::holds_alternative<solidity::yul::FunctionDefinition>(stmt))
			continue; // Already collected in first pass
		buildStatement(stmt, result);
	}

	// Drain any statements still pending after the last statement: a bare
	// expression-statement builtin whose handler queues its effect (e.g. a
	// trailing `calldatacopy(...)`) previously left it undrained here and the
	// memory write silently vanished.
	drainPendingStatements(result);

	// Flush blob at block end; skip when halt already emitted (trailing store = unreachable).
	if (!m_haltEmitted)
	{
		awst::SourceLocation loc;
		loc.file = m_sourceFile;
		flushMemoryToScratch(loc, result);
	}

	// Write the signed shadows' low 8 bytes back to their typed locals (the
	// 64-bit-TC view of the possibly-dirty word — EVM keeps asm dirt too).
	if (m_haltEmitted)
		m_signedShadow.clear();
	for (auto const& [name, shadow]: m_signedShadow)
	{
		awst::SourceLocation loc;
		loc.file = m_sourceFile;
		result.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(name, awst::WType::uint64Type(), loc),
			safeBtoi(awst::makeVarExpression(shadow, awst::WType::biguintType(), loc), loc),
			loc));
	}
	m_signedShadow.clear();

	// Coerce biguint-upgraded variables back to their original types at block end.
	if (m_haltEmitted)
		m_upgradedLocals.clear();
	for (auto const& [name, origType]: m_upgradedLocals)
	{
		awst::SourceLocation loc;
		loc.file = m_sourceFile;

		auto src = awst::makeVarExpression(name, awst::WType::biguintType(), loc);
		// For sub-64-bit Solidity types, mask to width before converting to uint64
		// (e.g. uint16 a := 0x0f0f0f0f0f → mask to 0x0f0f).
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

		auto converted = safeBtoi(std::move(valueToCast), loc);
		auto target = awst::makeVarExpression(name, origType, loc);
		result.push_back(awst::makeAssignmentStatement(std::move(target), std::move(converted), loc));
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
	// AVM.sol Scratch library slots (flash-accounting deltas) — reserved so puya never reuses them.
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

std::vector<std::shared_ptr<awst::Statement>> AssemblyBuilder::emitBytesBlobAlloc(
	std::shared_ptr<awst::Expression> _lenU64, std::string const& _offVar,
	int _uniqueId, awst::SourceLocation const& _loc)
{
	std::vector<std::shared_ptr<awst::Statement>> out;
	auto u64 = awst::WType::uint64Type();
	auto k = [&](char const* v) { return awst::makeIntegerConstant(v, _loc); };

	// Materialise the length once; buffer offset = current FMP (extractUInt64 @ 88).
	std::string lenVar = "__bytesalloc_len_" + std::to_string(_uniqueId);
	out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(lenVar, u64, _loc), std::move(_lenU64), _loc));
	auto lenRead = [&]() { return awst::makeVarExpression(lenVar, u64, _loc); };

	out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(_offVar, u64, _loc),
		awst::makeExtractUInt64(awst::makeLoadSlot(MEMORY_SLOT_FIRST, _loc), k("88"), _loc), _loc));
	auto offRead = [&]() { return awst::makeVarExpression(_offVar, u64, _loc); };

	// Write the 32-byte length word at the buffer offset: replace3(blob, off, pad32(len)).
	auto lenWord = awst::makeLeftPad(awst::makeItob(lenRead(), _loc), 24, _loc);
	out.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(MEMORY_SLOT_FIRST,
		awst::makeReplace3(awst::makeLoadSlot(MEMORY_SLOT_FIRST, _loc), offRead(),
			std::move(lenWord), _loc), _loc), _loc));

	// newFMP = off + 32 + ceil(len/32)*32.
	auto ceil32 = awst::makeUInt64BinOp(
		awst::makeUInt64BinOp(
			awst::makeUInt64BinOp(lenRead(), awst::UInt64BinaryOperator::Add, k("31"), _loc),
			awst::UInt64BinaryOperator::FloorDiv, k("32"), _loc),
		awst::UInt64BinaryOperator::Mult, k("32"), _loc);
	auto newFmp = awst::makeUInt64BinOp(
		awst::makeUInt64BinOp(offRead(), awst::UInt64BinaryOperator::Add, k("32"), _loc),
		awst::UInt64BinaryOperator::Add, std::move(ceil32), _loc);
	auto fmpWord = awst::makeLeftPad(awst::makeItob(std::move(newFmp), _loc), 24, _loc);
	out.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(MEMORY_SLOT_FIRST,
		awst::makeReplace3(awst::makeLoadSlot(MEMORY_SLOT_FIRST, _loc), k("64"),
			std::move(fmpWord), _loc), _loc), _loc));
	return out;
}

void AssemblyBuilder::initializeMemoryBlob(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	awst::SourceLocation loc;
	loc.file = m_sourceFile;

	// Slot 0 lives directly in scratch (no __evm_memory local cache).
	// MEMORY_VAR declared as vestigial; memoryVar()/assignMemoryVar() go straight to scratch slot 0.
	m_locals[MEMORY_VAR] = awst::WType::bytesType();
	{
		auto blob = loadMemoryBlob(loc, MEMORY_SLOT_FIRST);
		assignMemoryVar(std::move(blob), loc, _out);
	}

	// Do NOT re-initialize FMP: it's set to 0x80 in the approval preamble and
	// subsequent blocks must not reset it (previous blocks may have advanced it).
	// __free_memory_ptr is the initial value; mstore(0x40,...) may change it at runtime.
	m_localConstants["__free_memory_ptr"] = 0x80;

	// Build __cd_blob (selector + head + tail) for dynamic-offset calldataload/calldatasize,
	// then seed the mutable (__cd_off_x, __cd_len_x) pointer locals from it. The seeding MUST
	// be inside the guard: without the blob the seeds read an unassigned __cd_blob (was a
	// missing-braces bug, latent only because re-seeding made the bad seeds dead stores).
	if (m_useSyntheticCalldata)
	{
		buildSyntheticCalldataBlob(m_calldataParams, _out, loc);
		initCalldataPointerLocals(_out, loc);
	}

	// Write array param elements into blob at 0x80 + i*0x20
	if (!m_arrayParamName.empty() && m_arrayParamSize > 0)
	{
		for (int64_t i = 0; i < m_arrayParamSize; ++i)
		{
			uint64_t offset = 0x80 + static_cast<uint64_t>(i) * 0x20;

			// Access param[i]
			auto base = awst::makeVarExpression(m_arrayParamName, m_arrayParamType, loc);
			auto index = awst::makeIntegerConstant(i, loc);
			auto indexExpr = awst::makeIndexExpression(std::move(base), std::move(index), awst::WType::biguintType(), loc);
			auto padded = padTo32Bytes(std::move(indexExpr), loc);
			auto offsetConst = awst::makeIntegerConstant(offset, loc);
			auto replace = awst::makeReplace3(memoryVar(loc), std::move(offsetConst), std::move(padded), loc);
			assignMemoryVar(std::move(replace), loc, _out);
		}
	}
}

std::shared_ptr<awst::Expression> AssemblyBuilder::memoryVar(awst::SourceLocation const& _loc)
{
	// Read slot 0 straight from scratch — no __evm_memory local cache.
	// The cached form caused puya to miscount the dig in large split pieces,
	// storing uint64 into slot 0; direct scratch loads/stores avoid that.
	return awst::makeLoadSlot(MEMORY_SLOT_FIRST, _loc);
}

void AssemblyBuilder::assignMemoryVar(
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
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
	// No-op now that slot 0 lives directly in scratch; retained as a splitter sync hook.
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


void AssemblyBuilder::collectReassignedLocals(solidity::yul::Block const& _block)
{
	using namespace solidity::yul;
	std::function<void(Block const&)> walk = [&](Block const& blk)
	{
		for (auto const& s: blk.statements)
		{
			if (auto const* assign = std::get_if<Assignment>(&s))
				for (auto const& var: assign->variableNames)
					m_reassignedLocals.insert(var.name.str());
			else if (auto const* b = std::get_if<Block>(&s))
				walk(*b);
			else if (auto const* iff = std::get_if<If>(&s))
				walk(iff->body);
			else if (auto const* sw = std::get_if<Switch>(&s))
				for (auto const& c: sw->cases)
					walk(c.body);
			else if (auto const* fl = std::get_if<ForLoop>(&s))
			{
				walk(fl->pre);
				walk(fl->post);
				walk(fl->body);
			}
			else if (auto const* fn = std::get_if<FunctionDefinition>(&s))
				walk(fn->body);
		}
	};
	walk(_block);
}


void AssemblyBuilder::invalidateMemConstants()
{
	for (auto it = m_localConstants.begin(); it != m_localConstants.end();)
	{
		if (it->first.rfind("mem_0x", 0) == 0)
			it = m_localConstants.erase(it);
		else
			++it;
	}
	m_lastMstoreValue = nullptr;
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

	if (auto const* varExpr = dynamic_cast<awst::VarExpression const*>(_expr.get()))
	{
		// Calldata params: m_localConstants holds their calldata head offset (for `.offset`), not a
		// value — a bare param used as an offset must resolve to its runtime value (skip here).
		auto it = m_localConstants.find(varExpr->name);
		if (it != m_localConstants.end() && !m_calldataParamNames.count(varExpr->name))
			return it->second;
	}

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
		// Mod where right > uint64_t (e.g. 2^256): if left resolves, it's a no-op (left < right).
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
	if (_debugData)
		return m_typeMapper.sourceMap().toAwstLoc(
			m_sourceFile, _debugData->nativeLocation);
	awst::SourceLocation loc;
	loc.file = m_sourceFile;
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

	if (_expr->wtype == awst::WType::biguintType())
		return _expr;

	if (_expr->wtype == awst::WType::boolType())
	{
		auto one = awst::makeBiguintConstant("1", _loc);
		auto zero = awst::makeBiguintConstant("0", _loc);

		return awst::makeConditional(
			std::move(_expr), std::move(one), std::move(zero),
			awst::WType::biguintType(), _loc);
	}

	if (_expr->wtype == awst::WType::uint64Type())
	{
		auto itob = awst::makeItob(std::move(_expr), _loc);
		return awst::makeAsBiguint(std::move(itob), _loc);
	}

	if (_expr->wtype->kind() == awst::WTypeKind::Bytes)
		return awst::makeAsBiguint(std::move(_expr), _loc);

	// account → biguint: AVM addresses are 32 raw bytes; reinterpret via bytes.
	// Without this the fallback below would zero the value — silently miscompiling
	// every assembly read of an `address` param (e.g. Solady's
	// `or(newOwner, shl(255, iszero(newOwner)))` in `_setOwner` → owner() reads zero).
	if (_expr->wtype == awst::WType::accountType())
	{
		auto asBytes = awst::makeAsBytes(std::move(_expr), _loc);
		auto asBiguint = awst::makeAsBiguint(std::move(asBytes), _loc);
		return asBiguint;
	}

	// arc4.uintN: exactly N/8 big-endian bytes, no length prefix — raw bytes ARE the value.
	// In-expression integers are already biguint; this fires when an arc4 numeric leaks
	// from the ABI/storage boundary (was silently coerced to 0 by the old fallback).
	if (_expr->wtype->kind() == awst::WTypeKind::ARC4UIntN)
	{
		auto asBytes = awst::makeAsBytes(std::move(_expr), _loc);
		return awst::makeAsBiguint(std::move(asBytes), _loc);
	}

	// Non-scalar (array/struct/tuple/reference-array) has no integer value (e.g. EVM
	// pointer arithmetic like add(array, 0x20) is meaningless on AVM's slot model).
	// Hard-error instead of coercing to 0 silently; return placeholder so the build
	// surfaces further errors in the same run.
	Logger::instance().error(
		"cannot coerce non-scalar type '" + _expr->wtype->name()
		+ "' to biguint in assembly arithmetic — no integer value",
		_loc
	);
	return awst::makeBiguintConstant("0", _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::ensureBiguintSlotArg(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc
)
{
	if (!_expr || !_expr->wtype)
		return _expr;
	auto const* w = _expr->wtype;
	bool scalar = w == awst::WType::biguintType()
		|| w == awst::WType::boolType()
		|| w == awst::WType::uint64Type()
		|| w == awst::WType::accountType()
		|| w->kind() == awst::WTypeKind::Bytes
		|| w->kind() == awst::WTypeKind::ARC4UIntN;
	if (scalar)
		return ensureBiguint(std::move(_expr), _loc);
	// A non-scalar here means an UNMODELED `.slot` reference (e.g. a local
	// storage ref to a struct-member array: `uint256[] storage x = s.x;
	// sstore(x.slot, ...)`) that fell through every resolution path. The slot
	// value would be garbage at runtime — the write lands on an arbitrary slot.
	// Fail loudly rather than miscompile silently.
	Logger::instance().error(
		"unmodeled .slot reference (type '" + w->name()
		+ "') used as a storage slot — sload/sstore through this alias is not"
		" supported yet (storage refs to struct-member aggregates)",
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

	if (_expr->wtype == awst::WType::boolType())
		return _expr;

	// Yul: non-zero = true.
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

	// Yul `if value {}` admits any uint256, including fixed-size bytes (e.g. bytes32 EIP-712
	// hashes, Solady _ERC1967_IMPLEMENTATION_SLOT). Compare to zero buffer of matching length —
	// satisfies puya's bool-only IfElse validator.
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
	return awst::makeBigUIntBinOp(ensureBiguint(std::move(_left), _loc), _op, ensureBiguint(std::move(_right), _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::makeTwoPow256(
	awst::SourceLocation const& _loc
)
{
	return makePow256(_loc);
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
	// EVM div/mod by zero returns 0; AVM panics. Emit: right != 0 ? left op right : 0.
	// Materialize the divisor once — it appears in both the guard and the op;
	// shared-pointer reuse re-ran non-trivial expressions twice (makeEvalOnce =
	// OperandPlan primitive; skips SE on a constant/var divisor).
	auto right = awst::makeEvalOnce(ensureBiguint(std::move(_right), _loc), _loc);

	auto cond = awst::makeNumericCompare(
		right, awst::NumericComparison::Ne, awst::makeBiguintConstant("0", _loc), _loc);

	auto divExpr = makeBigUIntBinOp(std::move(_left), _op, right, _loc);

	return awst::makeConditional(
		std::move(cond), std::move(divExpr), awst::makeBiguintConstant("0", _loc),
		awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::safeBtoi(
	std::shared_ptr<awst::Expression> _biguintExpr,
	awst::SourceLocation const& _loc
)
{
	// Take low 8 bytes: handles biguint > 8 bytes (e.g. from b&/b|/b^ padding).
	return awst::makeBiguintToUInt64(std::move(_biguintExpr), _loc);
}

void AssemblyBuilder::buildRecursiveYulSubroutine(
	solidity::yul::FunctionDefinition const& _funcDef,
	std::string const& _subroutineId,
	std::string const& _subroutineName
)
{
	auto loc = makeLoc(_funcDef.debugData);

	// Save state so the outer block can resume after the subroutine is built.
	auto savedLocals = std::move(m_locals);
	auto savedConstants = std::move(m_localConstants);
	auto savedCalldataParamNames = std::move(m_calldataParamNames);
	auto savedUpgraded = std::move(m_upgradedLocals);
	auto savedParamBitWidths = m_paramBitWidths;
	auto savedSignedParamBits = m_signedParamBits;
	auto savedSignedShadow = std::move(m_signedShadow);
	auto savedPending = std::move(m_pendingStatements);
	auto savedHalt = m_haltEmitted;
	auto savedInlineDepth = m_inlineDepth;
	auto savedArrayParamName = m_arrayParamName;
	auto savedArrayParamType = m_arrayParamType;
	auto savedArrayParamSize = m_arrayParamSize;
	auto savedReturnType = m_returnType;

	m_locals.clear();
	m_localConstants.clear();
	m_calldataParamNames.clear();
	m_upgradedLocals.clear();
	m_pendingStatements.clear();
	m_haltEmitted = false;
	m_inlineDepth = 0;
	m_arrayParamName.clear();
	m_arrayParamType = nullptr;
	m_arrayParamSize = 0;
	m_returnType = awst::WType::biguintType();

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

	for (auto const& r: _funcDef.returnVariables)
		m_locals[r.name.str()] = awst::WType::biguintType();

	std::vector<std::shared_ptr<awst::Statement>> bodyStmts;
	// Init return vars to 0 (Yul default)
	for (auto const& r: _funcDef.returnVariables)
	{
		auto rLoc = makeLoc(r.debugData);
		auto target = awst::makeVarExpression(r.name.str(), awst::WType::biguintType(), rLoc);
		auto zero = awst::makeZero(rLoc, awst::WType::biguintType());
		auto init = awst::makeAssignmentStatement(std::move(target), std::move(zero), rLoc);
		bodyStmts.push_back(std::move(init));
	}

	for (auto const& stmt: _funcDef.body.statements)
		buildStatement(stmt, bodyStmts);

	awst::WType const* retType = awst::WType::voidType();
	size_t nRet = _funcDef.returnVariables.size();
	if (nRet == 1)
	{
		retType = awst::WType::biguintType();
		std::string retName = _funcDef.returnVariables[0].name.str();
		auto retVar = awst::makeVarExpression(retName, awst::WType::biguintType(), loc);
		bodyStmts.push_back(awst::makeReturnStatement(std::move(retVar), loc));
	}
	else if (nRet > 1)
	{
		std::vector<awst::WType const*> rts(nRet, awst::WType::biguintType());
		retType = m_typeMapper.createType<awst::WTuple>(std::move(rts));
		auto tupleExpr = awst::makeTupleExpression(retType, loc);
		for (auto const& r: _funcDef.returnVariables)
			tupleExpr->items.push_back(
				awst::makeVarExpression(r.name.str(), awst::WType::biguintType(), loc));
		bodyStmts.push_back(awst::makeReturnStatement(std::move(tupleExpr), loc));
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

	m_typeMapper.artifacts().pendingYulSubroutines.push_back(std::move(sub));

	m_locals = std::move(savedLocals);
	m_localConstants = std::move(savedConstants);
	m_calldataParamNames = std::move(savedCalldataParamNames);
	m_upgradedLocals = std::move(savedUpgraded);
	m_paramBitWidths = std::move(savedParamBitWidths);
	m_signedParamBits = std::move(savedSignedParamBits);
	m_signedShadow = std::move(savedSignedShadow);
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
