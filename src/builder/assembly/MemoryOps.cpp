/// @file MemoryOps.cpp
/// Memory operations: mload, mstore, handleReturn, tryHandleBytesMemoryRead.
/// Uses scratch-slot-backed bytes blob for EVM memory simulation.

#include "builder/assembly/AssemblyBuilder.h"
#include "builder/AwstShorthand.h"
#include "builder/abi/EvmAbiDecode.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/sol-types/TypeCoercion.h"
#include "awst/NameGen.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "Logger.h"

#include <sstream>
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>

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
// EVM memory spans the session-configured scratch slots, each
// SLOT_SIZE (4096) bytes. Offset → (slot = off/SLOT_SIZE, sub = off%SLOT_SIZE).
// All slots (including 0) read/write directly to scratch; no __evm_memory cache.
// 32-byte-aligned accesses never straddle a boundary (SLOT_SIZE % 32 == 0);
// unaligned straddling words are stitched/split across the two adjacent slots.

std::shared_ptr<awst::Statement> AssemblyBuilder::memBoundsAssert(
	ScratchLayout const& _scratch,
	std::shared_ptr<awst::Expression> _off, awst::SourceLocation const& _loc)
{
	// assert(off + 32 <= cap): spilling into non-memory scratch slots corrupts silently.
	uint64_t cap = static_cast<uint64_t>(SLOT_SIZE)
		* static_cast<uint64_t>(_scratch.memoryCount());
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
	if (slot >= memorySlotCount())
		Logger::instance().error("EVM memory read beyond the reserved scratch slots (raise --evm-memory-slots)", _loc);

	if (sub + 32 <= static_cast<uint64_t>(SLOT_SIZE))
		return awst::makeExtract3(awst::makeLoadSlot(memorySlotFirst() + slot, _loc),
			awst::makeIntegerConstant(sub, _loc), awst::makeIntegerConstant("32", _loc), _loc);

	// Straddles the slot boundary: tail of `slot` ++ head of `slot+1`.
	uint64_t firstLen = static_cast<uint64_t>(SLOT_SIZE) - sub;
	auto part1 = awst::makeExtract3(awst::makeLoadSlot(memorySlotFirst() + slot, _loc),
		awst::makeIntegerConstant(sub, _loc), awst::makeIntegerConstant(firstLen, _loc), _loc);
	auto part2 = awst::makeExtract3(awst::makeLoadSlot(memorySlotFirst() + slot + 1, _loc),
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
	if (slot >= memorySlotCount())
		Logger::instance().error("EVM memory write beyond the reserved scratch slots (raise --evm-memory-slots)", _loc);

	if (sub + 32 <= static_cast<uint64_t>(SLOT_SIZE))
	{
		auto rep = awst::makeReplace3(awst::makeLoadSlot(memorySlotFirst() + slot, _loc),
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
	auto rep1 = awst::makeReplace3(awst::makeLoadSlot(memorySlotFirst() + slot, _loc),
		awst::makeIntegerConstant(sub, _loc), std::move(valPart1), _loc);
	storeMemoryBlob(std::move(rep1), _loc, _out, slot);

	auto valPart2 = awst::makeExtract3(tmpRead(), awst::makeIntegerConstant(firstLen, _loc),
		awst::makeIntegerConstant(32 - firstLen, _loc), _loc);
	auto rep2 = awst::makeReplace3(awst::makeLoadSlot(memorySlotFirst() + slot + 1, _loc),
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
	m_pendingStatements.push_back(memBoundsAssert(scratchLayout(), off, _loc));
	// ONE path for every slot. Slot 0 is plain scratch since the __evm_memory
	// cache removal, so the old `off < SLOT_SIZE ? slot-0-fast : slow`
	// conditional selected between two IDENTICAL computations — paying an SE
	// fan-out, a compare, a branch and a duplicated extract on every dynamic
	// mload. Dyn is now Direct plus the bounds assert + eval-once wrapper.
	return readMemWordDirect(scratchLayout(), std::move(off), _loc);
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
	std::string offN = "__mem_dyn_off_" + std::to_string(
		awst::NameGen::next("MemoryOps.dynamicOffset"));
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(offN, awst::WType::uint64Type(), _loc),
		offsetToUint64(std::move(_offset), _loc), _loc));
	writeMemWordDirect(scratchLayout(),
		awst::makeVarExpression(offN, awst::WType::uint64Type(), _loc),
		std::move(_value32), _loc, _out);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::readMemStackRange(
	ScratchLayout const& _scratch,
	std::shared_ptr<awst::Expression> _offset,
	std::shared_ptr<awst::Expression> _length,
	awst::SourceLocation const& _loc)
{
	using O = awst::UInt64BinaryOperator;
	auto ss = [&]() { return awst::makeIntegerConstant(static_cast<uint64_t>(SLOT_SIZE), _loc); };
	// Both feed the slot/sub math AND both straddle arms — evaluate once.
	auto off = awst::makeEvalOnce(std::move(_offset), _loc);
	auto len = awst::makeEvalOnce(std::move(_length), _loc);

	auto loads = [&](int _slotDelta) {
		auto slot = awst::makeUInt64BinOp(off, O::FloorDiv, ss(), _loc);
		int base = _scratch.memoryFirst() + _slotDelta;
		if (base != 0)
			slot = awst::makeUInt64BinOp(std::move(slot), O::Add,
				awst::makeIntegerConstant(static_cast<uint64_t>(base), _loc), _loc);
		auto call = awst::makeIntrinsicCall("loads", awst::WType::bytesType(), _loc);
		call->stackArgs.push_back(std::move(slot));
		return call;
	};
	auto sub = [&]() { return awst::makeUInt64BinOp(off, O::Mod, ss(), _loc); };
	// Bytes available in the starting slot from `sub` onwards.
	auto avail = [&]() { return awst::makeUInt64BinOp(ss(), O::Sub, sub(), _loc); };

	// Straddling SLOT_SIZE is only reachable from an UNALIGNED offset (the
	// slot size is a multiple of 32), which is exactly what the range
	// word-loops produce: tail of this slot ++ head of the next, the same
	// stitch readMemWordConst does. The runtime path silently lacked it, so
	// extract3 ran off the end of the 4096-byte slot and panicked on code EVM
	// runs fine. A stack VALUE is at most one AVM element (SLOT_SIZE bytes),
	// so it can never span more than two slots.
	auto inSlot = awst::makeExtract3(loads(0), sub(), len, _loc);
	auto straddle = awst::makeConcat(
		awst::makeExtract3(loads(0), sub(), avail(), _loc),
		awst::makeExtract3(loads(1), awst::makeIntegerConstant("0", _loc),
			awst::makeUInt64BinOp(len, O::Sub, avail(), _loc), _loc),
		_loc);
	return awst::makeConditional(
		awst::makeNumericCompare(len, awst::NumericComparison::Lte, avail(), _loc),
		std::move(inSlot), std::move(straddle), awst::WType::bytesType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::readMemWordDirect(
	ScratchLayout const& _scratch,
	std::shared_ptr<awst::Expression> _offset, awst::SourceLocation const& _loc)
{
	return readMemStackRange(_scratch, std::move(_offset),
		awst::makeIntegerConstant("32", _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::readMemRangeDirect(
	ScratchLayout const& _scratch,
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
		auto word = readMemWordDirect(_scratch, std::move(wordOff), _loc);
		acc = acc ? awst::makeConcat(std::move(acc), std::move(word), _loc) : std::move(word);
	}
	// Trim to the exact byte length when not word-aligned.
	if (_byteLen % 32 != 0)
		acc = awst::makeExtract3(std::move(acc), awst::makeIntegerConstant("0", _loc),
			awst::makeIntegerConstant(static_cast<uint64_t>(_byteLen), _loc), _loc);
	return acc;
}

void AssemblyBuilder::writeMemBytesDirect(
	ScratchLayout const& _scratch,
	std::shared_ptr<awst::Expression> _offU64,
	std::shared_ptr<awst::Expression> _bytesValue,
	int _uniqueId,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	std::string pfx = "__blobw_" + std::to_string(_uniqueId) + "_";
	auto u64v = [&](std::string const& n) {
		return awst::makeVarExpression(pfx + n, awst::WType::uint64Type(), _loc);
	};
	auto bytesv = [&]() {
		return awst::makeVarExpression(pfx + "v", awst::WType::bytesType(), _loc);
	};
	// pin value + base offset + length; pad value to a whole word so the last
	// extract3 never runs off the end
	_out.push_back(awst::makeAssignmentStatement(
		bytesv(), awst::makeConcat(std::move(_bytesValue),
			awst::makeBzero(32, _loc), _loc), _loc));
	_out.push_back(awst::makeAssignmentStatement(
		u64v("off"), std::move(_offU64), _loc));
	auto lenExpr = awst::makeUInt64BinOp(
		awst::makeLen(bytesv(), _loc), awst::UInt64BinaryOperator::Sub,
		awst::makeIntegerConstant("32", _loc), _loc);
	_out.push_back(awst::makeAssignmentStatement(u64v("len"), std::move(lenExpr), _loc));
	_out.push_back(awst::makeAssignmentStatement(
		u64v("i"), awst::makeIntegerConstant("0", _loc), _loc));
	auto cond = awst::makeNumericCompare(u64v("i"),
		awst::NumericComparison::Lt, u64v("len"), _loc);
	auto body = awst::makeBlock(_loc);
	auto word = awst::makeExtract3(bytesv(), u64v("i"),
		awst::makeIntegerConstant("32", _loc), _loc);
	std::vector<std::shared_ptr<awst::Statement>> ws;
	writeMemWordDirect(_scratch,
		awst::makeUInt64BinOp(u64v("off"), awst::UInt64BinaryOperator::Add,
			u64v("i"), _loc),
		std::move(word), _loc, ws);
	for (auto& st: ws)
		body->body.push_back(std::move(st));
	body->body.push_back(awst::makeAssignmentStatement(u64v("i"),
		awst::makeUInt64BinOp(u64v("i"), awst::UInt64BinaryOperator::Add,
			awst::makeIntegerConstant("32", _loc), _loc), _loc));
	_out.push_back(awst::makeWhileLoop(std::move(cond), std::move(body), _loc));
}

void AssemblyBuilder::writeMemWordDirect(
	ScratchLayout const& _scratch,
	std::shared_ptr<awst::Expression> _offset, std::shared_ptr<awst::Expression> _value32,
	awst::SourceLocation const& _loc, std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	using O = awst::UInt64BinaryOperator;
	int id = awst::NameGen::next("MemoryOps.dynamicStore");
	auto ss = [&]() { return awst::makeIntegerConstant(static_cast<uint64_t>(SLOT_SIZE), _loc); };

	_out.push_back(memBoundsAssert(_scratch, _offset, _loc));

	std::string slotN = "__blobw_slot_" + std::to_string(id);
	std::string subN = "__blobw_sub_" + std::to_string(id);
	std::string valN = "__blobw_val_" + std::to_string(id);
	auto u64v = [&](std::string const& n) { return shorthand::u64Var(n, _loc); };
	auto valR = [&]() { return awst::makeVarExpression(valN, awst::WType::bytesType(), _loc); };

	auto physicalSlot = awst::makeUInt64BinOp(_offset, O::FloorDiv, ss(), _loc);
	if (_scratch.memoryFirst() != 0)
		physicalSlot = awst::makeUInt64BinOp(std::move(physicalSlot), O::Add,
			awst::makeIntegerConstant(
				static_cast<uint64_t>(_scratch.memoryFirst()), _loc), _loc);
	_out.push_back(awst::makeAssignmentStatement(
		u64v(slotN), std::move(physicalSlot), _loc));
	_out.push_back(awst::makeAssignmentStatement(
		u64v(subN),
		awst::makeUInt64BinOp(std::move(_offset), O::Mod, ss(), _loc), _loc));
	_out.push_back(awst::makeAssignmentStatement(valR(), std::move(_value32), _loc));

	auto storeSlot = [&](std::shared_ptr<awst::Expression> _slot,
		std::shared_ptr<awst::Expression> _at,
		std::shared_ptr<awst::Expression> _bytes) {
		auto slotOnce = awst::makeEvalOnce(std::move(_slot), _loc);
		auto loadsCall = awst::makeIntrinsicCall("loads", awst::WType::bytesType(), _loc);
		loadsCall->stackArgs.push_back(slotOnce);
		auto rep = awst::makeReplace3(std::move(loadsCall), std::move(_at),
			std::move(_bytes), _loc);
		auto storesCall = awst::makeIntrinsicCall("stores", awst::WType::voidType(), _loc);
		storesCall->stackArgs.push_back(slotOnce);
		storesCall->stackArgs.push_back(std::move(rep));
		return awst::makeExpressionStatement(std::move(storesCall), _loc);
	};

	// In-slot write, or — for an UNALIGNED offset within 31 bytes of the
	// SLOT_SIZE boundary — split across the two adjacent slots, mirroring
	// writeMemWordConst. Without the split, replace3 ran past the end of the
	// 4096-byte slot and panicked (the range word-loops reach here from
	// arbitrary offsets: mcopy, returndatacopy, precompile output).
	auto thenBlk = awst::makeBlock(_loc);
	thenBlk->body.push_back(storeSlot(u64v(slotN), u64v(subN), valR()));

	auto elseBlk = awst::makeBlock(_loc);
	{
		auto firstLen = [&]() {
			return awst::makeUInt64BinOp(ss(), O::Sub, u64v(subN), _loc);
		};
		elseBlk->body.push_back(storeSlot(u64v(slotN), u64v(subN),
			awst::makeExtract3(valR(), awst::makeIntegerConstant("0", _loc),
				firstLen(), _loc)));
		elseBlk->body.push_back(storeSlot(
			awst::makeUInt64BinOp(u64v(slotN), O::Add, awst::makeOne(_loc), _loc),
			awst::makeIntegerConstant("0", _loc),
			awst::makeExtract3(valR(), firstLen(),
				awst::makeUInt64BinOp(awst::makeIntegerConstant("32", _loc),
					O::Sub, firstLen(), _loc), _loc)));
	}

	_out.push_back(awst::makeIfElse(
		awst::makeNumericCompare(u64v(subN), awst::NumericComparison::Lte,
			awst::makeIntegerConstant(static_cast<uint64_t>(SLOT_SIZE - 32), _loc), _loc),
		std::move(thenBlk), std::move(elseBlk), _loc));
}

void AssemblyBuilder::writeMemByteDirect(
	ScratchLayout const& _scratch,
	std::shared_ptr<awst::Expression> _offset,
	std::shared_ptr<awst::Expression> _valueByte,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	using O = awst::UInt64BinaryOperator;
	auto offset = awst::makeEvalOnce(std::move(_offset), _loc);
	auto inBounds = awst::makeNumericCompare(offset,
		awst::NumericComparison::Lt,
		awst::makeIntegerConstant(static_cast<uint64_t>(
			_scratch.memoryCount()) * SLOT_SIZE, _loc), _loc);
	_out.push_back(awst::makeExpressionStatement(
		awst::makeAssert(std::move(inBounds), _loc,
			"EVM memory byte access exceeds configured scratch slots"), _loc));
	auto slot = awst::makeUInt64BinOp(offset, O::FloorDiv,
		awst::makeIntegerConstant(static_cast<uint64_t>(SLOT_SIZE), _loc), _loc);
	if (_scratch.memoryFirst() != 0)
		slot = awst::makeUInt64BinOp(std::move(slot), O::Add,
			awst::makeIntegerConstant(
				static_cast<uint64_t>(_scratch.memoryFirst()), _loc), _loc);
	auto sub = awst::makeUInt64BinOp(offset, O::Mod,
		awst::makeIntegerConstant(static_cast<uint64_t>(SLOT_SIZE), _loc), _loc);
	auto slotOnce = awst::makeEvalOnce(std::move(slot), _loc);
	auto loadsCall = awst::makeIntrinsicCall(
		"loads", awst::WType::bytesType(), _loc);
	loadsCall->stackArgs.push_back(slotOnce);
	auto replaced = awst::makeReplace3(
		std::move(loadsCall), std::move(sub), std::move(_valueByte), _loc);
	auto storesCall = awst::makeIntrinsicCall(
		"stores", awst::WType::voidType(), _loc);
	storesCall->stackArgs.push_back(slotOnce);
	storesCall->stackArgs.push_back(std::move(replaced));
	_out.push_back(awst::makeExpressionStatement(std::move(storesCall), _loc));
}

std::shared_ptr<awst::Expression> AssemblyBuilder::tryHandleBytesMemoryRead(
	solidity::yul::Expression const& _addrExpr,
	awst::SourceLocation const& _loc
)
{
	// mload of a bytes/string memory local's DATA region. Use the SAME matcher as
	// the write path (matchBytesMemoryDataPtr) so an asm mstore then mload of the
	// same `new bytes` buffer round-trips. Previously this only matched the nested
	// add(add(m,32),k) form, so a plain mload(add(m,32)) fell through to the
	// scratch-blob path — which the value-local mstore(add(m,32)) never wrote, so
	// the read returned 0 (ENS AddrResolver bytesToAddress(addressToBytes(a))).
	auto m = matchBytesMemoryDataPtr(_addrExpr, _loc);
	if (!m)
		return nullptr;
	Logger::instance().debug(
		"mload bytes memory read: guarded value-local read of " + m->name, _loc);
	return emitGuardedBytesDataRead(std::move(*m), _loc);
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
		// A BLOB-BACKED aggregate (`bytes memory b = new bytes(n)` whose pointer
		// escapes into asm) lives in the scratch blob addressed by its offset var,
		// NOT in a value local. Its value local is never initialised, so routing
		// mstore/mload here would read/write an uninitialised bytes local (len on a
		// uint64). Let it fall through to the generic scratch path, which resolves
		// the offset var so mstore and mload hit the SAME scratch bytes and an asm
		// write is visible to a later asm read.
		if (m_blobOffsetVars.count(name))
			return std::nullopt;
		return std::make_pair(name, it->second);
	};

	auto* addCall = std::get_if<solidity::yul::FunctionCall>(&_addr);
	if (!addCall || getFunctionName(addCall->functionName) != "add"
		|| addCall->arguments.size() != 2)
		return std::nullopt;

	// Case 1: add(m, O) / add(O, m). O may address the length header at runtime;
	// retain it as an absolute offset so no eager `O - 32` can underflow.
	for (int i = 0; i < 2; ++i)
		if (auto bl = asBytesLocal(addCall->arguments[i]))
		{
			auto c = resolveConstantYulValue(addCall->arguments[1 - i]);
			auto oExpr = offsetToUint64(buildExpression(addCall->arguments[1 - i]), _loc);
			if (c && *c >= 32)
				return BytesDataPtrMatch{bl->first, bl->second,
					awst::makeIntegerConstant(*c - 32, _loc), nullptr};
			return BytesDataPtrMatch{
				bl->first, bl->second, nullptr, std::move(oExpr)};
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
					offsetToUint64(buildExpression(addCall->arguments[1 - i]), _loc), nullptr};
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
	if (_m.absoluteOff)
	{
		// Split at the header boundary lazily. For O<32, operate on a virtual
		// `[length-word ++ data]` image, then project its possibly-updated length
		// and data back into the value local.
		std::string pfx = "__memhdr_"
			+ std::to_string(awst::NameGen::next("MemoryOps.headerOverlap"));
		auto u64v = [&](std::string const& suffix) {
			return awst::makeVarExpression(
				pfx + suffix, awst::WType::uint64Type(), _loc);
		};
		auto bytesv = [&](std::string const& suffix) {
			return awst::makeVarExpression(
				pfx + suffix, awst::WType::bytesType(), _loc);
		};
		auto varRef = [&]() {
			return awst::makeVarExpression(_m.name, _m.type, _loc);
		};
		_out.push_back(awst::makeAssignmentStatement(
			u64v("_off"), std::move(_m.absoluteOff), _loc));

		auto dataBlock = awst::makeBlock(_loc);
		BytesDataPtrMatch dataMatch{
			_m.name, _m.type,
			awst::makeUInt64BinOp(u64v("_off"), awst::UInt64BinaryOperator::Sub,
				awst::makeIntegerConstant(uint64_t{32}, _loc), _loc), nullptr};
		emitGuardedBytesDataWrite(
			std::move(dataMatch), _value32, _sliceLen, _loc, dataBlock->body);

		auto headerBlock = awst::makeBlock(_loc);
		auto header = awst::makeLeftPadToN(
			awst::makeItob(awst::makeLen(varRef(), _loc), _loc), 32, _loc);
		auto image = awst::makeConcat(std::move(header), varRef(), _loc);
		image = awst::makeConcat(
			std::move(image), awst::makeBzero(32, _loc), _loc);
		std::shared_ptr<awst::Expression> slice = _sliceLen == 1
			? std::shared_ptr<awst::Expression>(awst::makeExtract(
				_value32, 31, 1, _loc))
			: _value32;
		headerBlock->body.push_back(awst::makeAssignmentStatement(
			bytesv("_img"), awst::makeReplace3(
				std::move(image), u64v("_off"), std::move(slice), _loc), _loc));
		auto newLenWord = [&] {
			return awst::makeExtract(bytesv("_img"), 0, 32, _loc);
		};
		headerBlock->body.push_back(awst::makeExpressionStatement(
			awst::makeAssert(awst::makeNumericCompare(
				awst::makeAsBiguint(awst::makeExtract(newLenWord(), 0, 24, _loc), _loc),
				awst::NumericComparison::Eq,
				awst::makeIntegerConstant("0", _loc, awst::WType::biguintType()), _loc),
				_loc, "bytes memory length exceeds AVM range"), _loc));
		headerBlock->body.push_back(awst::makeAssignmentStatement(
			u64v("_len"), awst::makeBtoi(
				awst::makeExtract(newLenWord(), 24, 8, _loc), _loc), _loc));
		headerBlock->body.push_back(awst::makeAssignmentStatement(
			u64v("_avail"), awst::makeUInt64BinOp(
				awst::makeLen(bytesv("_img"), _loc),
				awst::UInt64BinaryOperator::Sub,
				awst::makeIntegerConstant(uint64_t{32}, _loc), _loc), _loc));
		auto dataAll = [&] {
			return awst::makeExtract3(bytesv("_img"),
				awst::makeIntegerConstant(uint64_t{32}, _loc), u64v("_avail"), _loc);
		};
		std::shared_ptr<awst::Expression> shrunk = awst::makeExtract3(
			dataAll(), awst::makeZero(_loc), u64v("_len"), _loc);
		std::shared_ptr<awst::Expression> grown = awst::makeConcat(
			dataAll(), awst::makeBzero(awst::makeUInt64BinOp(
				u64v("_len"), awst::UInt64BinaryOperator::Sub,
				u64v("_avail"), _loc), _loc), _loc);
		if (_m.type == awst::WType::stringType())
		{
			shrunk = awst::makeReinterpretCast(
				std::move(shrunk), _m.type, _loc);
			grown = awst::makeReinterpretCast(
				std::move(grown), _m.type, _loc);
		}
		headerBlock->body.push_back(awst::makeAssignmentStatement(
			varRef(), awst::makeConditional(
				awst::makeNumericCompare(u64v("_len"),
					awst::NumericComparison::Lte, u64v("_avail"), _loc),
				std::move(shrunk), std::move(grown), _m.type, _loc), _loc));

		_out.push_back(awst::makeIfElse(
			awst::makeNumericCompare(u64v("_off"),
				awst::NumericComparison::Gte,
				awst::makeIntegerConstant(uint64_t{32}, _loc), _loc),
			std::move(dataBlock), std::move(headerBlock), _loc));
		return;
	}
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

std::shared_ptr<awst::Expression> AssemblyBuilder::emitGuardedBytesDataRead(
	BytesDataPtrMatch _m,
	awst::SourceLocation const& _loc
)
{
	if (_m.absoluteOff)
	{
		auto absolute = awst::makeEvalOnce(
			std::move(_m.absoluteOff), _loc);
		auto varRef = [&]() {
			return awst::makeVarExpression(_m.name, _m.type, _loc);
		};
		auto header = awst::makeLeftPadToN(
			awst::makeItob(awst::makeLen(varRef(), _loc), _loc), 32, _loc);
		auto image = awst::makeConcat(std::move(header), varRef(), _loc);
		image = awst::makeConcat(
			std::move(image), awst::makeBzero(32, _loc), _loc);
		auto headerRead = awst::makeAsBiguint(
			awst::makeExtract3(std::move(image), absolute,
				awst::makeIntegerConstant(uint64_t{32}, _loc), _loc), _loc);
		BytesDataPtrMatch dataMatch{
			_m.name, _m.type,
			awst::makeUInt64BinOp(absolute, awst::UInt64BinaryOperator::Sub,
				awst::makeIntegerConstant(uint64_t{32}, _loc), _loc), nullptr};
		auto dataRead = emitGuardedBytesDataRead(
			std::move(dataMatch), _loc);
		return awst::makeConditional(
			awst::makeNumericCompare(absolute, awst::NumericComparison::Gte,
				awst::makeIntegerConstant(uint64_t{32}, _loc), _loc),
			std::move(dataRead), std::move(headerRead),
			awst::WType::biguintType(), _loc);
	}
	// Inverse of emitGuardedBytesDataWrite. The local is a VALUE (raw bytes, no
	// length header); read a 32-byte MSB-first word at data offset `off`. Clamp
	// so extract3 stays in-bounds even past the buffer end (EVM mload of fresh /
	// adjacent memory reads zeros): safeOff = min(off,len), wlen = min(32, len−safeOff);
	// the wlen real bytes sit in the high positions, the rest are zero.
	auto u64 = awst::WType::uint64Type();
	auto varRef = [&]() { return awst::makeVarExpression(_m.name, _m.type, _loc); };
	auto k32 = [&]() { return awst::makeIntegerConstant("32", _loc); };
	auto len = awst::makeEvalOnce(awst::makeLen(varRef(), _loc), _loc);
	auto off = awst::makeEvalOnce(std::move(_m.dataOff), _loc);
	auto safeOff = awst::makeEvalOnce(awst::makeConditional(
		awst::makeNumericCompare(off, awst::NumericComparison::Lt, len, _loc),
		off, len, u64, _loc), _loc);
	auto rem = awst::makeEvalOnce(
		awst::makeUInt64BinOp(len, awst::UInt64BinaryOperator::Sub, safeOff, _loc), _loc);
	auto wlen = awst::makeConditional(
		awst::makeNumericCompare(rem, awst::NumericComparison::Lt, k32(), _loc),
		rem, k32(), u64, _loc);
	auto slice = awst::makeExtract3(varRef(), safeOff, std::move(wlen), _loc);
	// Right-pad the ≤32 data bytes to a full word: extract3(slice ++ bzero(32), 0, 32).
	auto word = awst::makeExtract3(
		awst::makeConcat(std::move(slice), awst::makeBzero(32, _loc), _loc),
		awst::makeIntegerConstant("0", _loc), k32(), _loc);
	return awst::makeAsBiguint(std::move(word), _loc);
}

bool AssemblyBuilder::tryHandleBytesMemoryLengthWrite(
	solidity::yul::FunctionCall const& _call,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// mstore(<bare bytes/string local>, n): the address is the buffer POINTER
	// itself, so EVM writes the LENGTH WORD — it resizes the buffer in place.
	// The OZ ShortStrings.toString idiom does exactly this:
	//     string memory str = new string(32);
	//     mstore(str, len); mstore(add(str, 0x20), sstr)
	// Only the add(...) form was matched, so a bare pointer fell through to the
	// generic path and hard-errored coercing a `string` to biguint.
	//
	// In the value model the local IS the raw bytes with no length header, so
	// "set the length" is a resize:  n <= len(m) ? extract3(m,0,n)
	//                                            : m ++ bzero(n − len(m))
	// Growing zero-fills; EVM would expose adjacent memory, which has no AVM
	// analogue and is not observable in the idioms this serves.
	if (_call.arguments.size() != 2)
		return false;
	auto* id = std::get_if<solidity::yul::Identifier>(&_call.arguments[0]);
	if (!id)
		return false;
	std::string name = resolveVarRef(*id);
	auto it = m_locals.find(name);
	if (it == m_locals.end())
		return false;
	if (it->second != awst::WType::bytesType() && it->second != awst::WType::stringType())
		return false;
	if (m_blobOffsetVars.count(name))
		return false;                 // blob-backed: generic scratch path owns it

	auto* ty = it->second;
	auto varRef = [&]() { return awst::makeVarExpression(name, ty, _loc); };
	auto n = awst::makeEvalOnce(
		offsetToUint64(buildExpression(_call.arguments[1]), _loc), _loc);
	auto curLen = awst::makeEvalOnce(awst::makeLen(varRef(), _loc), _loc);
	drainPendingStatements(_out);

	std::shared_ptr<awst::Expression> shrunk =
		awst::makeExtract3(varRef(), awst::makeZero(_loc), n, _loc);
	std::shared_ptr<awst::Expression> grown = awst::makeConcat(
		varRef(),
		awst::makeBzero(
			awst::makeUInt64BinOp(n, awst::UInt64BinaryOperator::Sub, curLen, _loc), _loc),
		_loc);
	if (ty == awst::WType::stringType())
	{
		shrunk = awst::makeReinterpretCast(std::move(shrunk), awst::WType::stringType(), _loc);
		grown = awst::makeReinterpretCast(std::move(grown), awst::WType::stringType(), _loc);
	}
	auto resized = awst::makeConditional(
		awst::makeNumericCompare(n, awst::NumericComparison::Lt, curLen, _loc),
		std::move(shrunk), std::move(grown), ty, _loc);
	_out.push_back(awst::makeAssignmentStatement(varRef(), std::move(resized), _loc));
	Logger::instance().debug("mstore length-word write: resized bytes local " + name, _loc);
	return true;
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
	if (tryHandleBytesMemoryLengthWrite(_call, _loc, _out))
		return true;
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
	// BLOB-backed vars (--evm-memory-layout, asm-touched) hold a uint64
	// offset; the real data lives in the blob. This value-model fast path
	// would replace3 a DETACHED copy while every other reader (returns,
	// mloads) uses the blob — the writes silently vanished
	// (mcopy_to_right_overlap returned the original bytes). Fall through to
	// the generic word-copy, which goes through mload/mstore and is
	// memmove-safe via the M13 source-word snapshot.
	if (m_blobOffsetVars.count(dstVar) || m_blobOffsetVars.count(srcVar))
		return false;
	if (dstIt->second != awst::WType::bytesType() && dstIt->second != awst::WType::stringType())
		return false;
	if (srcIt->second != awst::WType::bytesType() && srcIt->second != awst::WType::stringType())
		return false;

	Logger::instance().debug(
		"mcopy bytes memory: replace3(" + dstVar + ", dstOff, extract3(" + srcVar + ", srcOff, len))", _loc);

	// Translate offsets and length from Yul; pinned — each is referenced
	// several times in the guards below.
	auto dstOffExpr = awst::makeEvalOnce(offsetToUint64(buildExpression(*dstOffYul), _loc), _loc);
	auto srcOffExpr = awst::makeEvalOnce(offsetToUint64(buildExpression(*srcOffYul), _loc), _loc);
	auto lenExpr    = awst::makeEvalOnce(offsetToUint64(buildExpression(_call.arguments[2]), _loc), _loc);

	auto srcRef = [&]() { return awst::makeVarExpression(srcVar, srcIt->second, _loc); };
	auto dstRef = [&]() { return awst::makeVarExpression(dstVar, dstIt->second, _loc); };
	auto minU64 = [&](std::shared_ptr<awst::Expression> a, std::shared_ptr<awst::Expression> b) {
		return awst::makeConditional(
			awst::makeNumericCompare(a, awst::NumericComparison::Lt, b, _loc),
			a, b, awst::WType::uint64Type(), _loc);
	};

	// GUARDED copy (M13, matching the mstore siblings): reads past the src
	// end yield zeros (src ++ bzero(len), clamped start); the write is
	// truncated to the dst capacity instead of failing replace3. Overlap
	// (dst == src) is inherently safe: the slice materialises before the write.
	auto safeSrcOff = minU64(srcOffExpr, awst::makeLen(srcRef(), _loc));
	auto srcPadded  = awst::makeConcat(srcRef(), awst::makeBzero(lenExpr, _loc), _loc);
	auto srcBytes   = awst::makeExtract3(std::move(srcPadded), std::move(safeSrcOff), lenExpr, _loc);

	auto safeDstOff = awst::makeEvalOnce(minU64(dstOffExpr, awst::makeLen(dstRef(), _loc)), _loc);
	auto avail = awst::makeUInt64BinOp(awst::makeLen(dstRef(), _loc),
		awst::UInt64BinaryOperator::Sub, safeDstOff, _loc);
	auto effLen = minU64(lenExpr, std::move(avail));
	auto value = awst::makeExtract3(std::move(srcBytes),
		awst::makeIntegerConstant("0", _loc), std::move(effLen), _loc);

	auto replaced = awst::makeReplace3(dstRef(), safeDstOff, std::move(value), _loc);
	auto assign = awst::makeAssignmentStatement(dstRef(), std::move(replaced), _loc);
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

	// Write the low 8 bits of value as one byte at memory[ptr]. The byte never
	// straddles a slot, so route through the SAME runtime slot math the
	// slot-aware `mstore` uses (offset -> slot=off/SLOT_SIZE, sub=off%SLOT_SIZE)
	// instead of the old slot-0-only replace3, which panicked / mis-wrote for
	// any offset >= 4096 in an --evm-memory-slots contract (FMP reaches ~18KB).
	auto offsetU64 = awst::makeEvalOnce(offsetToUint64(_args[0], _loc), _loc);
	_out.push_back(memBoundsAssert(scratchLayout(), offsetU64, _loc));
	auto padded = padTo32Bytes(ensureBiguint(_args[1], _loc), _loc);
	auto lowByte = awst::makeExtract3(
		std::move(padded), awst::makeIntegerConstant("31", _loc), awst::makeOne(_loc), _loc);
	auto ss = [&]() { return awst::makeIntegerConstant(static_cast<uint64_t>(SLOT_SIZE), _loc); };
	std::string slotN = "__mstore8_slot_"
		+ std::to_string(awst::NameGen::next("MemoryOps.mstore8Slot"));
	_out.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(slotN, awst::WType::uint64Type(), _loc),
		awst::makeUInt64BinOp(offsetU64, awst::UInt64BinaryOperator::FloorDiv, ss(), _loc), _loc));
	auto slotR = [&]() { return awst::makeVarExpression(slotN, awst::WType::uint64Type(), _loc); };
	auto sub = awst::makeUInt64BinOp(std::move(offsetU64), awst::UInt64BinaryOperator::Mod, ss(), _loc);
	auto loadsCall = awst::makeIntrinsicCall("loads", awst::WType::bytesType(), _loc);
	loadsCall->stackArgs.push_back(slotR());
	auto rep = awst::makeReplace3(std::move(loadsCall), std::move(sub), std::move(lowByte), _loc);
	auto storesCall = awst::makeIntrinsicCall("stores", awst::WType::voidType(), _loc);
	storesCall->stackArgs.push_back(slotR());
	storesCall->stackArgs.push_back(std::move(rep));
	_out.push_back(awst::makeExpressionStatement(std::move(storesCall), _loc));
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

		if (returnSize && *returnSize == 0)
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

		// RUNTIME offset/size are as good as constants here: the payload is
		// extract3(blob, off, len) either way. This used to fall into the
		// bare-exit arm above, which SILENTLY DROPPED a runtime-sized payload —
		// exactly the shape of a tape-playing fallback
		// (`return(add(b, 32), mload(b))`), whose answers are variable-width.
		Logger::instance().warning(
			"assembly return() in void function — emitting "
			+ (returnSize ? std::to_string(*returnSize) + " bytes" : std::string("runtime-sized payload"))
			+ " as structured log", _loc
		);

		// Read the return region from the memory blob: extract3(blob, offset, size)
		auto offsetU64 = returnOffset
			? std::shared_ptr<awst::Expression>(awst::makeIntegerConstant(*returnOffset, _loc))
			: offsetToUint64(_args[0], _loc);

		auto sizeU64 = returnSize
			? std::shared_ptr<awst::Expression>(awst::makeIntegerConstant(*returnSize, _loc))
			: offsetToUint64(_args[1], _loc);

		// Slot-routed: the payload region is at the runtime FMP, so the old
		// slot-0 extract logged the wrong bytes (or panicked) for any
		// --evm-memory-slots contract whose buffer passed 4096.
		auto extract = readMemRangeDyn(
			std::move(offsetU64), std::move(sizeU64), _loc, _out);
		// log(0x151f7c75 ++ data): on EVM the return() payload IS the
		// returndata the caller sees, and on AVM every consumer of a call's
		// result — the typed caller-decode (extract 4,N past the prefix), the
		// low-level returndata capture, algosdk's ATC — reads the LAST LOG in
		// the ARC4 return convention. A bare log made the payload invisible to
		// all of them: the stand-in's `fallback { return(0,0x20) }` answered
		// 32 raw bytes, the caller's decode wanted 36, and CoWSwapEthFlow's
		// ctor died on the resulting wrong-width address.
		auto prefixed = awst::makeConcat(
			awst::makeBytesConstant({0x15, 0x1f, 0x7c, 0x75}, _loc),
			std::move(extract), _loc);
		auto logCall = awst::makeIntrinsicCall("log", awst::WType::voidType(), _loc);
		logCall->stackArgs.push_back(std::move(prefixed));

		auto logStmt = awst::makeExpressionStatement(std::move(logCall), _loc);
		_out.push_back(std::move(logStmt));

		flushMemoryToScratch(_loc, _out);
		if (m_frameIsProgram)
		{
			// Program frame (internal/private/fallback/receive): EVM return()
			// ends the whole call — halt so the answer log above stays the
			// LAST log (the return-carrier the caller decodes). A subroutine
			// return here let the router append its empty void carrier after.
			auto returnOp = awst::makeIntrinsicCall(
				"return", awst::WType::voidType(), _loc);
			returnOp->stackArgs.push_back(awst::makeTrue(_loc));
			_out.push_back(awst::makeExpressionStatement(std::move(returnOp), _loc));
			m_haltEmitted = true;
			return;
		}
		auto ret = awst::makeReturnStatement(nullptr, _loc);
		_out.push_back(std::move(ret));
		return;
	}

	// A runtime return region is standard EVM ABI. Decode it through the same
	// recursive type-directed codec used by abi.decode: solc supplies every
	// head stride/dynamic fact, so bytes32[], narrow arrays, nested arrays,
	// structs, and tuples do not need separate assembly-return branches.
	if (!m_frameIsProgram && !m_returnSolTypes.empty() && m_returnType
		&& abi::canDecodeEvmAbi(m_returnSolTypes)
		&& (m_returnSolTypes.size() == 1
			? m_returnType->kind() != awst::WTypeKind::WTuple
			: (dynamic_cast<awst::WTuple const*>(m_returnType)
				&& static_cast<awst::WTuple const*>(m_returnType)->types().size()
					== m_returnSolTypes.size())))
	{
		auto region = readMemRangeDyn(
			offsetToUint64(_args[0], _loc),
			offsetToUint64(_args[1], _loc), _loc, _out);
		auto returnValue = abi::decodeEvmAbi(
			m_typeMapper, std::move(region), m_returnSolTypes,
			m_returnType, _loc, _out);
		if (m_returnSolTypes.size() == 1
			&& returnValue && returnValue->wtype != m_returnType)
		{
			auto const* solType = m_returnSolTypes[0];
			auto kind = m_returnType->kind();
			if (kind == awst::WTypeKind::ARC4Struct
				|| kind == awst::WTypeKind::ARC4StaticArray
				|| kind == awst::WTypeKind::ARC4DynamicArray)
				returnValue = codec::valueToArc4(m_typeMapper, solType,
					std::move(returnValue), m_returnType, _loc);
			else if (m_returnType == awst::WType::biguintType())
				returnValue = awst::makeAsBiguint(codec::valueToEvmWord(
					m_typeMapper, solType, std::move(returnValue), _loc), _loc);
			else
				returnValue = TypeCoercion::coerceForAssignment(
					std::move(returnValue), m_returnType, _loc);
		}

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
	bool const alreadyArc4 = isArc4EncodedType(arc4Value->wtype);
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
