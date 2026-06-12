/// @file MemoryOps.cpp
/// Memory operations: mload, mstore, handleReturn, tryHandleBytesMemoryRead.
/// Uses scratch-slot-backed bytes blob for EVM memory simulation.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <sstream>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AssemblyBuilder::handleMload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 1, "mload", _loc))
		return nullptr;

	// First check calldata map for constant offsets (function parameters)
	auto constOffset = resolveConstantOffset(_args[0]);
	if (constOffset)
	{
		auto cdIt = m_calldataMap.find(*constOffset);
		if (cdIt != m_calldataMap.end())
		{
			auto const& elem = cdIt->second;
			auto base = awst::makeVarExpression(elem.paramName, m_locals.count(elem.paramName)
				? m_locals[elem.paramName]
				: awst::WType::biguintType(), _loc);

			return accessFlatElement(std::move(base), elem.paramType, elem.flatIndex, _loc);
		}
		// Not a calldata parameter → read from EVM memory at this constant
		// offset, routed to the scratch slot holding it (all slots, including 0,
		// are read directly from scratch — no __evm_memory local cache).
		return awst::makeAsBiguint(readMemWordConst(*constOffset, _loc), _loc);
	}

	// Dynamic offset → route to the correct slot at runtime (offsets < SLOT_SIZE
	// hit slot 0; all slots read directly from scratch). mload → uint256.
	return awst::makeAsBiguint(readMemWordDyn(_args[0], _loc), _loc);
}

// ── Multi-slot memory word access ───────────────────────────────────────────
// EVM memory spans scratch slots [MEMORY_SLOT_FIRST, MEMORY_SLOT_LAST], each
// SLOT_SIZE (4096) bytes. A byte offset maps to (slot = offset / SLOT_SIZE,
// sub = offset % SLOT_SIZE). Every slot — including slot 0 — is read/written
// directly against scratch (loads/stores); there is no __evm_memory local cache.
// 32-byte-aligned accesses
// never straddle a slot boundary (SLOT_SIZE % 32 == 0); unaligned straddling
// words are stitched/split across the two adjacent slots.

