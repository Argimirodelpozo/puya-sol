#include "builder/contract/ContractBuilder.h"
#include "builder/contract/StorageDispatchSupport.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/StorageRuntimePlan.h"
#include "builder/storage/SlotWordCodec.h"
#include "builder/storage/SlotHandleAccess.h"

#include <libsolidity/ast/Types.h>
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <algorithm>
#include <set>

namespace puyasol::builder
{
void ContractBuilder::buildEvmSlotStorageDispatch(
	StorageRuntimePlan const& _storagePlan,
	awst::Contract* _contractNode,
	std::string const& _contractName
)
{
	auto const& layout = _storagePlan.solidityLayout;

	// Dense-only: every runtime slot is provably < 2^16 — no mapping / dynamic
	// array / bytes / string anywhere in the persistent layout (their slots are
	// keccak-derived) and no inline assembly (arbitrary computed slots). The
	// sparse "s:" arms and the mod-2^256 slot wrap are then dead weight — a
	// scalar-only contract pays a few hundred bytes of unreachable code
	// (pushed external_call_signed_narrow_return's child-embed over the 8KB cap).
	// UNIT-GLOBAL, not per-contract: the runtime subroutines share one
	// SubroutineID across the whole unit, so variant bodies clobber each other
	// (AWSTBuilder pre-scans and sets the flags before any contract builds).
	bool const denseOnly = m_typeMapper.profile().denseOnlyStorage;

	std::string cref = m_sourceFile + "." + _contractName;
	awst::SourceLocation loc;
	loc.file = m_sourceFile;

	auto makeUint64 = [&](std::string const& val) {
		return awst::makeIntegerConstant(val, loc);
	};
	auto makeBytes = [&](std::string const& s) {
		return awst::makeUtf8BytesConstant(s, loc);
	};
	auto slotVar = [&]() {
		return awst::makeVarExpression("__slot", awst::WType::biguintType(), loc);
	};

	// EVM slot arithmetic wraps mod 2^256 (see buildStorageDispatch).
	auto makeSlotWrapStmt = [&]() {
		auto wrapped = awst::makeBigUIntBinOp(
			slotVar(),
			awst::BigUIntBinaryOperator::Mod,
			awst::makeBiguintConstant(
				"115792089237316195423570985008687907853269984665640564039457584007913129639936",
				loc),
			loc);
		return awst::makeAssignmentStatement(slotVar(), std::move(wrapped), loc);
	};

	// __slot < 2^16 → dense region (declared vars), page boxes of 64 slots.
	auto denseCmp = [&]() {
		return awst::makeNumericCompare(slotVar(), awst::NumericComparison::Lt,
			awst::makeIntegerConstant(std::to_string(kEvmDenseSlotLimit), loc,
				awst::WType::biguintType()), loc);
	};

	// Bind the uint64 slot, then key = "p:" ++ itob(slot / 64), off = (slot % 64) * 32.
	// Dense-only, single-page layouts (≤64 slots): key is the constant page-0
	// name, offset drops the mod, and the biguint→u64 cast needs no 8-byte
	// normalisation (slot fits 2 bytes; btoi accepts ≤8). Unit-global like
	// denseOnly (same shared-SubroutineID hazard).
	bool const singlePage = denseOnly
		&& m_typeMapper.profile().singlePageStorage;
	std::string const s64Name = "__eslot64";
	auto s64Var = [&]() {
		return awst::makeVarExpression(s64Name, awst::WType::uint64Type(), loc);
	};
	auto bindS64 = [&](awst::Block& _blk) {
		// ALWAYS 8-byte-normalise before btoi: a biguint's byte length is not
		// bounded by its VALUE — slot args arrive 32-byte-padded from the
		// getter path (leftPadToN canonicals), and btoi rejects >8 bytes.
		auto cast = awst::makeAsBytes(slotVar(), loc);
		auto cat = awst::makeLeftPad(std::move(cast), 8, loc);
		auto extract = awst::makeExtractLastN(std::move(cat), 8, loc);
		_blk.body.push_back(awst::makeAssignmentStatement(
			s64Var(), awst::makeBtoi(std::move(extract), loc), loc));
	};
	auto pageKey = [&]() -> std::shared_ptr<awst::Expression> {
		if (singlePage)
			return awst::makeConcat(makeBytes("p:"),
				awst::makeItob(makeUint64("0"), loc), loc);
		auto page = awst::makeUInt64BinOp(s64Var(),
			awst::UInt64BinaryOperator::FloorDiv,
			makeUint64(std::to_string(kEvmSlotsPerPage)), loc);
		return awst::makeConcat(makeBytes("p:"),
			awst::makeItob(std::move(page), loc), loc);
	};
	auto pageOff = [&]() -> std::shared_ptr<awst::Expression> {
		if (singlePage)
			return awst::makeUInt64BinOp(s64Var(),
				awst::UInt64BinaryOperator::Mult, makeUint64("32"), loc);
		auto idx = awst::makeUInt64BinOp(s64Var(),
			awst::UInt64BinaryOperator::Mod,
			makeUint64(std::to_string(kEvmSlotsPerPage)), loc);
		return awst::makeUInt64BinOp(std::move(idx),
			awst::UInt64BinaryOperator::Mult, makeUint64("32"), loc);
	};
	auto sparseKey = [&]() {
		auto slotBytes = awst::makeLeftPadToN(awst::makeAsBytes(slotVar(), loc), 32, loc);
		return awst::makeConcat(makeBytes("s:"), std::move(slotBytes), loc);
	};
	auto boxExists = [&](std::shared_ptr<awst::Expression> _key) {
		auto boxLen = StorageMapper::makeBoxLenTuple(m_typeMapper, std::move(_key), loc);
		return awst::makeTupleItem(std::move(boxLen), 1, awst::WType::boolType(), loc);
	};
	auto retZero = [&](awst::Block& _blk) {
		_blk.body.push_back(awst::makeReturnStatement(
			awst::makeIntegerConstant("0", loc, awst::WType::biguintType()), loc));
	};

	// ── __storage_read(slot: biguint) -> biguint ──
	auto buildStorageRead = [&]() {
		awst::ContractMethod readSub;
		readSub.sourceLocation = loc;
		readSub.cref = cref;
		readSub.memberName = "__storage_read";
		readSub.returnType = awst::WType::biguintType();
		readSub.arc4MethodConfig = std::nullopt;
		readSub.pure = false;

		awst::SubroutineArgument slotArg;
		slotArg.name = "__slot";
		slotArg.wtype = awst::WType::biguintType();
		slotArg.sourceLocation = loc;
		readSub.args.push_back(slotArg);

		auto body = awst::makeBlock(loc);
		if (!denseOnly)
			body->body.push_back(makeSlotWrapStmt());

		// Dense: absent page reads as 0 (no box_create on the read path — a
		// read must not charge MBR).
		auto denseBlk = awst::makeBlock(loc);
		{
			bindS64(*denseBlk);
			auto key = pageKey();
			auto thenBlk = awst::makeBlock(loc);
			thenBlk->body.push_back(awst::makeReturnStatement(
				awst::makeAsBiguint(
					awst::makeBoxExtract(key, pageOff(), makeUint64("32"), loc), loc),
				loc));
			denseBlk->body.push_back(awst::makeIfElse(
				boxExists(key), std::move(thenBlk), nullptr, loc));
			retZero(*denseBlk);
		}

		if (denseOnly)
			for (auto& st: denseBlk->body)
				body->body.push_back(std::move(st));
		else
		{
			// Sparse: one box per slot, absent slot reads as 0.
			auto sparseBlk = awst::makeBlock(loc);
			{
				auto key = sparseKey();
				auto thenBlk = awst::makeBlock(loc);
				thenBlk->body.push_back(awst::makeReturnStatement(
					awst::makeAsBiguint(
						awst::makeBoxExtract(key, makeUint64("0"), makeUint64("32"), loc), loc),
					loc));
				sparseBlk->body.push_back(awst::makeIfElse(
					boxExists(key), std::move(thenBlk), nullptr, loc));
				retZero(*sparseBlk);
			}

			body->body.push_back(awst::makeIfElse(
				denseCmp(), std::move(denseBlk), std::move(sparseBlk), loc));
		}

		readSub.body = body;
		_contractNode->methods.push_back(std::move(readSub));
	};
	buildStorageRead();

	// ── __storage_write(slot: biguint, value: biguint) -> void ──
	auto buildStorageWrite = [&]() {
		awst::ContractMethod writeSub;
		writeSub.sourceLocation = loc;
		writeSub.cref = cref;
		writeSub.memberName = "__storage_write";
		writeSub.returnType = awst::WType::voidType();
		writeSub.arc4MethodConfig = std::nullopt;
		writeSub.pure = false;

		awst::SubroutineArgument slotArg;
		slotArg.name = "__slot";
		slotArg.wtype = awst::WType::biguintType();
		slotArg.sourceLocation = loc;
		writeSub.args.push_back(slotArg);

		awst::SubroutineArgument valArg;
		valArg.name = "__value";
		valArg.wtype = awst::WType::biguintType();
		valArg.sourceLocation = loc;
		writeSub.args.push_back(valArg);

		auto body = awst::makeBlock(loc);
		if (!denseOnly)
			body->body.push_back(makeSlotWrapStmt());

		auto paddedVal = [&]() {
			return awst::makeLeftPadToN(awst::makeAsBytes(
				awst::makeVarExpression("__value", awst::WType::biguintType(), loc), loc),
				32, loc);
		};

		// Dense: lazily materialise the 2048-byte page (box_create is a no-op
		// when it already exists), then patch the slot's 32-byte window.
		auto denseBlk = awst::makeBlock(loc);
		{
			bindS64(*denseBlk);
			auto key = pageKey();
			denseBlk->body.push_back(awst::makeExpressionStatement(
				awst::makeBoxCreate(key,
					makeUint64(std::to_string(kEvmSlotsPerPage * 32ULL)), loc), loc));
			auto boxReplace = awst::makeIntrinsicCall("box_replace", awst::WType::voidType(), loc);
			boxReplace->stackArgs.push_back(key);
			boxReplace->stackArgs.push_back(pageOff());
			boxReplace->stackArgs.push_back(paddedVal());
			denseBlk->body.push_back(
				awst::makeExpressionStatement(std::move(boxReplace), loc));
			denseBlk->body.push_back(awst::makeReturnStatement(nullptr, loc));
		}

		if (denseOnly)
			for (auto& st: denseBlk->body)
				body->body.push_back(std::move(st));
		else
		{
			// Sparse: one 32-byte box per slot. NOT paged: mapping entries are
			// keccak(key ++ slot) and genuinely scattered, so a 64-slot page would
			// hold ONE live entry while charging its full MBR (28,900 → 835,300
			// microAlgos per entry — 29x on the commonest real pattern).
			auto sparseBlk = awst::makeBlock(loc);
			{
				auto key = sparseKey();
				sparseBlk->body.push_back(awst::makeExpressionStatement(
					awst::makeBoxCreate(key, makeUint64("32"), loc), loc));
				auto boxReplace = awst::makeIntrinsicCall("box_replace", awst::WType::voidType(), loc);
				boxReplace->stackArgs.push_back(key);
				boxReplace->stackArgs.push_back(makeUint64("0"));
				boxReplace->stackArgs.push_back(paddedVal());
				sparseBlk->body.push_back(
					awst::makeExpressionStatement(std::move(boxReplace), loc));
				sparseBlk->body.push_back(awst::makeReturnStatement(nullptr, loc));
			}

			body->body.push_back(awst::makeIfElse(
				denseCmp(), std::move(denseBlk), std::move(sparseBlk), loc));
		}

		writeSub.body = body;
		_contractNode->methods.push_back(std::move(writeSub));
	};
	buildStorageWrite();

	// ── EVM bytes/string storage codec ──────────────────────────────────────
	// Solidity storage format: short (len<32) = data left-aligned ++ 2*len in
	// the low byte, all in the slot word; long = word 2*len+1 at the slot,
	// data in 32-byte chunks at keccak256(slot32)+i.
	auto readWordCall = [&](std::shared_ptr<awst::Expression> _slot) {
		auto call = awst::makeSubroutineCall(
			awst::SubroutineID{"__puyasol___storage_read"},
			awst::WType::biguintType(), loc);
		awst::pushCallArg(call->args, "__slot", std::move(_slot));
		return std::shared_ptr<awst::Expression>(std::move(call));
	};
	auto writeWordStmt = [&](std::shared_ptr<awst::Expression> _slot,
		std::shared_ptr<awst::Expression> _word) {
		auto call = awst::makeSubroutineCall(
			awst::SubroutineID{"__puyasol___storage_write"},
			awst::WType::voidType(), loc);
		awst::pushCallArg(call->args, "__slot", std::move(_slot));
		awst::pushCallArg(call->args, "__value", std::move(_word));
		return awst::makeExpressionStatement(std::move(call), loc);
	};
	auto chunkBase = [&]() {
		// keccak256(slot32) as biguint — the data region of the long form.
		auto slotBytes = awst::makeLeftPadToN(
			awst::makeAsBytes(slotVar(), loc), 32, loc);
		return awst::makeAsBiguint(
			awst::makeKeccak256(std::move(slotBytes), loc), loc);
	};
	auto u64Var = [&](std::string const& n) {
		return awst::makeVarExpression(n, awst::WType::uint64Type(), loc);
	};
	auto bytesVar = [&](std::string const& n) {
		return awst::makeVarExpression(n, awst::WType::bytesType(), loc);
	};
	auto biguintVar = [&](std::string const& n) {
		return awst::makeVarExpression(n, awst::WType::biguintType(), loc);
	};
	auto u64c = [&](uint64_t v) { return awst::makeIntegerConstant(v, loc); };
	auto u64ToBiguint = [&](std::shared_ptr<awst::Expression> e) {
		return awst::makeAsBiguint(awst::makeItob(std::move(e), loc), loc);
	};
	auto biguintToU64 = [&](std::shared_ptr<awst::Expression> e) {
		auto cat = awst::makeLeftPad(awst::makeAsBytes(std::move(e), loc), 8, loc);
		return awst::makeBtoi(awst::makeExtractLastN(std::move(cat), 8, loc), loc);
	};

	// ── __evm_bytes_read(slot: biguint) -> bytes ──
	auto buildBytesRead = [&]() {
		awst::ContractMethod sub;
		sub.sourceLocation = loc;
		sub.cref = cref;
		sub.memberName = "__evm_bytes_read";
		sub.returnType = awst::WType::bytesType();
		sub.arc4MethodConfig = std::nullopt;
		sub.pure = false;
		awst::SubroutineArgument slotArg;
		slotArg.name = "__slot";
		slotArg.wtype = awst::WType::biguintType();
		slotArg.sourceLocation = loc;
		sub.args.push_back(slotArg);

		auto body = awst::makeBlock(loc);
		// wb = pad32(word); lastByte = wb[31]
		body->body.push_back(awst::makeAssignmentStatement(
			bytesVar("__wb"),
			awst::makeLeftPadToN(awst::makeAsBytes(
				readWordCall(slotVar()), loc), 32, loc), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__last"),
			awst::makeBtoi(awst::makeExtract(bytesVar("__wb"), 31, 1, loc), loc), loc));
		// short form: even last byte → len = last/2, data = wb[0:len]
		{
			auto isShort = awst::makeNumericCompare(
				awst::makeUInt64BinOp(u64Var("__last"),
					awst::UInt64BinaryOperator::Mod, u64c(2), loc),
				awst::NumericComparison::Eq, u64c(0), loc);
			auto thenBlk = awst::makeBlock(loc);
			auto lenS = awst::makeUInt64BinOp(u64Var("__last"),
				awst::UInt64BinaryOperator::FloorDiv, u64c(2), loc);
			thenBlk->body.push_back(awst::makeReturnStatement(
				awst::makeExtract3(bytesVar("__wb"), u64c(0), std::move(lenS), loc),
				loc));
			body->body.push_back(awst::makeIfElse(
				std::move(isShort), std::move(thenBlk), nullptr, loc));
		}
		// long form: len = (word-1)/2 (word reconstructed from wb)
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__len"),
			biguintToU64(awst::makeBigUIntBinOp(
				awst::makeBigUIntBinOp(
					awst::makeAsBiguint(bytesVar("__wb"), loc),
					awst::BigUIntBinaryOperator::Sub,
					awst::makeIntegerConstant("1", loc, awst::WType::biguintType()), loc),
				awst::BigUIntBinaryOperator::FloorDiv,
				awst::makeIntegerConstant("2", loc, awst::WType::biguintType()), loc)),
			loc));
		body->body.push_back(awst::makeAssignmentStatement(
			biguintVar("__chunk"), chunkBase(), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			bytesVar("__data"), awst::makeBytesConstant({}, loc), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__i"), u64c(0), loc));
		{
			auto cond = awst::makeNumericCompare(
				awst::makeUInt64BinOp(u64Var("__i"),
					awst::UInt64BinaryOperator::Mult, u64c(32), loc),
				awst::NumericComparison::Lt, u64Var("__len"), loc);
			auto loop = awst::makeBlock(loc);
			auto chunkWord = readWordCall(awst::makeBigUIntBinOp(
				biguintVar("__chunk"), awst::BigUIntBinaryOperator::Add,
				u64ToBiguint(u64Var("__i")), loc));
			loop->body.push_back(awst::makeAssignmentStatement(
				bytesVar("__data"),
				awst::makeConcat(bytesVar("__data"),
					awst::makeLeftPadToN(
						awst::makeAsBytes(std::move(chunkWord), loc), 32, loc),
					loc), loc));
			loop->body.push_back(awst::makeAssignmentStatement(
				u64Var("__i"),
				awst::makeUInt64BinOp(u64Var("__i"),
					awst::UInt64BinaryOperator::Add, u64c(1), loc), loc));
			body->body.push_back(awst::makeWhileLoop(std::move(cond), std::move(loop), loc));
		}
		body->body.push_back(awst::makeReturnStatement(
			awst::makeExtract3(bytesVar("__data"), u64c(0), u64Var("__len"), loc), loc));

		sub.body = body;
		_contractNode->methods.push_back(std::move(sub));
	};
	buildBytesRead();

	// ── __evm_bytes_write(slot: biguint, val: bytes) -> void ──
	auto buildBytesWrite = [&]() {
		awst::ContractMethod sub;
		sub.sourceLocation = loc;
		sub.cref = cref;
		sub.memberName = "__evm_bytes_write";
		sub.returnType = awst::WType::voidType();
		sub.arc4MethodConfig = std::nullopt;
		sub.pure = false;
		awst::SubroutineArgument slotArg;
		slotArg.name = "__slot";
		slotArg.wtype = awst::WType::biguintType();
		slotArg.sourceLocation = loc;
		sub.args.push_back(slotArg);
		awst::SubroutineArgument valArg;
		valArg.name = "__val";
		valArg.wtype = awst::WType::bytesType();
		valArg.sourceLocation = loc;
		sub.args.push_back(valArg);

		auto valVar = [&]() { return bytesVar("__val"); };
		auto body = awst::makeBlock(loc);
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__len"), awst::makeLen(valVar(), loc), loc));
		// old word FIRST (stale-chunk cleanup needs the previous length)
		body->body.push_back(awst::makeAssignmentStatement(
			bytesVar("__ow"),
			awst::makeLeftPadToN(awst::makeAsBytes(
				readWordCall(slotVar()), loc), 32, loc), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			biguintVar("__chunk"), chunkBase(), loc));
		// old chunk count: odd old word → ceil(((word-1)/2)/32), else 0
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__oldChunks"), u64c(0), loc));
		{
			auto wasLong = awst::makeNumericCompare(
				awst::makeUInt64BinOp(
					awst::makeBtoi(awst::makeExtract(bytesVar("__ow"), 31, 1, loc), loc),
					awst::UInt64BinaryOperator::Mod, u64c(2), loc),
				awst::NumericComparison::Eq, u64c(1), loc);
			auto thenBlk = awst::makeBlock(loc);
			auto oldLen = biguintToU64(awst::makeBigUIntBinOp(
				awst::makeBigUIntBinOp(
					awst::makeAsBiguint(bytesVar("__ow"), loc),
					awst::BigUIntBinaryOperator::Sub,
					awst::makeIntegerConstant("1", loc, awst::WType::biguintType()), loc),
				awst::BigUIntBinaryOperator::FloorDiv,
				awst::makeIntegerConstant("2", loc, awst::WType::biguintType()), loc));
			thenBlk->body.push_back(awst::makeAssignmentStatement(
				u64Var("__oldChunks"),
				awst::makeUInt64BinOp(
					awst::makeUInt64BinOp(std::move(oldLen),
						awst::UInt64BinaryOperator::Add, u64c(31), loc),
					awst::UInt64BinaryOperator::FloorDiv, u64c(32), loc), loc));
			body->body.push_back(awst::makeIfElse(
				std::move(wasLong), std::move(thenBlk), nullptr, loc));
		}
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__newChunks"), u64c(0), loc));
		// short: word = val ++ zeros to 31 ++ byte(2*len)
		{
			auto isShort = awst::makeNumericCompare(
				u64Var("__len"), awst::NumericComparison::Lt, u64c(32), loc);
			auto thenBlk = awst::makeBlock(loc);
			auto data31 = awst::makeExtract3(
				awst::makeConcat(valVar(), awst::makeBzero(31, loc), loc),
				u64c(0), u64c(31), loc);
			auto lenByte = awst::makeExtract(
				awst::makeItob(awst::makeUInt64BinOp(u64Var("__len"),
					awst::UInt64BinaryOperator::Mult, u64c(2), loc), loc),
				7, 1, loc);
			thenBlk->body.push_back(writeWordStmt(slotVar(),
				awst::makeAsBiguint(awst::makeConcat(
					std::move(data31), std::move(lenByte), loc), loc)));
			auto elseBlk = awst::makeBlock(loc);
			// long: length word = 2*len+1, chunks at keccak(slot)+i
			elseBlk->body.push_back(writeWordStmt(slotVar(),
				awst::makeAsBiguint(awst::makeItob(
					awst::makeUInt64BinOp(
						awst::makeUInt64BinOp(u64Var("__len"),
							awst::UInt64BinaryOperator::Mult, u64c(2), loc),
						awst::UInt64BinaryOperator::Add, u64c(1), loc), loc), loc)));
			elseBlk->body.push_back(awst::makeAssignmentStatement(
				u64Var("__newChunks"),
				awst::makeUInt64BinOp(
					awst::makeUInt64BinOp(u64Var("__len"),
						awst::UInt64BinaryOperator::Add, u64c(31), loc),
					awst::UInt64BinaryOperator::FloorDiv, u64c(32), loc), loc));
			elseBlk->body.push_back(awst::makeAssignmentStatement(
				bytesVar("__padded"),
				awst::makeConcat(valVar(), awst::makeBzero(32, loc), loc), loc));
			elseBlk->body.push_back(awst::makeAssignmentStatement(
				u64Var("__i"), u64c(0), loc));
			auto cond = awst::makeNumericCompare(u64Var("__i"),
				awst::NumericComparison::Lt, u64Var("__newChunks"), loc);
			auto loop = awst::makeBlock(loc);
			auto chunk = awst::makeExtract3(bytesVar("__padded"),
				awst::makeUInt64BinOp(u64Var("__i"),
					awst::UInt64BinaryOperator::Mult, u64c(32), loc),
				u64c(32), loc);
			loop->body.push_back(writeWordStmt(
				awst::makeBigUIntBinOp(biguintVar("__chunk"),
					awst::BigUIntBinaryOperator::Add,
					u64ToBiguint(u64Var("__i")), loc),
				awst::makeAsBiguint(std::move(chunk), loc)));
			loop->body.push_back(awst::makeAssignmentStatement(
				u64Var("__i"),
				awst::makeUInt64BinOp(u64Var("__i"),
					awst::UInt64BinaryOperator::Add, u64c(1), loc), loc));
			elseBlk->body.push_back(awst::makeWhileLoop(
				std::move(cond), std::move(loop), loc));
			body->body.push_back(awst::makeIfElse(
				std::move(isShort), std::move(thenBlk), std::move(elseBlk), loc));
		}
		// clear stale long chunks beyond the new count (EVM zeroes on shrink)
		{
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__j"), u64Var("__newChunks"), loc));
			auto cond = awst::makeNumericCompare(u64Var("__j"),
				awst::NumericComparison::Lt, u64Var("__oldChunks"), loc);
			auto loop = awst::makeBlock(loc);
			loop->body.push_back(writeWordStmt(
				awst::makeBigUIntBinOp(biguintVar("__chunk"),
					awst::BigUIntBinaryOperator::Add,
					u64ToBiguint(u64Var("__j")), loc),
				awst::makeIntegerConstant("0", loc, awst::WType::biguintType())));
			loop->body.push_back(awst::makeAssignmentStatement(
				u64Var("__j"),
				awst::makeUInt64BinOp(u64Var("__j"),
					awst::UInt64BinaryOperator::Add, u64c(1), loc), loc));
			body->body.push_back(awst::makeWhileLoop(
				std::move(cond), std::move(loop), loc));
		}
		body->body.push_back(awst::makeReturnStatement(nullptr, loc));

		sub.body = body;
		_contractNode->methods.push_back(std::move(sub));
	};
	buildBytesWrite();

	// ── __evm_dynarr_read(slot: biguint) -> bytes ──
	// Materialise a dynamic array of 32-byte-encoded elements as its ARC4
	// form [u16 count][elems]: count word at the slot, elements at
	// keccak256(slot32)+i. Callers cap/validate element width.
	auto buildDynamicArrayRead = [&]() {
		awst::ContractMethod sub;
		sub.sourceLocation = loc;
		sub.cref = cref;
		sub.memberName = "__evm_dynarr_read";
		sub.returnType = awst::WType::bytesType();
		sub.arc4MethodConfig = std::nullopt;
		sub.pure = false;
		awst::SubroutineArgument slotArg2;
		slotArg2.name = "__slot";
		slotArg2.wtype = awst::WType::biguintType();
		slotArg2.sourceLocation = loc;
		sub.args.push_back(slotArg2);
		for (char const* an: {"__size", "__aw", "__per", "__mul", "__bp"})
		{
			awst::SubroutineArgument a;
			a.name = an;
			a.wtype = awst::WType::uint64Type();
			a.sourceLocation = loc;
			sub.args.push_back(a);
		}

		// __size = storage bytes per element, __aw = ARC4 bytes per element
		// (differs for address: 20 stored, 32 encoded), __per = elements per
		// slot (EVM packs from the LOW end of the word).
		auto body = awst::makeBlock(loc);
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__n"), biguintToU64(readWordCall(slotVar())), loc));
		// __mul = lanes per ELEMENT (fixed-array / uniform-struct elements are
		// lane concatenations in both slot and ARC4 layouts); the loops run
		// over LANES while the count word/prefix stays in elements. __bp marks
		// fixed bool[N] elements: EVM stores byte lanes, ARC4 stores MSB-first
		// bits in an __aw-byte region reset for each outer element.
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__nl"), awst::makeUInt64BinOp(u64Var("__n"),
				awst::UInt64BinaryOperator::Mult, u64Var("__mul"), loc), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			biguintVar("__chunk"), chunkBase(), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			bytesVar("__data"),
			awst::makeExtract(awst::makeItob(u64Var("__n"), loc), 6, 2, loc), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__i"), u64c(0), loc));
		// Seed __wb so definite assignment is provable. Both loops below only
		// (re)establish it at a word boundary (__j == 0), and __i starts at 0 so
		// the first iteration always takes that branch — but puya cannot derive
		// __j == 0 from __i == 0, so it warned "__wb potentially used before
		// assignment" on EVERY slot-mode contract with a mapping. Spurious, but
		// it buries real warnings, and the write path's self-referential
		// `__wb = (__j == 0) ? bzero(32) : __wb` genuinely reads it first.
		body->body.push_back(awst::makeAssignmentStatement(
			bytesVar("__wb"), awst::makeBzero(u64c(32), loc), loc));
		auto cond = awst::makeNumericCompare(u64Var("__i"),
			awst::NumericComparison::Lt, u64Var("__nl"), loc);
		auto loop = awst::makeBlock(loc);
		// slotIdx = i / per ; j = i - slotIdx*per ; off = 32 - (j+1)*size
		loop->body.push_back(awst::makeAssignmentStatement(u64Var("__wi"),
			awst::makeUInt64BinOp(u64Var("__i"),
				awst::UInt64BinaryOperator::FloorDiv, u64Var("__per"), loc), loc));
		loop->body.push_back(awst::makeAssignmentStatement(u64Var("__j"),
			awst::makeUInt64BinOp(u64Var("__i"),
				awst::UInt64BinaryOperator::Sub,
				awst::makeUInt64BinOp(u64Var("__wi"),
					awst::UInt64BinaryOperator::Mult, u64Var("__per"), loc), loc), loc));
		// Read once at the first lane of each storage word.  Packed elements —
		// especially fixed bool arrays, where one outer element expands to many
		// lanes — otherwise paid for the same storage read on every lane.
		{
			auto loadWord = awst::makeBlock(loc);
			loadWord->body.push_back(awst::makeAssignmentStatement(bytesVar("__wb"),
				awst::makeLeftPadToN(awst::makeAsBytes(readWordCall(
					awst::makeBigUIntBinOp(biguintVar("__chunk"),
						awst::BigUIntBinaryOperator::Add,
						u64ToBiguint(u64Var("__wi")), loc)), loc), 32, loc), loc));
			loop->body.push_back(awst::makeIfElse(
				awst::makeNumericCompare(u64Var("__j"),
					awst::NumericComparison::Eq, u64c(0), loc),
				std::move(loadWord), nullptr, loc));
		}
		auto offExpr = awst::makeUInt64BinOp(u64c(32),
			awst::UInt64BinaryOperator::Sub,
			awst::makeUInt64BinOp(
				awst::makeUInt64BinOp(u64Var("__j"),
					awst::UInt64BinaryOperator::Add, u64c(1), loc),
				awst::UInt64BinaryOperator::Mult, u64Var("__size"), loc), loc);
		loop->body.push_back(awst::makeAssignmentStatement(bytesVar("__el"),
			awst::makeExtract3(bytesVar("__wb"), std::move(offExpr),
				u64Var("__size"), loc), loc));
		// Byte-aligned elements append one ARC4 lane. Fixed bool[N] elements
		// append a zeroed region once per outer element and set its bits from
		// the canonical low-byte EVM lanes.
		{
			auto bitBlk = awst::makeBlock(loc);
			bitBlk->body.push_back(awst::makeAssignmentStatement(u64Var("__bj"),
				awst::makeUInt64BinOp(u64Var("__i"),
					awst::UInt64BinaryOperator::Mod, u64Var("__mul"), loc), loc));
			auto beginElem = awst::makeBlock(loc);
			beginElem->body.push_back(awst::makeAssignmentStatement(
				bytesVar("__data"), awst::makeConcat(bytesVar("__data"),
					awst::makeBzero(u64Var("__aw"), loc), loc), loc));
			bitBlk->body.push_back(awst::makeIfElse(
				awst::makeNumericCompare(u64Var("__bj"),
					awst::NumericComparison::Eq, u64c(0), loc),
				std::move(beginElem), nullptr, loc));
			auto elemNo = awst::makeUInt64BinOp(u64Var("__i"),
				awst::UInt64BinaryOperator::FloorDiv, u64Var("__mul"), loc);
			auto bitIndex = awst::makeUInt64BinOp(
				awst::makeUInt64BinOp(
					awst::makeUInt64BinOp(u64c(2),
						awst::UInt64BinaryOperator::Add,
						awst::makeUInt64BinOp(std::move(elemNo),
							awst::UInt64BinaryOperator::Mult, u64Var("__aw"), loc), loc),
					awst::UInt64BinaryOperator::Mult, u64c(8), loc),
				awst::UInt64BinaryOperator::Add, u64Var("__bj"), loc);
			auto isTrue = awst::makeNumericCompare(
				awst::makeBtoi(bytesVar("__el"), loc),
				awst::NumericComparison::Ne, u64c(0), loc);
			bitBlk->body.push_back(awst::makeAssignmentStatement(
				bytesVar("__data"), awst::makeSetbit(bytesVar("__data"),
					std::move(bitIndex), std::move(isTrue), loc), loc));

			auto byteBlk = awst::makeBlock(loc);
			byteBlk->body.push_back(awst::makeAssignmentStatement(
				bytesVar("__data"),
				awst::makeConcat(bytesVar("__data"),
					awst::makeConcat(
						awst::makeBzero(
							awst::makeUInt64BinOp(u64Var("__aw"),
								awst::UInt64BinaryOperator::Sub,
								u64Var("__size"), loc), loc),
						bytesVar("__el"), loc), loc), loc));
			loop->body.push_back(awst::makeIfElse(
				awst::makeNumericCompare(u64Var("__bp"),
					awst::NumericComparison::Ne, u64c(0), loc),
				std::move(bitBlk), std::move(byteBlk), loc));
		}
		loop->body.push_back(awst::makeAssignmentStatement(u64Var("__i"),
			awst::makeUInt64BinOp(u64Var("__i"),
				awst::UInt64BinaryOperator::Add, u64c(1), loc), loc));
		body->body.push_back(awst::makeWhileLoop(std::move(cond), std::move(loop), loc));
		body->body.push_back(awst::makeReturnStatement(bytesVar("__data"), loc));

		sub.body = body;
		_contractNode->methods.push_back(std::move(sub));
	};
	buildDynamicArrayRead();

