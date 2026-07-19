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

	// Constant offset: check calldata map first; fall back to scratch slot.
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
		return awst::makeAsBiguint(readMemWordConst(*constOffset, _loc), _loc);
	}

	return awst::makeAsBiguint(readMemWordDyn(_args[0], _loc), _loc);
}

// ── Multi-slot memory word access ───────────────────────────────────────────
// EVM memory spans scratch slots [MEMORY_SLOT_FIRST, MEMORY_SLOT_LAST], each
// SLOT_SIZE (4096) bytes. Offset → (slot = off/SLOT_SIZE, sub = off%SLOT_SIZE).
// All slots (including 0) read/write directly to scratch; no __evm_memory cache.
// 32-byte-aligned accesses never straddle a boundary (SLOT_SIZE % 32 == 0);
// unaligned straddling words are stitched/split across the two adjacent slots.

std::shared_ptr<awst::Statement> AssemblyBuilder::memBoundsAssert(
	std::shared_ptr<awst::Expression> _off, awst::SourceLocation const& _loc)
{
	// assert(off + 32 <= cap): spilling into non-memory scratch slots corrupts silently.
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

	// No slot-0 special case: slot 0 is plain scratch (no local cache), and
	// the old fast path also SKIPPED the straddle stitch for offsets in
	// [SLOT_SIZE-31, SLOT_SIZE) — the general paths below cover both.
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

	// No slot-0 special case (see readMemWordConst) — the general in-slot and
	// straddle paths below cover slot 0 as plain scratch.
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
	// Materialize once: offset appears in the bounds-assert + slot/sub (~3 refs);
	// a side-effecting mload(q) would otherwise re-run each time (makeEvalOnce =
	// OperandPlan primitive; a var/constant offset is duplicated as-is).
	off = awst::makeEvalOnce(std::move(off), _loc);
	m_pendingStatements.push_back(memBoundsAssert(off, _loc));
	// ONE path for every slot. Slot 0 is plain scratch since the __evm_memory
	// cache removal, so the old `off < SLOT_SIZE ? slot-0-fast : slow`
	// conditional selected between two IDENTICAL computations — paying an SE
	// fan-out, a compare, a branch and a duplicated extract on every dynamic
	// mload. Dyn is now Direct plus the bounds assert + eval-once wrapper.
	return readMemWordDirect(std::move(off), _loc);
}

void AssemblyBuilder::writeMemWordDyn(
	std::shared_ptr<awst::Expression> _offset, std::shared_ptr<awst::Expression> _value32,
	awst::SourceLocation const& _loc, std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	// ONE path for every slot (see readMemWordDyn): the old slot-0-fast /
	// slow if/else duplicated the whole write for two identical outcomes.
	// writeMemWordDirect materialises slot+value and bounds-asserts; only the
	// offset needs pinning here so its side effects run once across the
	// assert + slot + sub references.
	static int s_ctr = 0;
	std::string offN = "__mem_dyn_off_" + std::to_string(s_ctr++);
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(offN, awst::WType::uint64Type(), _loc),
		offsetToUint64(std::move(_offset), _loc), _loc));
	writeMemWordDirect(
		awst::makeVarExpression(offN, awst::WType::uint64Type(), _loc),
		std::move(_value32), _loc, _out);
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
	// Concat ceil(_byteLen/32) successive words; each re-derives its slot
	// so straddling SLOT_SIZE is handled transparently.
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
	// Match: mload(add(add(bytes_param, 32), offset)) or add commuted.
	// EVM: data_ptr+32 (skip length header)+offset → mload.
	// AVM: extract3(data, offset, 32) — bytes have no length header.

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

	std::string paramName = resolveVarRef(*paramId);
	auto paramIt = m_locals.find(paramName);
	if (paramIt == m_locals.end())
		return nullptr;

	auto* paramType = paramIt->second;
	if (paramType != awst::WType::bytesType() && paramType != awst::WType::stringType())
		return nullptr;

	Logger::instance().debug(
		"mload bytes memory read: extract3(" + paramName + ", offset, 32)", _loc
	);

	auto offsetExpr = buildExpression(*offsetExprYul);
	auto offsetU64 = awst::makeBtoi(awst::makeAsBytes(offsetExpr, _loc), _loc);
	auto extract = awst::makeExtract3(
		awst::makeVarExpression(paramName, paramType, _loc),
		std::move(offsetU64), awst::makeIntegerConstant("32", _loc), _loc);
	return awst::makeAsBiguint(std::move(extract), _loc);
}