std::shared_ptr<awst::Statement> AssemblyBuilder::memBoundsAssert(
	std::shared_ptr<awst::Expression> _off, awst::SourceLocation const& _loc)
{
	// Blob capacity (bytes) = SLOT_SIZE * slot count; a 32-byte word must fit:
	// assert(off + 32 <= cap). Beyond this the access would spill into a
	// non-memory scratch slot (silent corruption) or error opaquely (slot>255).
	uint64_t cap = static_cast<uint64_t>(SLOT_SIZE) * static_cast<uint64_t>(MEMORY_SLOT_LAST + 1);
	auto end = awst::makeUInt64BinOp(std::move(_off), awst::UInt64BinaryOperator::Add,
		awst::makeIntegerConstant(static_cast<uint64_t>(32), _loc), _loc);
	auto cond = awst::makeNumericCompare(std::move(end), awst::NumericComparison::Lte,
		awst::makeIntegerConstant(cap, _loc), _loc);
	return awst::makeExpressionStatement(
		awst::makeAssert(std::move(cond), _loc,
			"EVM memory access exceeds the modeled scratch blob (raise --evm-memory-slots)"), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::readMemWordConst(
	uint64_t _offset, awst::SourceLocation const& _loc)
{
	int slot = static_cast<int>(_offset / SLOT_SIZE);
	uint64_t sub = _offset % SLOT_SIZE;

	if (slot == 0)
		return awst::makeExtract3(memoryVar(_loc), awst::makeIntegerConstant(_offset, _loc),
			awst::makeIntegerConstant("32", _loc), _loc);

	if (slot > MEMORY_SLOT_LAST)
		Logger::instance().error("EVM memory read beyond the reserved scratch slots (raise --evm-memory-slots)", _loc);

	if (sub + 32 <= static_cast<uint64_t>(SLOT_SIZE))
		return awst::makeExtract3(awst::makeLoadSlot(MEMORY_SLOT_FIRST + slot, _loc),
			awst::makeIntegerConstant(sub, _loc), awst::makeIntegerConstant("32", _loc), _loc);

	// Straddles the slot boundary: tail of `slot` ++ head of `slot+1`.
	uint64_t firstLen = static_cast<uint64_t>(SLOT_SIZE) - sub;
	auto part1 = awst::makeExtract3(awst::makeLoadSlot(MEMORY_SLOT_FIRST + slot, _loc),
		awst::makeIntegerConstant(sub, _loc), awst::makeIntegerConstant(firstLen, _loc), _loc);
	auto part2 = awst::makeExtract3(awst::makeLoadSlot(MEMORY_SLOT_FIRST + slot + 1, _loc),
		awst::makeIntegerConstant("0", _loc), awst::makeIntegerConstant(32 - firstLen, _loc), _loc);
	return awst::makeConcat(std::move(part1), std::move(part2), _loc);
}

void AssemblyBuilder::writeMemWordConst(
	uint64_t _offset, std::shared_ptr<awst::Expression> _value32,
	awst::SourceLocation const& _loc, std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	int slot = static_cast<int>(_offset / SLOT_SIZE);
	uint64_t sub = _offset % SLOT_SIZE;

	if (slot == 0)
	{
		auto rep = awst::makeReplace3(memoryVar(_loc), awst::makeIntegerConstant(_offset, _loc),
			std::move(_value32), _loc);
		assignMemoryVar(std::move(rep), _loc, _out);
		return;
	}

	if (slot > MEMORY_SLOT_LAST)
		Logger::instance().error("EVM memory write beyond the reserved scratch slots (raise --evm-memory-slots)", _loc);

	if (sub + 32 <= static_cast<uint64_t>(SLOT_SIZE))
	{
		auto rep = awst::makeReplace3(awst::makeLoadSlot(MEMORY_SLOT_FIRST + slot, _loc),
			awst::makeIntegerConstant(sub, _loc), std::move(_value32), _loc);
		storeMemoryBlob(std::move(rep), _loc, _out, slot);
		return;
	}

	// Straddles the slot boundary: split the 32-byte value across two slots.
	// Materialise it in a temp local so the value isn't evaluated twice.
	uint64_t firstLen = static_cast<uint64_t>(SLOT_SIZE) - sub;
	std::string tmp = "__mem_straddle_" + std::to_string(_offset);
	auto tmpTarget = awst::makeVarExpression(tmp, awst::WType::bytesType(), _loc);
	_out.push_back(awst::makeAssignmentStatement(std::move(tmpTarget), std::move(_value32), _loc));
	auto tmpRead = [&]() { return awst::makeVarExpression(tmp, awst::WType::bytesType(), _loc); };

	auto valPart1 = awst::makeExtract3(tmpRead(), awst::makeIntegerConstant("0", _loc),
		awst::makeIntegerConstant(firstLen, _loc), _loc);
	auto rep1 = awst::makeReplace3(awst::makeLoadSlot(MEMORY_SLOT_FIRST + slot, _loc),
		awst::makeIntegerConstant(sub, _loc), std::move(valPart1), _loc);
	storeMemoryBlob(std::move(rep1), _loc, _out, slot);

	auto valPart2 = awst::makeExtract3(tmpRead(), awst::makeIntegerConstant(firstLen, _loc),
		awst::makeIntegerConstant(32 - firstLen, _loc), _loc);
	auto rep2 = awst::makeReplace3(awst::makeLoadSlot(MEMORY_SLOT_FIRST + slot + 1, _loc),
		awst::makeIntegerConstant("0", _loc), std::move(valPart2), _loc);
	storeMemoryBlob(std::move(rep2), _loc, _out, slot + 1);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::readMemWordDyn(
	std::shared_ptr<awst::Expression> _offset, awst::SourceLocation const& _loc)
{
	auto off = offsetToUint64(std::move(_offset), _loc);
	// Evaluate the offset once: it appears in the bounds assert, the slot-0 fast
	// path, and the slot/sub slow path (~5 references); a side-effecting offset
	// like mload(q) would otherwise re-run each time. writeMemWordDyn already
	// materializes its offset for the same reason.
	off = awst::makeSingleEvaluation(
		std::move(off), awst::WType::uint64Type(), awst::nextSingleEvalId(), _loc);
	// Fail clearly if this offset spills past the modeled blob (vs silently
	// reading a non-memory scratch slot); fires before the read is consumed.
	m_pendingStatements.push_back(memBoundsAssert(off, _loc));
	auto ss = [&]() { return awst::makeIntegerConstant(static_cast<uint64_t>(SLOT_SIZE), _loc); };

	// offset < SLOT_SIZE → cached slot-0 local (unchanged for ≤4KB memory).
	auto cmp = awst::makeNumericCompare(off, awst::NumericComparison::Lt, ss(), _loc);
	auto fast = awst::makeExtract3(memoryVar(_loc), off, awst::makeIntegerConstant("32", _loc), _loc);

	// else extract3(loads(off / SLOT_SIZE), off % SLOT_SIZE, 32).
	auto slot = awst::makeUInt64BinOp(off, awst::UInt64BinaryOperator::FloorDiv, ss(), _loc);
	auto sub = awst::makeUInt64BinOp(off, awst::UInt64BinaryOperator::Mod, ss(), _loc);
	auto loadsCall = awst::makeIntrinsicCall("loads", awst::WType::bytesType(), _loc);
	loadsCall->stackArgs.push_back(std::move(slot));
	auto slow = awst::makeExtract3(std::move(loadsCall), std::move(sub), awst::makeIntegerConstant("32", _loc), _loc);

	return awst::makeConditional(std::move(cmp), std::move(fast), std::move(slow),
		awst::WType::bytesType(), _loc);
}

void AssemblyBuilder::writeMemWordDyn(
	std::shared_ptr<awst::Expression> _offset, std::shared_ptr<awst::Expression> _value32,
	awst::SourceLocation const& _loc, std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	static int s_ctr = 0;
	int id = s_ctr++;
	auto ss = [&]() { return awst::makeIntegerConstant(static_cast<uint64_t>(SLOT_SIZE), _loc); };

	// Materialise offset + value once so neither branch re-evaluates them.
	std::string offN = "__mem_dyn_off_" + std::to_string(id);
	std::string valN = "__mem_dyn_val_" + std::to_string(id);
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(offN, awst::WType::uint64Type(), _loc),
		offsetToUint64(std::move(_offset), _loc), _loc));
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(valN, awst::WType::bytesType(), _loc),
		std::move(_value32), _loc));
	auto offR = [&]() { return awst::makeVarExpression(offN, awst::WType::uint64Type(), _loc); };
	auto valR = [&]() { return awst::makeVarExpression(valN, awst::WType::bytesType(), _loc); };
	// Fail clearly if the write spills past the modeled blob (would otherwise
	// silently corrupt a non-memory scratch slot).
	_out.push_back(memBoundsAssert(offR(), _loc));

	// Fast: __evm_memory = replace3(__evm_memory, off, val)  [slot 0]
	auto fastBlock = awst::makeBlock(_loc);
	{
		auto rep = awst::makeReplace3(memoryVar(_loc), offR(), valR(), _loc);
		assignMemoryVar(std::move(rep), _loc, fastBlock->body);
	}

	// Slow: stores(slot, replace3(loads(slot), sub, val))  [slot 1+]
	auto slowBlock = awst::makeBlock(_loc);
	{
		std::string slotN = "__mem_dyn_slot_" + std::to_string(id);
		slowBlock->body.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(slotN, awst::WType::uint64Type(), _loc),
			awst::makeUInt64BinOp(offR(), awst::UInt64BinaryOperator::FloorDiv, ss(), _loc), _loc));
		auto slotR = [&]() { return awst::makeVarExpression(slotN, awst::WType::uint64Type(), _loc); };
		auto sub = awst::makeUInt64BinOp(offR(), awst::UInt64BinaryOperator::Mod, ss(), _loc);
		auto loadsCall = awst::makeIntrinsicCall("loads", awst::WType::bytesType(), _loc);
		loadsCall->stackArgs.push_back(slotR());
		auto rep = awst::makeReplace3(std::move(loadsCall), std::move(sub), valR(), _loc);
		auto storesCall = awst::makeIntrinsicCall("stores", awst::WType::voidType(), _loc);
		storesCall->stackArgs.push_back(slotR());
		storesCall->stackArgs.push_back(std::move(rep));
		slowBlock->body.push_back(awst::makeExpressionStatement(std::move(storesCall), _loc));
	}

	auto cmp = awst::makeNumericCompare(offR(), awst::NumericComparison::Lt, ss(), _loc);
	_out.push_back(awst::makeIfElse(std::move(cmp), std::move(fastBlock), std::move(slowBlock), _loc));
}

