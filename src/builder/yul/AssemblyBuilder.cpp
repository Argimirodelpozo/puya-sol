#include "builder/types/TypeCoercion.h"
#include "builder/solc/SourceLocConvert.h"
#include "builder/context/CompilationSession.h"
#include "builder/context/BuildArtifacts.h"
#include "builder/solc/SolcFacts.h"
#include "builder/solc/PreparedAssembly.h"
#include "builder/yul/AssemblyBuilder.h"
#include "Logger.h"

#include <liblangutil/DebugData.h>
#include <liblangutil/EVMVersion.h>
#include <libyul/backends/evm/EVMDialect.h>
#include <libevmasm/Instruction.h>

#include <algorithm>
#include <array>
#include <optional>
#include <limits>
#include <set>
#include <functional>
#include <sstream>
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>

namespace puyasol::builder
{

std::string AssemblyBuilder::getFunctionName(
	solidity::yul::FunctionName const& _name) const
{
	if (auto const* ident = std::get_if<solidity::yul::Identifier>(&_name))
		return ident->name.str();
	if (auto const* builtin = std::get_if<solidity::yul::BuiltinName>(&_name))
	{
		if (!m_context->dialect)
			throw std::logic_error("Yul builtin has no prepared solc dialect");
		return std::string(m_context->dialect->builtin(builtin->handle).name);
	}
	return "<unknown>";
}

AssemblyBuilder::AssemblyBuilder(
	TypeMapper& _typeMapper,
	std::string const& _sourceFile,
	std::string const& _contextName,
	bool _inConstructor
)
	: m_typeMapper(_typeMapper), m_preparingContext(std::make_shared<Context>()),
	  m_context(m_preparingContext)
{
	m_preparingContext->sourceFile = _sourceFile;
	m_preparingContext->contextName = _contextName;
	m_preparingContext->inConstructor = _inConstructor;
}

// ─── Public entry point ─────────────────────────────────────────────────────

std::vector<std::shared_ptr<awst::Statement>> AssemblyBuilder::buildBlock(
	PreparedAssembly const& _assembly,
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	awst::WType const* _returnType,
	std::map<std::string, std::string> const& _constants,
	std::map<std::string, unsigned> const& _paramBitWidths,
	std::map<std::string, std::string> const& _storageSlotVars,
	std::map<std::string, BoxKeyedSlot> const& _boxKeyedStructSlots,
	std::map<std::string, std::string> const& _blobOffsetVars,
	std::map<std::string, std::string> const& _structRefSlotLocals,
	std::map<std::string, StateVarSlot> const& _stateVarSlots,
	std::function<std::string(solidity::frontend::VariableDeclaration const&)> _declName,
	size_t _numCalldataParams
)
{
	auto& context = prepareContext();
	auto const& _block = _assembly.block;
	context.dialect = _assembly.dialect;
	m_frame.returnType = _returnType;
	m_frame.locals.clear();
	m_frame.localConstants.clear();
	m_frame.localWideConstants.clear();
	context.yulConstantValues.clear();
	m_frame.alignedLocals.clear();
	m_frame.localSlotConstants.clear();
	context.reassignedLocals.clear();
	auto const& yulFacts = _assembly.facts;
	context.reassignedLocals = yulFacts.assignedVariables;
	context.yulConstantValues = yulFacts.constantValues;
	m_frame.calldataParamNames.clear();
	m_frame.calldataMap.clear();
	context.asmFunctions.clear();
	context.paramBitWidths = _paramBitWidths;
	context.constants = _constants;
	auto argumentFacts = SolcFacts::yulArgumentFacts(_assembly, _constants);
	context.yulConstantValues.insert(argumentFacts.constants.begin(), argumentFacts.constants.end());
	context.yulArgumentAlignments = std::move(argumentFacts.residuesMod32);
	context.storageSlotVars = _storageSlotVars;
	context.boxKeyedStructSlots = _boxKeyedStructSlots;
	m_frame.blobOffsetVars = _blobOffsetVars;
	context.structRefSlotLocals = _structRefSlotLocals;
	context.stateVarSlots = _stateVarSlots;
	context.externalRefs = _assembly.externalReferences;
	context.declName = std::move(_declName);
	m_frame.haltEmitted = false;

	for (auto const& [name, type]: _params)
		m_frame.locals[name] = type ? type : awst::WType::biguintType();

	// The synthetic calldata blob + offset map model the EVM calldata buffer, which holds ONLY
	// the function's real input args — not the external refs / return vars SolInlineAssembly
	// appends to _params. Slice to the leading calldata params so the EVM-ABI head/tail layout
	// (and thus .offset/.length) is correct.
	size_t nCd = std::min(_numCalldataParams, _params.size());
	context.calldataParams.assign(_params.begin(), _params.begin() + nCd);

	initializeCalldataMap(m_context->calldataParams);

	// Enable synthetic-calldata blob if Yul accesses calldata at non-constant offsets / calldatasize
	// / a dynamic param's .offset|.length. Blob is emitted in the prelude below, after array-param init.
	m_frame.useSyntheticCalldata = m_frame.functionCalldata || detectDynamicCalldataAccess(_block)
		|| !yulFacts.calldataFunctions.empty();

	// solc owns Yul function discovery, reachability, and recursion semantics.
	context.asmFunctions = yulFacts.functions;
	context.yulFuncSubroutineIds.clear();
	context.yulCalldataFunctions = yulFacts.calldataFunctions;
	context.yulMemoryWritingFunctions = yulFacts.memoryWritingFunctions;
	for (auto const& name: yulFacts.reachableFunctions)
	{
		if (yulFacts.terminatingFunctions.count(name))
		{
			if (yulFacts.recursiveFunctions.count(name))
				Logger::instance().error(
					"recursive Yul function with EVM return/stop requires a program-exit calling convention",
					makeLoc(m_context->asmFunctions.at(name)->debugData));
			continue;
		}
		context.yulFuncSubroutineIds[name] = m_context->sourceFile + "." + m_context->contextName + "::__yul_" + name;
	}
	m_preparingContext.reset(); // publish immutable context before outlining children
	// Register every ID before lowering any body, including mutually recursive
	// and forward calls. Definition/map order must not affect dispatch.
	for (auto const& [name, subId]: m_context->yulFuncSubroutineIds)
	{
		std::string safeCtx = m_context->contextName;
		std::replace(safeCtx.begin(), safeCtx.end(), '.', '_');
		std::string subName = "__yul_" + safeCtx + "_" + name;
		buildYulSubroutine(*m_context->asmFunctions.at(name), subId, subName);
	}

	// Second pass: translate statements (skip function definitions already collected)
	std::vector<std::shared_ptr<awst::Statement>> result;
	// Load scratch blob, write params into it; blob pre-allocated in preamble.
	initializeMemoryBlob(_params, result);

	for (auto const& [name, word]: m_frame.wordShadow)
		m_frame.locals[word] = awst::WType::biguintType();

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

	return result;
}

// ─── Memory blob model ──────────────────────────────────────────────────────

std::vector<std::shared_ptr<awst::Statement>> AssemblyBuilder::emitFreeMemoryBump(
	ScratchLayout const& _scratch,
	int _size, awst::SourceLocation const& _loc, int _uniqueId)
{
	std::vector<std::shared_ptr<awst::Statement>> out;
	if (_size <= 0)
		return out;

	std::string blobTmp = "__fmp_blob_" + std::to_string(_uniqueId);

	auto loadOp = awst::makeLoadSlot(_scratch.memoryFirst(), _loc);
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
	auto storeOp = awst::makeStoreSlot(_scratch.memoryFirst(), std::move(replaceCall), _loc);

	out.push_back(awst::makeExpressionStatement(std::move(storeOp), _loc));
	return out;
}

std::vector<std::shared_ptr<awst::Statement>> AssemblyBuilder::emitMemoryAlloc(
	ScratchLayout const& _scratch,
	std::shared_ptr<awst::Expression> _sizeU64,
	std::string const& _offVar,
	int _uniqueId, awst::SourceLocation const& _loc)
{
	std::vector<std::shared_ptr<awst::Statement>> out;
	auto const* u64Type = awst::WType::uint64Type();
	auto constant = [&](uint64_t value) {
		return awst::makeIntegerConstant(value, _loc);
	};
	std::string sizeName = "__memalloc_size_" + std::to_string(_uniqueId);
	auto size = [&]() {
		return awst::makeVarExpression(sizeName, u64Type, _loc);
	};
	auto offset = [&]() {
		return awst::makeVarExpression(_offVar, u64Type, _loc);
	};
	out.push_back(awst::makeAssignmentStatement(size(), std::move(_sizeU64), _loc));
	out.push_back(awst::makeAssignmentStatement(
		offset(),
		awst::makeExtractUInt64(awst::makeLoadSlot(_scratch.memoryFirst(), _loc),
			constant(88), _loc), _loc));

	// Solidity keeps the FMP word-aligned even when the logical payload is not.
	auto rounded = awst::makeUInt64BinOp(
		awst::makeUInt64BinOp(
			awst::makeUInt64BinOp(size(), awst::UInt64BinaryOperator::Add,
				constant(31), _loc),
			awst::UInt64BinaryOperator::FloorDiv, constant(32), _loc),
		awst::UInt64BinaryOperator::Mult, constant(32), _loc);
	auto next = awst::makeUInt64BinOp(
		offset(), awst::UInt64BinaryOperator::Add, std::move(rounded), _loc);
	auto nextWord = awst::makeLeftPad(awst::makeItob(std::move(next), _loc), 24, _loc);
	out.push_back(awst::makeExpressionStatement(
		awst::makeStoreSlot(_scratch.memoryFirst(),
			awst::makeReplace3(awst::makeLoadSlot(_scratch.memoryFirst(), _loc),
				constant(64), std::move(nextWord), _loc), _loc), _loc));
	return out;
}

std::vector<std::shared_ptr<awst::Statement>> AssemblyBuilder::emitBytesBlobAlloc(
	TypeMapper& _typeMapper,
	std::shared_ptr<awst::Expression> _lenU64, std::string const& _offVar,
	int _uniqueId, awst::SourceLocation const& _loc)
{
	auto const& _scratch = _typeMapper.profile().scratchLayout;
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
		awst::makeExtractUInt64(awst::makeLoadSlot(_scratch.memoryFirst(), _loc), k("88"), _loc), _loc));
	auto offRead = [&]() { return awst::makeVarExpression(_offVar, u64, _loc); };

	// Write the 32-byte length word at the buffer offset. Slot-routed: the
	// offset IS the FMP, which passes 4096 in any --evm-memory-slots contract
	// — the old slot-0 replace3 wrote the length into the wrong slot (or
	// panicked) for every buffer allocated past the first slot.
	auto lenWord = awst::makeLeftPad(awst::makeItob(lenRead(), _loc), 24, _loc);
	writeMemWordDirect(_typeMapper, offRead(), std::move(lenWord), _loc, out);

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
	out.push_back(awst::makeExpressionStatement(awst::makeStoreSlot(_scratch.memoryFirst(),
		awst::makeReplace3(awst::makeLoadSlot(_scratch.memoryFirst(), _loc), k("64"),
			std::move(fmpWord), _loc), _loc), _loc));
	return out;
}