std::optional<AssemblyBuilder::BytesDataPtrMatch> AssemblyBuilder::matchBytesMemoryDataPtr(
	solidity::yul::Expression const& _addr,
	awst::SourceLocation const& _loc
)
{
	auto asBytesLocal = [&](solidity::yul::Expression const& e)
		-> std::optional<std::pair<std::string, awst::WType const*>>
	{
		auto* id = std::get_if<solidity::yul::Identifier>(&e);
		if (!id)
			return std::nullopt;
		std::string name = resolveVarRef(*id);
		auto it = m_locals.find(name);
		if (it == m_locals.end())
			return std::nullopt;
		if (it->second != awst::WType::bytesType() && it->second != awst::WType::stringType())
			return std::nullopt;
		return std::make_pair(name, it->second);
	};

	auto* addCall = std::get_if<solidity::yul::FunctionCall>(&_addr);
	if (!addCall || getFunctionName(addCall->functionName) != "add"
		|| addCall->arguments.size() != 2)
		return std::nullopt;

	// Case 1: add(m, O) / add(O, m) → data offset = O − 32 (O skips the length
	// word). A CONSTANT O < 32 targets the length word itself — not a data write,
	// leave it to the generic path (old behaviour).
	for (int i = 0; i < 2; ++i)
		if (auto bl = asBytesLocal(addCall->arguments[i]))
		{
			auto c = resolveConstantYulValue(addCall->arguments[1 - i]);
			if (c && *c < 32)
				return std::nullopt;
			auto oExpr = offsetToUint64(buildExpression(addCall->arguments[1 - i]), _loc);
			auto off = awst::makeUInt64BinOp(std::move(oExpr), awst::UInt64BinaryOperator::Sub,
				awst::makeIntegerConstant("32", _loc), _loc);
			return BytesDataPtrMatch{bl->first, bl->second, std::move(off)};
		}

	// Case 2: add(add(m, 32), k) / commuted → data offset = k.
	for (int i = 0; i < 2; ++i)
	{
		auto* inner = std::get_if<solidity::yul::FunctionCall>(&addCall->arguments[i]);
		if (!inner || getFunctionName(inner->functionName) != "add" || inner->arguments.size() != 2)
			continue;
		for (int j = 0; j < 2; ++j)
		{
			auto bl = asBytesLocal(inner->arguments[j]);
			auto c = resolveConstantYulValue(inner->arguments[1 - j]);
			if (bl && c && *c == 32)
				return BytesDataPtrMatch{bl->first, bl->second,
					offsetToUint64(buildExpression(addCall->arguments[1 - i]), _loc)};
		}
	}

	return std::nullopt;
}

void AssemblyBuilder::emitGuardedBytesDataWrite(
	BytesDataPtrMatch _m,
	std::shared_ptr<awst::Expression> _value32,
	int _sliceLen,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// The local stays a VALUE (raw bytes, no length header); the generic path
	// would `b+` on that value and revert past 64 bytes (AVM bigint-op operand
	// limit). Write in place instead:
	//   m = (off < len(m)) ? replace3(m, off, slice) : m
	// slice = the value's low byte (mstore8) or its leading min(32, len−off)
	// bytes (mstore; EVM words are MSB-first, data[off] gets the top byte).
	// Bytes at/past len are EVM padding / adjacent memory that a length-bounded
	// copy never observes — dropped, not an error.
	auto varRef = [&]() { return awst::makeVarExpression(_m.name, _m.type, _loc); };
	auto len = [&]() { return awst::makeLen(varRef(), _loc); };
	// off is referenced by the guard + replace3 (+ rem); eval-once, guard-first.
	auto off = awst::makeEvalOnce(std::move(_m.dataOff), _loc);

	std::shared_ptr<awst::Expression> slice;
	if (_sliceLen == 1)
		slice = awst::makeExtract3(std::move(_value32),
			awst::makeIntegerConstant("31", _loc), awst::makeOne(_loc), _loc);
	else
	{
		// wlen = min(32, len − off); the sub only evaluates in the off < len branch.
		auto rem = awst::makeEvalOnce(
			awst::makeUInt64BinOp(len(), awst::UInt64BinaryOperator::Sub, off, _loc), _loc);
		auto wlen = awst::makeConditional(
			awst::makeNumericCompare(rem, awst::NumericComparison::Lt,
				awst::makeIntegerConstant("32", _loc), _loc),
			rem, awst::makeIntegerConstant("32", _loc), awst::WType::uint64Type(), _loc);
		slice = awst::makeExtract3(std::move(_value32), awst::makeZero(_loc), std::move(wlen), _loc);
	}

	std::shared_ptr<awst::Expression> written =
		awst::makeReplace3(varRef(), off, std::move(slice), _loc);
	if (_m.type == awst::WType::stringType())
		written = awst::makeReinterpretCast(std::move(written), awst::WType::stringType(), _loc);
	auto guard = awst::makeNumericCompare(off, awst::NumericComparison::Lt, len(), _loc);
	auto merged = awst::makeConditional(
		std::move(guard), std::move(written), varRef(), _m.type, _loc);
	_out.push_back(awst::makeAssignmentStatement(varRef(), std::move(merged), _loc));
}

bool AssemblyBuilder::tryHandleBytesMemoryWrite(
	solidity::yul::FunctionCall const& _call,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// mstore(<data ptr>, value): 32-byte word write into a bytes/string memory
	// local at any data offset (see emitGuardedBytesDataWrite for semantics).
	if (_call.arguments.size() != 2)
		return false;
	auto m = matchBytesMemoryDataPtr(_call.arguments[0], _loc);
	if (!m)
		return false;
	auto padded = padTo32Bytes(ensureBiguint(buildExpression(_call.arguments[1]), _loc), _loc);
	// Side effects in the offset / value (e.g. an mload) land as pending; drain
	// them before the materialisation + write statements.
	drainPendingStatements(_out);
	emitGuardedBytesDataWrite(std::move(*m), std::move(padded), 32, _loc, _out);
	return true;
}