std::shared_ptr<awst::Expression> AssemblyBuilder::readMemWordDirect(
	std::shared_ptr<awst::Expression> _offset, awst::SourceLocation const& _loc)
{
	auto ss = [&]() { return awst::makeIntegerConstant(static_cast<uint64_t>(SLOT_SIZE), _loc); };
	auto slot = awst::makeUInt64BinOp(_offset, awst::UInt64BinaryOperator::FloorDiv, ss(), _loc);
	auto sub = awst::makeUInt64BinOp(std::move(_offset), awst::UInt64BinaryOperator::Mod, ss(), _loc);
	auto loadsCall = awst::makeIntrinsicCall("loads", awst::WType::bytesType(), _loc);
	loadsCall->stackArgs.push_back(std::move(slot));
	return awst::makeExtract3(std::move(loadsCall), std::move(sub),
		awst::makeIntegerConstant("32", _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::readMemRangeDirect(
	std::shared_ptr<awst::Expression> _offset, int _byteLen, awst::SourceLocation const& _loc)
{
	// Concatenate ceil(_byteLen/32) successive 32-byte words. Each word read
	// re-derives its slot, so a range straddling a SLOT_SIZE boundary is handled
	// transparently. `_offset` is shared (pure base offset) across all words.
	int words = (_byteLen + 31) / 32;
	std::shared_ptr<awst::Expression> acc;
	for (int i = 0; i < words; ++i)
	{
		std::shared_ptr<awst::Expression> wordOff = (i == 0)
			? _offset
			: awst::makeUInt64BinOp(_offset, awst::UInt64BinaryOperator::Add,
				awst::makeIntegerConstant(static_cast<uint64_t>(i * 32), _loc), _loc);
		auto word = readMemWordDirect(std::move(wordOff), _loc);
		acc = acc ? awst::makeConcat(std::move(acc), std::move(word), _loc) : std::move(word);
	}
	// Trim to the exact byte length when not word-aligned.
	if (_byteLen % 32 != 0)
		acc = awst::makeExtract3(std::move(acc), awst::makeIntegerConstant("0", _loc),
			awst::makeIntegerConstant(static_cast<uint64_t>(_byteLen), _loc), _loc);
	return acc;
}

void AssemblyBuilder::writeMemWordDirect(
	std::shared_ptr<awst::Expression> _offset, std::shared_ptr<awst::Expression> _value32,
	awst::SourceLocation const& _loc, std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	static int s_ctr = 0;
	int id = s_ctr++;
	auto ss = [&]() { return awst::makeIntegerConstant(static_cast<uint64_t>(SLOT_SIZE), _loc); };

	// Fail clearly if the write spills past the modeled blob.
	_out.push_back(memBoundsAssert(_offset, _loc));

	std::string slotN = "__blobw_slot_" + std::to_string(id);
	std::string valN = "__blobw_val_" + std::to_string(id);
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(slotN, awst::WType::uint64Type(), _loc),
		awst::makeUInt64BinOp(_offset, awst::UInt64BinaryOperator::FloorDiv, ss(), _loc), _loc));
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(valN, awst::WType::bytesType(), _loc),
		std::move(_value32), _loc));
	auto slotR = [&]() { return awst::makeVarExpression(slotN, awst::WType::uint64Type(), _loc); };
	auto sub = awst::makeUInt64BinOp(std::move(_offset), awst::UInt64BinaryOperator::Mod, ss(), _loc);

	auto loadsCall = awst::makeIntrinsicCall("loads", awst::WType::bytesType(), _loc);
	loadsCall->stackArgs.push_back(slotR());
	auto rep = awst::makeReplace3(std::move(loadsCall), std::move(sub),
		awst::makeVarExpression(valN, awst::WType::bytesType(), _loc), _loc);
	auto storesCall = awst::makeIntrinsicCall("stores", awst::WType::voidType(), _loc);
	storesCall->stackArgs.push_back(slotR());
	storesCall->stackArgs.push_back(std::move(rep));
	_out.push_back(awst::makeExpressionStatement(std::move(storesCall), _loc));
}