void AssemblyBuilder::initializeMemoryBlob(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	awst::SourceLocation loc(m_context->sourceFile);

	// Memory is initialized once by the approval preamble; block entry and
	// exit do not reload or flush any local cache.
	// Do NOT re-initialize FMP: it's set to 0x80 in the approval preamble and
	// subsequent blocks must not reset it (previous blocks may have advanced it).
	// __free_memory_ptr is the initial value; mstore(0x40,...) may change it at runtime.
	m_frame.localConstants["__free_memory_ptr"] = 0x80;

	// Build __cd_blob (selector + head + tail) for dynamic-offset calldataload/calldatasize,
	// then seed the mutable (__cd_off_x, __cd_len_x) pointer locals from it. The seeding MUST
	// be inside the guard: without the blob the seeds read an unassigned __cd_blob (was a
	// missing-braces bug, latent only because re-seeding made the bad seeds dead stores).
	if (m_frame.useSyntheticCalldata && !m_frame.functionCalldata)
	{
		buildSyntheticCalldataBlob(m_context->calldataParams, _out, loc);
		initCalldataPointerLocals(_out, loc);
	}

}

std::shared_ptr<awst::Expression> AssemblyBuilder::memoryVar(awst::SourceLocation const& _loc)
{
	// Read slot 0 straight from scratch — no __evm_memory local cache.
	// The cached form caused puya to miscount the dig in large split pieces,
	// storing uint64 into slot 0; direct scratch loads/stores avoid that.
	return awst::makeLoadSlot(memorySlotFirst(), _loc);
}