bool AssemblyBuilder::tryHandleBytesMemoryWrite8(
	solidity::yul::FunctionCall const& _call,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// mstore8(<data ptr>, value): one-byte write (the value's LOW byte) into a
	// bytes/string memory local at any data offset.
	if (_call.arguments.size() != 2)
		return false;
	auto m = matchBytesMemoryDataPtr(_call.arguments[0], _loc);
	if (!m)
		return false;
	auto padded = padTo32Bytes(ensureBiguint(buildExpression(_call.arguments[1]), _loc), _loc);
	drainPendingStatements(_out);
	emitGuardedBytesDataWrite(std::move(*m), std::move(padded), 1, _loc, _out);
	return true;
}

// ── tryHandleBytesMemoryMcopy ──────────────────────────────────────────────
// Matches: mcopy(add(add(bytes_var, 0x20), dstOff), add(add(src_var, 0x20), srcOff), len)
// EVM: add(x,0x20) skips the 32-byte length header; data[off] = add(x,0x20)+off.
// AVM: x is raw bytes (no header), so data[off] = off directly.
// Translation: dst_var = replace3(dst_var, dstOff, extract3(src_var, srcOff, len)).
// Handles same- and cross-variable copies.

bool AssemblyBuilder::tryHandleBytesMemoryMcopy(
	solidity::yul::FunctionCall const& _call,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (_call.arguments.size() != 3)
		return false;

	// Match add(add(bytes_var, 0x20), dynOffset): returns {varName, dynOffset*} or {"",nullptr}.
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
				return {resolveVarRef(*varId), dynOff};
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

	// Track constant store values (e.g. FMP init at 0x40) for resolveConstantOffset.
	// A non-constant value at a constant offset KILLS that offset's entry; a
	// non-constant offset kills all content entries (could clobber any of them).
	auto constOffset = resolveConstantOffset(_args[0]);
	if (constOffset)
	{
		std::string varName = "mem_0x" + ([&] {
			std::ostringstream oss;
			oss << std::hex << *constOffset;
			return oss.str();
		})();
		auto storedVal = resolveConstantOffset(_args[1]);
		if (storedVal)
			m_localConstants[varName] = *storedVal;
		else
			m_localConstants.erase(varName);
	}
	else
		invalidateMemConstants();

	m_lastMstoreValue = _args[1];

	auto padded = padTo32Bytes(ensureBiguint(_args[1], _loc), _loc);

	if (constOffset)
	{
		writeMemWordConst(*constOffset, std::move(padded), _loc, _out);
		return;
	}
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

	// Write the low 8 bits of value as one byte at memory[ptr].
	// pad32(value)[31] = the low byte; replace3 one byte at the offset.
	auto offsetU64 = offsetToUint64(_args[0], _loc);
	auto padded = padTo32Bytes(ensureBiguint(_args[1], _loc), _loc);
	auto lowByte = awst::makeExtract3(
		std::move(padded), awst::makeIntegerConstant("31", _loc), awst::makeOne(_loc), _loc);
	assignMemoryVar(
		awst::makeReplace3(memoryVar(_loc), std::move(offsetU64), std::move(lowByte), _loc), _loc, _out);
}

void AssemblyBuilder::handleReturn(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (!checkArity(_args, 2, "return", _loc))
		return;

	// return(offset, size): EVM pattern bypassing ABI encoding.
	// Void function: emit data as structured log so callers read it from logs.
	if (!m_returnType || m_returnType == awst::WType::voidType())
	{
		auto returnOffset = resolveConstantOffset(_args[0]);
		auto returnSize = resolveConstantOffset(_args[1]);

		if (!returnOffset || !returnSize || *returnSize == 0)
		{
			// return(_, 0) → unconditional program-exit via AVM `return 1`.
			// Needed for Yul helpers using EVM `return` as a hard exit inside a nested call.
			flushMemoryToScratch(_loc, _out);
			auto returnOp = awst::makeIntrinsicCall("return", awst::WType::voidType(), _loc);
			returnOp->stackArgs.push_back(awst::makeTrue(_loc));
			_out.push_back(awst::makeExpressionStatement(std::move(returnOp), _loc));
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

		if (m_frameIsProgram)
		{
			emitArc4ReturnHalt(std::move(returnValue), _loc, _out);
			return;
		}
		flushMemoryToScratch(_loc, _out);
		_out.push_back(awst::makeReturnStatement(std::move(returnValue), _loc));
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

	if (m_frameIsProgram)
	{
		emitArc4ReturnHalt(std::move(returnValue), _loc, _out);
		return;
	}
	// Public/external frame: EVM return() ends this frame only — callers
	// (router or `this.f()` callsub) continue. Plain subroutine return.
	flushMemoryToScratch(_loc, _out);
	_out.push_back(awst::makeReturnStatement(std::move(returnValue), _loc));
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