std::shared_ptr<awst::Expression> AssemblyBuilder::tryHandleBytesMemoryRead(
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
	if (!outerAdd || getFunctionName(outerAdd->functionName) != "add"
		|| outerAdd->arguments.size() != 2)
		return nullptr;

	// One arg of outer add should be add(bytes_param, 32), the other is the offset
	solidity::yul::FunctionCall const* innerAdd = nullptr;
	solidity::yul::Expression const* offsetExprYul = nullptr;

	auto* call0 = std::get_if<solidity::yul::FunctionCall>(&outerAdd->arguments[0]);
	auto* call1 = std::get_if<solidity::yul::FunctionCall>(&outerAdd->arguments[1]);

	if (call0 && getFunctionName(call0->functionName) == "add" && call0->arguments.size() == 2)
	{
		innerAdd = call0;
		offsetExprYul = &outerAdd->arguments[1];
	}
	else if (call1 && getFunctionName(call1->functionName) == "add" && call1->arguments.size() == 2)
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
	auto paramVar = awst::makeVarExpression(paramName, paramType, _loc);

	// Translate the dynamic offset and convert biguint → uint64
	auto offsetExpr = buildExpression(*offsetExprYul);

	auto offsetBytes = awst::makeAsBytes(offsetExpr, _loc);

	auto offsetU64 = awst::makeBtoi(std::move(offsetBytes), _loc);

	// Length: 32 bytes
	auto lenArg = awst::makeIntegerConstant("32", _loc);

	// extract3(param, offset, 32)
	auto extract = awst::makeExtract3(std::move(paramVar), std::move(offsetU64), std::move(lenArg), _loc);
	// Cast bytes → biguint (mload returns uint256)
	auto result = awst::makeAsBiguint(std::move(extract), _loc);

	return result;
}