void AssemblyBuilder::assignMemoryVar(
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	_out.push_back(awst::makeExpressionStatement(
		awst::makeStoreSlot(memorySlotFirst(), std::move(_value), _loc), _loc));
}

std::shared_ptr<awst::Expression> AssemblyBuilder::loadMemoryBlob(
	awst::SourceLocation const& _loc,
	int _slot
)
{
	return awst::makeLoadSlot(memorySlotFirst() + _slot, _loc);
}

void AssemblyBuilder::storeMemoryBlob(
	std::shared_ptr<awst::Expression> _blob,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out,
	int _slot
)
{
	auto storeOp = awst::makeStoreSlot(memorySlotFirst() + _slot, std::move(_blob), _loc);
	auto exprStmt = awst::makeExpressionStatement(std::move(storeOp), _loc);
	_out.push_back(std::move(exprStmt));
}

std::shared_ptr<awst::Expression> AssemblyBuilder::offsetToUint64(
	std::shared_ptr<awst::Expression> _offset,
	awst::SourceLocation const& _loc
)
{
	if (_offset->wtype == awst::WType::uint64Type())
		return _offset;
	if (auto constant = resolveConstantOffset(_offset))
		return awst::makeIntegerConstant(*constant, _loc);

	// Keep the high bits until validation. Truncating first could turn an
	// out-of-range Yul address into an unrelated valid scratch-memory address.
	std::string const id = "__puyasol_checked_offset";
	auto call = awst::makeSubroutineCall(awst::SubroutineID{id}, awst::WType::uint64Type(), _loc);
	awst::pushCallArg(call->args, ensureBiguint(std::move(_offset), _loc));
	auto& subs = m_typeMapper.artifacts().bufferSubroutines;
	if (subs.count(id)) return call;
	auto word = awst::makeVarExpression("word", awst::WType::biguintType(), _loc);
	auto fits = awst::makeNumericCompare(word, awst::NumericComparison::Lte,
		awst::makeBiguintConstant("18446744073709551615", _loc), _loc);
	static awst::WTuple type({awst::WType::uint64Type(), awst::WType::boolType()});
	auto pair = awst::makeTupleExpression(&type, _loc);
	pair->items = {safeBtoi(word, _loc), std::move(fits)};
	auto checked = std::make_shared<awst::CheckedMaybe>();
	checked->sourceLocation = _loc;
	checked->wtype = awst::WType::uint64Type();
	checked->expr = std::move(pair);
	checked->comment = "EVM offset exceeds AVM uint64 range";
	auto body = awst::makeBlock(_loc);
	body->body.push_back(awst::makeReturnStatement(std::move(checked), _loc));
	auto sub = awst::makeSubroutine(id, id, {{"word", awst::WType::biguintType(), _loc}},
		awst::WType::uint64Type(), std::move(body), false, _loc);
	sub->inlineOpt = false;
	subs.emplace(id, std::move(sub));
	return call;
}