	// ── __evm_dynarr_write(slot: biguint, val: bytes) -> void ──
	// Inverse of __evm_dynarr_read: val is the ARC4 form [u16 count][32B
	// elems]. Writes the length word at the slot, elements at
	// keccak256(slot32)+i, and CLEARS the old tail when the array shrinks —
	// EVM assignment semantics, and a later push must see zeroed slots.
	auto buildDynamicArrayWrite = [&]() {
		awst::ContractMethod sub;
		sub.sourceLocation = loc;
		sub.cref = cref;
		sub.memberName = "__evm_dynarr_write";
		sub.returnType = awst::WType::voidType();
		sub.arc4MethodConfig = std::nullopt;
		sub.pure = false;
		awst::SubroutineArgument slotArg3;
		slotArg3.name = "__slot";
		slotArg3.wtype = awst::WType::biguintType();
		slotArg3.sourceLocation = loc;
		sub.args.push_back(slotArg3);
		awst::SubroutineArgument valArg;
		valArg.name = "__val";
		valArg.wtype = awst::WType::bytesType();
		valArg.sourceLocation = loc;
		sub.args.push_back(valArg);
		auto valVar = [&]() {
			return awst::makeVarExpression("__val", awst::WType::bytesType(), loc);
		};
		for (char const* an: {"__size", "__aw", "__per", "__mul", "__bp"})
		{
			awst::SubroutineArgument a;
			a.name = an;
			a.wtype = awst::WType::uint64Type();
			a.sourceLocation = loc;
			sub.args.push_back(a);
		}

		auto body = awst::makeBlock(loc);
		// old length (for the shrink-clear tail)
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__old"), biguintToU64(readWordCall(slotVar())), loc));
		// new length from the ARC4 u16 header
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__n"),
			awst::makeBtoi(awst::makeExtract(valVar(), 0, 2, loc), loc), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__nl"), awst::makeUInt64BinOp(u64Var("__n"),
				awst::UInt64BinaryOperator::Mult, u64Var("__mul"), loc), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__oldl"), awst::makeUInt64BinOp(u64Var("__old"),
				awst::UInt64BinaryOperator::Mult, u64Var("__mul"), loc), loc));
		body->body.push_back(writeWordStmt(slotVar(),
			u64ToBiguint(u64Var("__n"))));
		body->body.push_back(awst::makeAssignmentStatement(
			biguintVar("__chunk"), chunkBase(), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__i"), u64c(0), loc));
		// Seed __wb so definite assignment is provable. Both loops below only
		// (re)establish it at a word boundary (__j == 0), and __i starts at 0 so
		// the first iteration always takes that branch — but puya cannot derive
		// __j == 0 from __i == 0, so it warned "__wb potentially used before
		// assignment" on EVERY slot-mode contract with a mapping. Spurious, but
		// it buries real warnings, and the write path's self-referential
		// `__wb = (__j == 0) ? bzero(32) : __wb` genuinely reads it first.
		body->body.push_back(awst::makeAssignmentStatement(
			bytesVar("__wb"), awst::makeBzero(u64c(32), loc), loc));
		// write the new elements (packed: rebuild each word from zero at its
		// first element, so a partially-filled last word has clean high bytes)
		{
			auto cond = awst::makeNumericCompare(u64Var("__i"),
				awst::NumericComparison::Lt, u64Var("__nl"), loc);
			auto loop = awst::makeBlock(loc);
			loop->body.push_back(awst::makeAssignmentStatement(u64Var("__wi"),
				awst::makeUInt64BinOp(u64Var("__i"),
					awst::UInt64BinaryOperator::FloorDiv, u64Var("__per"), loc), loc));
			loop->body.push_back(awst::makeAssignmentStatement(u64Var("__j"),
				awst::makeUInt64BinOp(u64Var("__i"),
					awst::UInt64BinaryOperator::Sub,
					awst::makeUInt64BinOp(u64Var("__wi"),
						awst::UInt64BinaryOperator::Mult, u64Var("__per"), loc),
					loc), loc));
			loop->body.push_back(awst::makeAssignmentStatement(
				biguintVar("__ws"),
				awst::makeBigUIntBinOp(biguintVar("__chunk"),
					awst::BigUIntBinaryOperator::Add,
					u64ToBiguint(u64Var("__wi")), loc), loc));
			// fresh word at each word boundary — no read needed, because every
			// element of the word is (re)written before it is stored
			loop->body.push_back(awst::makeAssignmentStatement(bytesVar("__wb"),
				awst::makeConditional(
					awst::makeNumericCompare(u64Var("__j"),
						awst::NumericComparison::Eq, u64c(0), loc),
					awst::makeBzero(u64c(32), loc),
					bytesVar("__wb"),
					awst::WType::bytesType(), loc), loc));
			// Byte-aligned elements take the low `size` bytes of each ARC4 lane.
			// Fixed bool[N] elements read one ARC4 bit and turn it back into the
			// canonical 0/1 byte stored by Solidity.
			{
				auto bitBlk = awst::makeBlock(loc);
				bitBlk->body.push_back(awst::makeAssignmentStatement(u64Var("__bj"),
					awst::makeUInt64BinOp(u64Var("__i"),
						awst::UInt64BinaryOperator::Mod, u64Var("__mul"), loc), loc));
				auto elemNo = awst::makeUInt64BinOp(u64Var("__i"),
					awst::UInt64BinaryOperator::FloorDiv, u64Var("__mul"), loc);
				auto bitIndex = awst::makeUInt64BinOp(
					awst::makeUInt64BinOp(
						awst::makeUInt64BinOp(u64c(2),
							awst::UInt64BinaryOperator::Add,
							awst::makeUInt64BinOp(std::move(elemNo),
								awst::UInt64BinaryOperator::Mult, u64Var("__aw"), loc), loc),
						awst::UInt64BinaryOperator::Mult, u64c(8), loc),
					awst::UInt64BinaryOperator::Add, u64Var("__bj"), loc);
				bitBlk->body.push_back(awst::makeAssignmentStatement(bytesVar("__el"),
					awst::makeExtract(awst::makeItob(
						awst::makeGetbit(valVar(), std::move(bitIndex), loc), loc),
						7, 1, loc), loc));

				auto byteBlk = awst::makeBlock(loc);
				auto vOff = awst::makeUInt64BinOp(
					awst::makeUInt64BinOp(u64Var("__i"),
						awst::UInt64BinaryOperator::Mult, u64Var("__aw"), loc),
					awst::UInt64BinaryOperator::Add,
					awst::makeUInt64BinOp(u64c(2),
						awst::UInt64BinaryOperator::Add,
						awst::makeUInt64BinOp(u64Var("__aw"),
							awst::UInt64BinaryOperator::Sub, u64Var("__size"), loc),
						loc), loc);
				byteBlk->body.push_back(awst::makeAssignmentStatement(bytesVar("__el"),
					awst::makeExtract3(valVar(), std::move(vOff),
						u64Var("__size"), loc), loc));
				loop->body.push_back(awst::makeIfElse(
					awst::makeNumericCompare(u64Var("__bp"),
						awst::NumericComparison::Ne, u64c(0), loc),
					std::move(bitBlk), std::move(byteBlk), loc));
			}
			auto wOff = awst::makeUInt64BinOp(u64c(32),
				awst::UInt64BinaryOperator::Sub,
				awst::makeUInt64BinOp(
					awst::makeUInt64BinOp(u64Var("__j"),
						awst::UInt64BinaryOperator::Add, u64c(1), loc),
					awst::UInt64BinaryOperator::Mult, u64Var("__size"), loc), loc);
			loop->body.push_back(awst::makeAssignmentStatement(bytesVar("__wb"),
				awst::makeReplace3(bytesVar("__wb"), std::move(wOff),
					bytesVar("__el"), loc), loc));
			{
				auto flush = awst::makeBlock(loc);
				flush->body.push_back(writeWordStmt(biguintVar("__ws"),
					awst::makeAsBiguint(bytesVar("__wb"), loc)));
				auto lastInWord = awst::makeNumericCompare(u64Var("__j"),
					awst::NumericComparison::Eq,
					awst::makeUInt64BinOp(u64Var("__per"),
						awst::UInt64BinaryOperator::Sub, u64c(1), loc), loc);
				auto lastElem = awst::makeNumericCompare(u64Var("__i"),
					awst::NumericComparison::Eq,
					awst::makeUInt64BinOp(u64Var("__nl"),
						awst::UInt64BinaryOperator::Sub, u64c(1), loc), loc);
				loop->body.push_back(awst::makeIfElse(
					awst::makeBoolBinOp(std::move(lastInWord),
						awst::BinaryBooleanOperator::Or, std::move(lastElem), loc),
					std::move(flush), nullptr, loc));
			}
			loop->body.push_back(awst::makeAssignmentStatement(u64Var("__i"),
				awst::makeUInt64BinOp(u64Var("__i"),
					awst::UInt64BinaryOperator::Add, u64c(1), loc), loc));
			body->body.push_back(awst::makeWhileLoop(
				std::move(cond), std::move(loop), loc));
		}
		// clear the shrink tail: whole WORDS from ceil(n/per) to ceil(old/per)
		{
			auto ceilDiv = [&](std::shared_ptr<awst::Expression> _v) {
				return awst::makeUInt64BinOp(
					awst::makeUInt64BinOp(std::move(_v),
						awst::UInt64BinaryOperator::Add,
						awst::makeUInt64BinOp(u64Var("__per"),
							awst::UInt64BinaryOperator::Sub, u64c(1), loc), loc),
					awst::UInt64BinaryOperator::FloorDiv, u64Var("__per"), loc);
			};
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__wi"), ceilDiv(u64Var("__nl")), loc));
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__we"), ceilDiv(u64Var("__oldl")), loc));
			auto cond = awst::makeNumericCompare(u64Var("__wi"),
				awst::NumericComparison::Lt, u64Var("__we"), loc);
			auto loop = awst::makeBlock(loc);
			loop->body.push_back(writeWordStmt(
				awst::makeBigUIntBinOp(biguintVar("__chunk"),
					awst::BigUIntBinaryOperator::Add,
					u64ToBiguint(u64Var("__wi")), loc),
				awst::makeZero(loc, awst::WType::biguintType())));
			loop->body.push_back(awst::makeAssignmentStatement(u64Var("__wi"),
				awst::makeUInt64BinOp(u64Var("__wi"),
					awst::UInt64BinaryOperator::Add, u64c(1), loc), loc));
			body->body.push_back(awst::makeWhileLoop(
				std::move(cond), std::move(loop), loc));
		}
		sub.body = body;
		_contractNode->methods.push_back(std::move(sub));
	};
	buildDynamicArrayWrite();

	// ── Recursive dynamic-array read / write ──
	// Every dynamic-array layer has the same storage and ARC4 structure. Depth
	// one delegates to the leaf codec above; greater depths recursively compose
	// u16 heads and inner tails. T[][][] is therefore the same path as T[][].
	auto buildNestedDynamicArrayMethods = [&]() {
		auto metricArgs = [&](std::vector<awst::CallArg>& _args) {
			for (char const* an: {"__size", "__aw", "__per", "__mul", "__bp"})
				awst::pushCallArg(_args, an, u64Var(an));
		};
		auto leafRead = [&](std::shared_ptr<awst::Expression> _slot) {
			auto call = awst::makeSubroutineCall(
				awst::SubroutineID{"__puyasol___evm_dynarr_read"},
				awst::WType::bytesType(), loc);
			awst::pushCallArg(call->args, "__slot", std::move(_slot));
			metricArgs(call->args);
			return std::shared_ptr<awst::Expression>(std::move(call));
		};
		auto leafWriteStmt = [&](std::shared_ptr<awst::Expression> _slot,
			std::shared_ptr<awst::Expression> _bytes) {
			auto call = awst::makeSubroutineCall(
				awst::SubroutineID{"__puyasol___evm_dynarr_write"},
				awst::WType::voidType(), loc);
			awst::pushCallArg(call->args, "__slot", std::move(_slot));
			awst::pushCallArg(call->args, "__val", std::move(_bytes));
			metricArgs(call->args);
			return awst::makeExpressionStatement(std::move(call), loc);
		};
		auto nextDepth = [&]() {
			return awst::makeUInt64BinOp(u64Var("__depth"),
				awst::UInt64BinaryOperator::Sub, u64c(1), loc);
		};
		auto innerRead = [&](std::shared_ptr<awst::Expression> _slot) {
			auto call = awst::makeSubroutineCall(
				awst::SubroutineID{"__puyasol___evm_dynarr_recursive_read"},
				awst::WType::bytesType(), loc);
			awst::pushCallArg(call->args, "__slot", std::move(_slot));
			awst::pushCallArg(call->args, "__depth", nextDepth());
			metricArgs(call->args);
			return std::shared_ptr<awst::Expression>(std::move(call));
		};
		auto innerWriteStmt = [&](std::shared_ptr<awst::Expression> _slot,
			std::shared_ptr<awst::Expression> _bytes) {
			auto call = awst::makeSubroutineCall(
				awst::SubroutineID{"__puyasol___evm_dynarr_recursive_write"},
				awst::WType::voidType(), loc);
			awst::pushCallArg(call->args, "__slot", std::move(_slot));
			awst::pushCallArg(call->args, "__val", std::move(_bytes));
			awst::pushCallArg(call->args, "__depth", nextDepth());
			metricArgs(call->args);
			return awst::makeExpressionStatement(std::move(call), loc);
		};
		auto elemSlotJ = [&]() {
			return awst::makeBigUIntBinOp(biguintVar("__chunk"),
				awst::BigUIntBinaryOperator::Add,
				u64ToBiguint(u64Var("__j")), loc);
		};
		auto u16Of = [&](std::shared_ptr<awst::Expression> _v) {
			return awst::makeExtract(awst::makeItob(std::move(_v), loc), 6, 2, loc);
		};
		auto incJ = [&]() {
			return awst::makeAssignmentStatement(u64Var("__j"),
				awst::makeUInt64BinOp(u64Var("__j"),
					awst::UInt64BinaryOperator::Add, u64c(1), loc), loc);
		};
		auto mkArgs = [&](awst::ContractMethod& _sub, bool _withVal) {
			awst::SubroutineArgument sa;
			sa.name = "__slot";
			sa.wtype = awst::WType::biguintType();
			sa.sourceLocation = loc;
			_sub.args.push_back(sa);
			if (_withVal)
			{
				awst::SubroutineArgument va;
				va.name = "__val";
				va.wtype = awst::WType::bytesType();
				va.sourceLocation = loc;
				_sub.args.push_back(va);
			}
			awst::SubroutineArgument depth;
			depth.name = "__depth";
			depth.wtype = awst::WType::uint64Type();
			depth.sourceLocation = loc;
			_sub.args.push_back(depth);
			for (char const* an: {"__size", "__aw", "__per", "__mul", "__bp"})
			{
				awst::SubroutineArgument a;
				a.name = an;
				a.wtype = awst::WType::uint64Type();
				a.sourceLocation = loc;
				_sub.args.push_back(a);
			}
		};

		// READ
		{
			awst::ContractMethod sub;
			sub.sourceLocation = loc;
			sub.cref = cref;
			sub.memberName = "__evm_dynarr_recursive_read";
			sub.returnType = awst::WType::bytesType();
			sub.arc4MethodConfig = std::nullopt;
			sub.pure = false;
			mkArgs(sub, /*_withVal=*/false);
			auto body = awst::makeBlock(loc);
			{
				auto base = awst::makeBlock(loc);
				base->body.push_back(awst::makeReturnStatement(
					leafRead(slotVar()), loc));
				body->body.push_back(awst::makeIfElse(
					awst::makeNumericCompare(u64Var("__depth"),
						awst::NumericComparison::Lte, u64c(1), loc),
					std::move(base), nullptr, loc));
			}
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__n"), biguintToU64(readWordCall(slotVar())), loc));
			body->body.push_back(awst::makeAssignmentStatement(
				biguintVar("__chunk"), chunkBase(), loc));
			body->body.push_back(awst::makeAssignmentStatement(
				bytesVar("__heads"), awst::makeBytesConstant({}, loc), loc));
			body->body.push_back(awst::makeAssignmentStatement(
				bytesVar("__tails"), awst::makeBytesConstant({}, loc), loc));
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__off"), awst::makeUInt64BinOp(u64c(2),
					awst::UInt64BinaryOperator::Mult, u64Var("__n"), loc), loc));
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__j"), u64c(0), loc));
			auto cond = awst::makeNumericCompare(u64Var("__j"),
				awst::NumericComparison::Lt, u64Var("__n"), loc);
			auto loop = awst::makeBlock(loc);
			loop->body.push_back(awst::makeAssignmentStatement(
				bytesVar("__heads"),
				awst::makeConcat(bytesVar("__heads"),
					u16Of(u64Var("__off")), loc), loc));
			loop->body.push_back(awst::makeAssignmentStatement(
				bytesVar("__inner"), innerRead(elemSlotJ()), loc));
			loop->body.push_back(awst::makeAssignmentStatement(
				bytesVar("__tails"),
				awst::makeConcat(bytesVar("__tails"), bytesVar("__inner"),
					loc), loc));
			loop->body.push_back(awst::makeAssignmentStatement(
				u64Var("__off"), awst::makeUInt64BinOp(u64Var("__off"),
					awst::UInt64BinaryOperator::Add,
					awst::makeLen(bytesVar("__inner"), loc), loc), loc));
			loop->body.push_back(incJ());
			body->body.push_back(awst::makeWhileLoop(
				std::move(cond), std::move(loop), loc));
			auto ret = awst::makeReturnStatement(
				awst::makeConcat(u16Of(u64Var("__n")),
					awst::makeConcat(bytesVar("__heads"), bytesVar("__tails"),
						loc), loc), loc);
			body->body.push_back(std::move(ret));
			sub.body = body;
			_contractNode->methods.push_back(std::move(sub));
		}

		// WRITE
		{
			awst::ContractMethod sub;
			sub.sourceLocation = loc;
			sub.cref = cref;
			sub.memberName = "__evm_dynarr_recursive_write";
			sub.returnType = awst::WType::voidType();
			sub.arc4MethodConfig = std::nullopt;
			sub.pure = false;
			mkArgs(sub, /*_withVal=*/true);
			auto valVar2 = [&]() {
				return awst::makeVarExpression("__val", awst::WType::bytesType(), loc);
			};
			auto headAbs = [&](std::shared_ptr<awst::Expression> _idx) {
				// absolute byte start of element _idx: 2 + head (head is
				// relative to the tuple start at byte 2)
				return awst::makeUInt64BinOp(u64c(2),
					awst::UInt64BinaryOperator::Add,
					awst::makeBtoi(awst::makeExtract3(valVar2(),
						awst::makeUInt64BinOp(u64c(2),
							awst::UInt64BinaryOperator::Add,
							awst::makeUInt64BinOp(u64c(2),
								awst::UInt64BinaryOperator::Mult,
								std::move(_idx), loc), loc),
						u64c(2), loc), loc), loc);
			};
			auto body = awst::makeBlock(loc);
			{
				auto base = awst::makeBlock(loc);
				base->body.push_back(leafWriteStmt(slotVar(), valVar2()));
				base->body.push_back(awst::makeReturnStatement(nullptr, loc));
				body->body.push_back(awst::makeIfElse(
					awst::makeNumericCompare(u64Var("__depth"),
						awst::NumericComparison::Lte, u64c(1), loc),
					std::move(base), nullptr, loc));
			}
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__old"), biguintToU64(readWordCall(slotVar())), loc));
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__n"),
				awst::makeBtoi(awst::makeExtract(valVar2(), 0, 2, loc), loc),
				loc));
			body->body.push_back(writeWordStmt(slotVar(),
				u64ToBiguint(u64Var("__n"))));
			body->body.push_back(awst::makeAssignmentStatement(
				biguintVar("__chunk"), chunkBase(), loc));
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__j"), u64c(0), loc));
			{
				auto cond = awst::makeNumericCompare(u64Var("__j"),
					awst::NumericComparison::Lt, u64Var("__n"), loc);
				auto loop = awst::makeBlock(loc);
				loop->body.push_back(awst::makeAssignmentStatement(
					u64Var("__hs"), headAbs(u64Var("__j")), loc));
				auto lastJ = awst::makeNumericCompare(
					awst::makeUInt64BinOp(u64Var("__j"),
						awst::UInt64BinaryOperator::Add, u64c(1), loc),
					awst::NumericComparison::Lt, u64Var("__n"), loc);
				loop->body.push_back(awst::makeAssignmentStatement(
					u64Var("__he"),
					awst::makeConditional(std::move(lastJ),
						headAbs(awst::makeUInt64BinOp(u64Var("__j"),
							awst::UInt64BinaryOperator::Add, u64c(1), loc)),
						awst::makeLen(valVar2(), loc),
						awst::WType::uint64Type(), loc), loc));
				loop->body.push_back(innerWriteStmt(elemSlotJ(),
					awst::makeExtract3(valVar2(), u64Var("__hs"),
						awst::makeUInt64BinOp(u64Var("__he"),
							awst::UInt64BinaryOperator::Sub, u64Var("__hs"),
							loc), loc)));
				loop->body.push_back(incJ());
				body->body.push_back(awst::makeWhileLoop(
					std::move(cond), std::move(loop), loc));
			}
			// shrink-clear: writing an EMPTY inner array clears its length and
			// stale words
			{
				auto cond = awst::makeNumericCompare(u64Var("__j"),
					awst::NumericComparison::Lt, u64Var("__old"), loc);
				auto loop = awst::makeBlock(loc);
				loop->body.push_back(innerWriteStmt(elemSlotJ(),
					awst::makeBytesConstant({0, 0}, loc)));
				loop->body.push_back(incJ());
				body->body.push_back(awst::makeWhileLoop(
					std::move(cond), std::move(loop), loc));
			}
			sub.body = body;
			_contractNode->methods.push_back(std::move(sub));
		}
	};
	buildNestedDynamicArrayMethods();

	// Library/free-function callers cannot use InstanceMethodTarget, so runtime
	// helpers are roots rather than contract methods.
	storage_dispatch::promoteMethods(*_contractNode, m_dispatchSubroutines, "__puyasol_",
		{"__storage_read", "__storage_write",
			"__evm_bytes_read", "__evm_bytes_write",
			"__evm_dynarr_read", "__evm_dynarr_write",
			"__evm_dynarr_recursive_read", "__evm_dynarr_recursive_write"});

	Logger::instance().debug(
		"Generated EVM-slot __storage_read/__storage_write (paged<"
		+ std::to_string(kEvmDenseSlotLimit) + "/sparse) for "
		+ std::to_string(layout.totalSlots()) + " dense slots", loc);
}

} // namespace puyasol::builder