bool AssemblyBuilder::tryHandleBytesMemoryWrite(
	solidity::yul::FunctionCall const& _call,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// Match: mstore(add(bytes_var, 32), value)
	// or:    mstore(add(32, bytes_var), value)
	//
	// In EVM: bytes memory x has layout [length(32)][data...] at address x.
	// add(x, 32) points to the data region. mstore writes 32 bytes there.
	// The variable's length is unchanged, so on return only len(x) bytes matter.
	//
	// In AVM: x is raw bytes (no length header). We overwrite x's content
	// with the first len(x) bytes of the 32-byte value.

	if (_call.arguments.size() != 2)
		return false;

	// First arg must be add(bytes_var, 32) or add(32, bytes_var)
	auto* addCall = std::get_if<solidity::yul::FunctionCall>(&_call.arguments[0]);
	if (!addCall || getFunctionName(addCall->functionName) != "add"
		|| addCall->arguments.size() != 2)
		return false;

	// Find which arg is 32 and which is the bytes variable
	solidity::yul::Expression const* varExpr = nullptr;

	auto val0 = resolveConstantYulValue(addCall->arguments[0]);
	auto val1 = resolveConstantYulValue(addCall->arguments[1]);

	if (val1 && *val1 == 32)
		varExpr = &addCall->arguments[0];
	else if (val0 && *val0 == 32)
		varExpr = &addCall->arguments[1];
	else
		return false;

	// The variable must be an Identifier referencing a bytes/string local
	auto* varId = std::get_if<solidity::yul::Identifier>(varExpr);
	if (!varId)
		return false;

	std::string varName = varId->name.str();
	auto localIt = m_locals.find(varName);
	if (localIt == m_locals.end())
		return false;

	auto* varType = localIt->second;
	if (varType != awst::WType::bytesType() && varType != awst::WType::stringType())
		return false;

	// Pattern matched! Translate the value expression.
	auto valueExpr = buildExpression(_call.arguments[1]);

	Logger::instance().debug(
		"mstore bytes memory write: replacing content of '" + varName + "'", _loc
	);

	// Build: x = extract3(pad32(value), 0, len(x))
	// This overwrites x with the first len(x) bytes of the 32-byte value.

	// Reference to the variable
	auto varRef = awst::makeVarExpression(varName, varType, _loc);

	// len(x)
	auto lenCall = awst::makeLen(varRef, _loc);

	// pad32(value) — get the 32 bytes representation
	auto padded = padTo32Bytes(ensureBiguint(valueExpr, _loc), _loc);

	// extract3(padded, 0, len(x))
	auto zero = awst::makeZero(_loc);

	auto extract = awst::makeExtract3(std::move(padded), std::move(zero), std::move(lenCall), _loc);
	// Cast if needed for string type
	std::shared_ptr<awst::Expression> newValue = std::move(extract);
	if (varType == awst::WType::stringType())
	{
		auto cast = awst::makeReinterpretCast(std::move(newValue), awst::WType::stringType(), _loc);
		newValue = std::move(cast);
	}

	// x = newValue
	auto target = awst::makeVarExpression(varName, varType, _loc);

	auto assign = awst::makeAssignmentStatement(std::move(target), std::move(newValue), _loc);
	_out.push_back(std::move(assign));

	return true;
}

// ── tryHandleBytesMemoryMcopy ──────────────────────────────────────────────
//
// Matches: mcopy(add(add(bytes_var, 0x20), dstOff),
//                add(add(bytes_var, 0x20), srcOff),
//                len)
//
// In EVM, `bytes memory x` has layout [uint256_len][data...] at pointer x.
// `add(x, 0x20)` skips the 32-byte length header to reach the data region.
// `add(add(x, 0x20), offset)` points to data[offset].
//
// In AVM, `x` is stored as raw bytes with NO length header, so `data[offset]`
// is at position `offset` directly. The translation is:
//   extract3(src_var, srcOff, len)  →  the source bytes
//   replace3(dst_var, dstOff, ...)  →  write into dst at dstOff
//
// Supports both same-variable (intra-buffer overlap copy) and
// cross-variable (inter-buffer) copies.