void AssemblyBuilder::invalidateMemConstants()
{
	for (auto it = m_frame.localConstants.begin(); it != m_frame.localConstants.end();)
	{
		if (it->first.rfind("mem_0x", 0) == 0)
			it = m_frame.localConstants.erase(it);
		else
			++it;
	}
}


std::optional<uint64_t> AssemblyBuilder::resolveConstantOffset(
	std::shared_ptr<awst::Expression> const& _expr)
{
	if (auto const* literal = dynamic_cast<awst::IntegerConstant const*>(_expr.get()))
	{
		if (literal->value.empty() || literal->value.find_first_not_of("0123456789") != std::string::npos)
			return std::nullopt;
		solidity::bigint value{literal->value};
		if (value <= std::numeric_limits<uint64_t>::max())
			return static_cast<uint64_t>(value);
	}
	if (auto const* var = dynamic_cast<awst::VarExpression const*>(_expr.get());
		var && !m_frame.calldataParamNames.count(var->name))
		if (auto it = m_frame.localConstants.find(var->name); it != m_frame.localConstants.end())
			return it->second;
	// Source arithmetic is folded by solc before lowering. Never reinterpret
	// arbitrary AWST biguint arithmetic as wrapping native uint64 arithmetic.
	return std::nullopt;
}

// ─── Source location helper ─────────────────────────────────────────────────