bool AssemblyBuilder::tryHandleBytesMemoryMcopy(
	solidity::yul::FunctionCall const& _call,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (_call.arguments.size() != 3)
		return false;

	// Match add(add(bytes_var, 0x20), dynOffset) in raw Yul.
	// Returns {varName, dynOffsetExpr*} or {"", nullptr} if no match.
	auto matchPtr = [&](solidity::yul::Expression const& _expr)
		-> std::pair<std::string, solidity::yul::Expression const*>
	{
		auto const* outerAdd = std::get_if<solidity::yul::FunctionCall>(&_expr);
		if (!outerAdd) return {};
		if (getFunctionName(outerAdd->functionName) != "add"
			|| outerAdd->arguments.size() != 2)
			return {};

		for (int innerIdx = 0; innerIdx < 2; ++innerIdx)
		{
			auto const* innerAdd = std::get_if<solidity::yul::FunctionCall>(&outerAdd->arguments[innerIdx]);
			if (!innerAdd) continue;
			if (getFunctionName(innerAdd->functionName) != "add"
				|| innerAdd->arguments.size() != 2)
				continue;

			// Inner add must have one arg = literal 32 (0x20)
			for (int litIdx = 0; litIdx < 2; ++litIdx)
			{
				auto constVal = resolveConstantYulValue(innerAdd->arguments[litIdx]);
				if (!constVal || *constVal != 32) continue;

				// The other arg is the bytes_var identifier
				auto const* varId = std::get_if<solidity::yul::Identifier>(&innerAdd->arguments[1 - litIdx]);
				if (!varId) continue;

				// dynOffset is the other arg of the outer add
				auto const* dynOff = &outerAdd->arguments[1 - innerIdx];
				return {varId->name.str(), dynOff};
			}
		}
		return {};
	};

	// Match dst and src pointers
	auto [dstVar, dstOffYul] = matchPtr(_call.arguments[0]);
	auto [srcVar, srcOffYul] = matchPtr(_call.arguments[1]);

	if (dstVar.empty() || srcVar.empty())
		return false;

	// Both must be known bytes/string locals
	auto dstIt = m_locals.find(dstVar);
	auto srcIt = m_locals.find(srcVar);
	if (dstIt == m_locals.end() || srcIt == m_locals.end())
		return false;
	if (dstIt->second != awst::WType::bytesType() && dstIt->second != awst::WType::stringType())
		return false;
	if (srcIt->second != awst::WType::bytesType() && srcIt->second != awst::WType::stringType())
		return false;

	Logger::instance().debug(
		"mcopy bytes memory: replace3(" + dstVar + ", dstOff, extract3(" + srcVar + ", srcOff, len))", _loc);

	// Translate offsets and length from Yul
	auto dstOffExpr = offsetToUint64(buildExpression(*dstOffYul), _loc);
	auto srcOffExpr = offsetToUint64(buildExpression(*srcOffYul), _loc);
	auto lenExpr    = offsetToUint64(buildExpression(_call.arguments[2]), _loc);

	// src_bytes = extract3(src_var, srcOff, len)
	auto srcVarRef = awst::makeVarExpression(srcVar, srcIt->second, _loc);
	auto srcBytes  = awst::makeExtract3(std::move(srcVarRef), std::move(srcOffExpr), std::move(lenExpr), _loc);

	// dst_var = replace3(dst_var, dstOff, src_bytes)
	auto dstVarRef  = awst::makeVarExpression(dstVar, dstIt->second, _loc);
	auto replaced   = awst::makeReplace3(std::move(dstVarRef), std::move(dstOffExpr), std::move(srcBytes), _loc);

	auto dstTarget  = awst::makeVarExpression(dstVar, dstIt->second, _loc);
	auto assign     = awst::makeAssignmentStatement(std::move(dstTarget), std::move(replaced), _loc);
	_out.push_back(std::move(assign));
	return true;
}

void AssemblyBuilder::handleMstore(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (!checkArity(_args, 2, "mstore", _loc))
		return;

	// Track last mstore value for dynamic-length keccak256 patterns
	m_lastMstoreValue = _args[1];

	// Track constant values stored to memory (especially free memory pointer)
	auto constOffset = resolveConstantOffset(_args[0]);
	if (constOffset)
	{
		auto storedVal = resolveConstantOffset(_args[1]);
		if (storedVal)
		{
			// Track by offset for resolveConstantOffset to find later
			std::string varName = "mem_0x" + ([&] {
				std::ostringstream oss;
				oss << std::hex << *constOffset;
				return oss.str();
			})();
			m_localConstants[varName] = *storedVal;
		}
	}

	// Write 32 bytes into EVM memory at the given offset.
	auto padded = padTo32Bytes(ensureBiguint(_args[1], _loc), _loc);

	// Constant offset → route to the scratch slot holding it (every slot,
	// including 0, is load-modify-stored directly in scratch — no local cache).
	if (constOffset)
	{
		writeMemWordConst(*constOffset, std::move(padded), _loc, _out);
		return;
	}

	// Dynamic offset → route to the correct slot at runtime (offsets < SLOT_SIZE
	// hit slot 0; all slots written directly to scratch).
	writeMemWordDyn(_args[0], std::move(padded), _loc, _out);
}

void AssemblyBuilder::handleMstore8(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (!checkArity(_args, 2, "mstore8", _loc))
		return;

	// mstore8(ptr, value): write the low 8 bits of value as a single byte
	// at memory[ptr]. Pad the value to 32 bytes and extract byte[31] (the
	// low byte), then replace3 one byte at the target offset in the blob.
	auto offsetU64 = offsetToUint64(_args[0], _loc);
	auto padded = padTo32Bytes(ensureBiguint(_args[1], _loc), _loc);

	auto start = awst::makeIntegerConstant("31", _loc);
	auto len = awst::makeOne(_loc);

	auto lowByte = awst::makeExtract3(std::move(padded), std::move(start), std::move(len), _loc);
	auto replace = awst::makeReplace3(memoryVar(_loc), std::move(offsetU64), std::move(lowByte), _loc);
	assignMemoryVar(std::move(replace), _loc, _out);
}

void AssemblyBuilder::handleReturn(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (!checkArity(_args, 2, "return", _loc))
		return;

	// return(offset, size): Return the value stored at memory[offset]

	// Void Solidity function with assembly return(offset, size) — EVM pattern
	// that bypasses ABI encoding. On AVM, emit the data as a structured log
	// so callers can read it from transaction logs.
	if (!m_returnType || m_returnType == awst::WType::voidType())
	{
		auto returnOffset = resolveConstantOffset(_args[0]);
		auto returnSize = resolveConstantOffset(_args[1]);

		if (!returnOffset || !returnSize || *returnSize == 0)
		{
			// `assembly { return(_, 0) }` halts the entire program with
			// success. Emit the raw AVM `return 1` intrinsic so puya lowers
			// it to an unconditional program-exit (not just a subroutine
			// return). Needed for Yul helpers that use the EVM `return`
			// opcode as a hard exit from inside a nested call.
			flushMemoryToScratch(_loc, _out);

			auto returnOp = awst::makeIntrinsicCall("return", awst::WType::voidType(), _loc);
			returnOp->stackArgs.push_back(awst::makeTrue(_loc));

			auto exitStmt = awst::makeExpressionStatement(std::move(returnOp), _loc);
			_out.push_back(std::move(exitStmt));
			m_haltEmitted = true;
			return;
		}

		Logger::instance().warning(
			"assembly return() in void function — emitting " +
			std::to_string(*returnSize) + " bytes as structured log", _loc
		);

		// Read the return region from the memory blob: extract3(blob, offset, size)
		auto offsetU64 = awst::makeIntegerConstant(*returnOffset, _loc);

		auto sizeU64 = awst::makeIntegerConstant(*returnSize, _loc);

		auto extract = awst::makeExtract3(memoryVar(_loc), std::move(offsetU64), std::move(sizeU64), _loc);
		// log(data) — emit the raw bytes as a transaction log
		auto logCall = awst::makeIntrinsicCall("log", awst::WType::voidType(), _loc);
		logCall->stackArgs.push_back(std::move(extract));

		auto logStmt = awst::makeExpressionStatement(std::move(logCall), _loc);
		_out.push_back(std::move(logStmt));

		// Flush and return void
		flushMemoryToScratch(_loc, _out);
		auto ret = awst::makeReturnStatement(nullptr, _loc);
		_out.push_back(std::move(ret));
		return;
	}

	// EIP-2330 batch-read idiom (Extsload/Exttload): assembly hand-builds an
	// EVM-ABI-encoded `bytes32[]` in memory and returns it with a runtime
	// offset/size — `return(start, sub(end, start))`. The memory region is full
	// EVM ABI:  [0x00] 0x20 offset word | [0x20] uint256 length | [0x40..] elems.
	// The AVM method return type is an ARC4 dynamic array of `byte[32]`, whose
	// layout is `uint16 length ++ elements` (elements are 32-byte static, so no
	// per-element offset table). Convert by stitching the ARC4 length prefix
	// (the low 2 bytes of the EVM length word) onto the element bytes, then
	// reinterpret as the return type. Guarded to `byte[32]` elements only — any
	// other element type falls through to the errors below rather than emitting
	// a silently-wrong layout.
	if (auto const* dynArr = dynamic_cast<awst::ARC4DynamicArray const*>(m_returnType);
		dynArr && dynArr->elementType()->name() == "byte[32]"
		&& !resolveConstantOffset(_args[0]))
	{
		// count (ARC4 uint16) = last 2 bytes of the EVM length word at start+0x20
		auto countOff = awst::makeUInt64BinOp(
			offsetToUint64(_args[0], _loc),
			awst::UInt64BinaryOperator::Add,
			awst::makeIntegerConstant(uint64_t{0x3E}, _loc), _loc);
		auto countBytes = awst::makeExtract3(
			memoryVar(_loc), std::move(countOff),
			awst::makeIntegerConstant(uint64_t{2}, _loc), _loc);

		// elements = region[start+0x40 .. start+size)  → (size - 0x40) bytes
		auto elemsOff = awst::makeUInt64BinOp(
			offsetToUint64(_args[0], _loc),
			awst::UInt64BinaryOperator::Add,
			awst::makeIntegerConstant(uint64_t{0x40}, _loc), _loc);
		auto elemsLen = awst::makeUInt64BinOp(
			offsetToUint64(_args[1], _loc),
			awst::UInt64BinaryOperator::Sub,
			awst::makeIntegerConstant(uint64_t{0x40}, _loc), _loc);
		auto elemsBytes = awst::makeExtract3(
			memoryVar(_loc), std::move(elemsOff), std::move(elemsLen), _loc);

		auto arc4Bytes = awst::makeConcat(std::move(countBytes), std::move(elemsBytes), _loc);
		auto returnValue = awst::makeReinterpretCast(std::move(arc4Bytes), m_returnType, _loc);

		emitArc4ReturnHalt(std::move(returnValue), _loc, _out);
		return;
	}

	auto offset = resolveConstantOffset(_args[0]);
	if (!offset)
	{
		Logger::instance().error(
			"return with non-constant offset not supported", _loc
		);
		return;
	}

	// Read from the memory blob at this offset
	std::shared_ptr<awst::Expression> returnValue = readMemSlot(*offset, _loc);

	// Convert to bool if the function's return type is bool
	if (m_returnType == awst::WType::boolType()
		&& returnValue->wtype != awst::WType::boolType())
	{
		auto zero = awst::makeBiguintConstant("0", _loc);

		auto cmp = awst::makeNumericCompare(std::move(returnValue), awst::NumericComparison::Ne, std::move(zero), _loc);
		returnValue = std::move(cmp);
	}

	// When the function returns an array type but assembly produces a scalar,
	// the assembly was manually building ABI-encoded memory (EVM-specific).
	// Return an empty array as fallback since the memory ops don't translate.
	if (m_returnType && dynamic_cast<awst::ReferenceArray const*>(m_returnType)
		&& !dynamic_cast<awst::ReferenceArray const*>(returnValue->wtype))
	{
		// HARD ERROR — returning an empty array would silently hand the caller
		// `[]` instead of the real ABI-encoded data the assembly built in EVM
		// memory. Refuse to compile rather than emit a wrong return value.
		Logger::instance().error(
			"assembly `return(offset, size)` builds an ABI-encoded array in EVM "
			"memory, which has no AVM translation here; returning an empty array "
			"would silently hand the caller `[]` instead of the real data.", _loc
		);
		auto emptyArr = awst::makeNewArray(m_returnType, _loc);
		returnValue = std::move(emptyArr);
	}

	emitArc4ReturnHalt(std::move(returnValue), _loc, _out);
}