awst::SourceLocation AssemblyBuilder::makeLoc(
	solidity::langutil::DebugData::ConstPtr const& _debugData
)
{
	if (_debugData)
		return m_typeMapper.sourceMap().toAwstLoc(
			m_context->sourceFile, _debugData->nativeLocation);
	return awst::SourceLocation(m_context->sourceFile);
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

namespace
{
/// Decimal digits, at least one of them non-zero. A non-decimal spelling (an
/// unconverted 0x literal) is rejected rather than guessed at.
bool isNonZeroDecimal(std::string const& _v)
{
	return !_v.empty()
		&& _v.find_first_not_of("0123456789") == std::string::npos
		&& _v.find_first_not_of('0') != std::string::npos;
}
} // namespace

namespace
{
/// Decimal string mod 32, digit by digit so the value's width never matters.
std::optional<unsigned> decimalMod32(std::string const& _v)
{
	if (_v.empty())
		return std::nullopt;
	unsigned r = 0;
	for (char c: _v)
	{
		if (c < '0' || c > '9')
			return std::nullopt;
		r = (r * 10 + static_cast<unsigned>(c - '0')) % 32;
	}
	return r;
}
} // namespace

std::optional<unsigned> AssemblyBuilder::alignmentMod32(
	awst::Expression const& _offset) const
{
	if (auto const* num = dynamic_cast<awst::IntegerConstant const*>(&_offset))
		return decimalMod32(num->value);
	if (auto const* se = dynamic_cast<awst::SingleEvaluation const*>(&_offset))
		return se->source ? alignmentMod32(*se->source) : std::nullopt;
	if (auto const* rc = dynamic_cast<awst::ReinterpretCast const*>(&_offset))
		return rc->expr ? alignmentMod32(*rc->expr) : std::nullopt;
	if (auto const* var = dynamic_cast<awst::VarExpression const*>(&_offset))
	{
		if (m_frame.alignedLocals.count(var->name))
			return 0u;
		if (auto fact = m_context->yulArgumentAlignments.find(var->name); fact != m_context->yulArgumentAlignments.end())
			return fact->second;
		auto it = m_context->yulConstantValues.find(var->name);
		if (it != m_context->yulConstantValues.end())
			return decimalMod32(it->second);
		return std::nullopt;
	}
	if (auto const* big = dynamic_cast<awst::BigUIntBinaryOperation const*>(&_offset))
	{
		// Yul arithmetic is 256-bit, so an offset arrives as biguint ops —
		// including the mod-2^256 wrap every add/mul carries.
		if (!big->left || !big->right)
			return std::nullopt;
		using B = awst::BigUIntBinaryOperator;
		auto l = alignmentMod32(*big->left);
		auto r = alignmentMod32(*big->right);
		switch (big->op)
		{
		case B::Add:
			if (l && r) return (*l + *r) % 32;
			return std::nullopt;
		case B::Sub:
			if (l && r) return (*l + 32 - *r) % 32;
			return std::nullopt;
		case B::Mult:
			if ((l && *l == 0) || (r && *r == 0)) return 0u;
			return std::nullopt;
		case B::Mod:
			// x mod m keeps x's residue whenever 32 divides m (2^256, 4096).
			if (r && *r == 0) return l;
			return std::nullopt;
		default:
			return std::nullopt;
		}
	}
	if (auto const* bin = dynamic_cast<awst::UInt64BinaryOperation const*>(&_offset))
	{
		if (!bin->left || !bin->right)
			return std::nullopt;
		using O = awst::UInt64BinaryOperator;
		auto l = alignmentMod32(*bin->left);
		auto r = alignmentMod32(*bin->right);
		switch (bin->op)
		{
		case O::Add:
			if (l && r) return (*l + *r) % 32;
			return std::nullopt;
		case O::Sub:
			if (l && r) return (*l + 32 - *r) % 32;
			return std::nullopt;
		case O::Mult:
			// One aligned factor is enough: 32 | a implies 32 | a*b.
			if ((l && *l == 0) || (r && *r == 0)) return 0u;
			return std::nullopt;
		case O::Mod:
			// See the biguint arm: a modulus that is a multiple of 32 keeps
			// the residue (the slot size itself is 4096).
			if (r && *r == 0) return l;
			return std::nullopt;
		case O::LShift:
			// Shifting left by 5+ multiplies by a multiple of 32.
			if (auto const* k = dynamic_cast<awst::IntegerConstant const*>(bin->right.get()))
			{
				auto shift = decimalMod32(k->value);
				if (shift && k->value.size() <= 2 && std::stoi(k->value) >= 5)
					return 0u;
			}
			return std::nullopt;
		default:
			// No FloorDiv/RShift rule: halving an aligned value is not aligned.
			return std::nullopt;
		}
	}
	return std::nullopt;
}

bool AssemblyBuilder::divisorIsKnownNonZero(awst::Expression const& _divisor) const
{
	// Only m_frame.localWideConstants (let-bound number literals) and literal nodes
	// qualify. m_frame.localConstants is deliberately NOT consulted: it doubles as the
	// calldata-param head-offset table, where the recorded number is an offset
	// and not the local's value.
	if (auto const* var = dynamic_cast<awst::VarExpression const*>(&_divisor))
	{
		auto it = m_frame.localWideConstants.find(var->name);
		return it != m_frame.localWideConstants.end() && isNonZeroDecimal(it->second);
	}
	if (auto const* num = dynamic_cast<awst::IntegerConstant const*>(&_divisor))
		return isNonZeroDecimal(num->value);
	return false;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::safeDivMod(
	std::shared_ptr<awst::Expression> _left,
	awst::BigUIntBinaryOperator _op,
	std::shared_ptr<awst::Expression> _right,
	awst::SourceLocation const& _loc
)
{
	// A divisor that is a compile-time non-zero constant cannot hit the AVM
	// panic, so the guard is pure overhead — and it is not cheap: its ternary
	// costs three basic blocks, and poseidon's 816 mulmods over one field prime
	// chained ~2500 of them into a single straight-line body, which puya's SSA
	// reader walks recursively until Python's stack gives out.
	if (_right && divisorIsKnownNonZero(*_right))
		return makeBigUIntBinOp(
			std::move(_left), _op, ensureBiguint(std::move(_right), _loc), _loc);

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

void AssemblyBuilder::buildYulSubroutine(
	solidity::yul::FunctionDefinition const& _funcDef,
	std::string const& _subroutineId,
	std::string const& _subroutineName
)
{
	auto loc = makeLoc(_funcDef.debugData);

	// Keep compilation facts and host-contract routing, but isolate the local
	// scope. solc forbids capturing outer stack variables in a Yul function.
	// Scratch memory/returndata/transient state are already shared by callsub;
	// calldata is the sole synthetic buffer passed explicitly below.
	AssemblyBuilder child(m_typeMapper, m_context);
	child.m_frame.yulSubroutine = &_funcDef;
	child.m_frame.useSyntheticCalldata = m_context->yulCalldataFunctions.count(_funcDef.name.str());

	size_t nRet = _funcDef.returnVariables.size();
	awst::WType const* retType = nRet == 0 ? awst::WType::voidType()
		: nRet == 1 ? awst::WType::biguintType()
		: m_typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>(nRet, awst::WType::biguintType()));
	child.m_frame.returnType = retType;

	std::vector<awst::SubroutineArgument> subArgs;
	for (auto const& p: _funcDef.parameters)
	{
		std::string pName = p.name.str();
		if (auto constant = m_context->yulConstantValues.find(pName); constant != m_context->yulConstantValues.end())
		{
			child.m_frame.localWideConstants[pName] = constant->second;
		}
		child.m_frame.locals[pName] = awst::WType::biguintType();
		subArgs.emplace_back(
			pName, awst::WType::biguintType(), makeLoc(p.debugData));
	}
	if (child.m_frame.useSyntheticCalldata)
	{
		subArgs.emplace_back(CD_BLOB_VAR, awst::WType::bytesType(), loc);
		child.m_frame.locals[CD_BLOB_VAR] = awst::WType::bytesType();
	}

	for (auto const& r: _funcDef.returnVariables)
		child.m_frame.locals[r.name.str()] = awst::WType::biguintType();

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
		child.buildStatement(stmt, bodyStmts);
	child.drainPendingStatements(bodyStmts);
	if (!child.m_frame.haltEmitted)
		child.emitYulSubroutineReturn(loc, bodyStmts);

	auto block = awst::makeBlock(loc);
	block->body = std::move(bodyStmts);

	auto sub = awst::makeSubroutine(
		_subroutineId, _subroutineName, std::move(subArgs),
		retType, std::move(block), /*pure=*/false, loc);

	m_typeMapper.artifacts().pendingYulSubroutines.push_back(std::move(sub));
}

void AssemblyBuilder::emitYulSubroutineReturn(
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	std::shared_ptr<awst::Expression> value;
	auto const& returns = m_frame.yulSubroutine->returnVariables;
	if (returns.size() == 1)
		value = awst::makeVarExpression(returns[0].name.str(), awst::WType::biguintType(), _loc);
	else if (returns.size() > 1)
	{
		auto tuple = awst::makeTupleExpression(m_frame.returnType, _loc);
		for (auto const& r: returns)
			tuple->items.push_back(awst::makeVarExpression(r.name.str(), awst::WType::biguintType(), _loc));
		value = std::move(tuple);
	}
	_out.push_back(awst::makeReturnStatement(std::move(value), _loc));
	m_frame.haltEmitted = true;
}

// ─── Expression translation ─────────────────────────────────────────────────


} // namespace puyasol::builder