void AssemblyBuilder::emitArc4ReturnHalt(
	std::shared_ptr<awst::Expression> _value,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// EVM `return(offset, size)` halts the WHOLE call — even when it executes
	// inside a nested internal function (errors/require_error_evaluation_order_1:
	// an assembly `return` inside a require error-arg must end the call with
	// that data, not return from the enclosing subroutine). Lower it as the
	// ARC4 return log the router would emit — log(0x151f7c75 ++ ARC4(value)) —
	// followed by the raw AVM `return 1` program exit. The previous lowering
	// (subroutine ReturnStatement) was only correct when the enclosing
	// function happened to be the externally-called one.
	flushMemoryToScratch(_loc, _out);

	std::shared_ptr<awst::Expression> arc4Value = std::move(_value);
	bool alreadyArc4 = arc4Value->wtype
		&& arc4Value->wtype->kind() >= awst::WTypeKind::ARC4UIntN
		&& arc4Value->wtype->kind() <= awst::WTypeKind::ARC4Struct;
	if (!alreadyArc4)
	{
		auto* arc4Type = m_typeMapper.mapToARC4Type(
			arc4Value->wtype ? arc4Value->wtype : m_returnType);
		if (arc4Type && arc4Value->wtype != arc4Type)
			arc4Value = awst::makeARC4Encode(std::move(arc4Value), arc4Type, _loc);
	}
	auto arc4Bytes = awst::makeAsBytes(std::move(arc4Value), _loc);

	// 0x151f7c75 — the ARC4 return-value log prefix.
	auto prefix = awst::makeBytesConstant({0x15, 0x1f, 0x7c, 0x75}, _loc);
	auto logCall = awst::makeIntrinsicCall("log", awst::WType::voidType(), _loc);
	logCall->stackArgs.push_back(
		awst::makeConcat(std::move(prefix), std::move(arc4Bytes), _loc));
	_out.push_back(awst::makeExpressionStatement(std::move(logCall), _loc));

	auto returnOp = awst::makeIntrinsicCall("return", awst::WType::voidType(), _loc);
	returnOp->stackArgs.push_back(awst::makeTrue(_loc));
	_out.push_back(awst::makeExpressionStatement(std::move(returnOp), _loc));
	m_haltEmitted = true;
}

// ─── Statement translation ─────────────────────────────────────────────────


} // namespace puyasol::builder
