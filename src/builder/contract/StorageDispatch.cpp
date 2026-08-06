#include "builder/contract/ContractBuilder.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/SlotWordCodec.h"
#include "builder/storage/SlotHandleAccess.h"

#include <libsolidity/ast/Types.h>
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <algorithm>
#include <set>

namespace puyasol::builder
{

namespace {
/// Recursive visitor for any InlineAssembly node (including deeply nested).
struct InlineAsmDetector: public solidity::frontend::ASTConstVisitor
{
	bool found = false;
	bool visit(solidity::frontend::InlineAssembly const&) override
	{ found = true; return false; }
};
}

void ContractBuilder::buildEvmSlotStorageDispatch(
	solidity::frontend::ContractDefinition const& _contract,
	awst::Contract* _contractNode,
	std::string const& _contractName
)
{
	StorageLayout layout;
	layout.computeLayout(_contract, m_typeMapper);

	InlineAsmDetector asmDetector;
	forEachDefinedFunction(_contract, [&](auto const* func)
	{
		if (asmDetector.found) return;
		if (func->isImplemented())
			func->body().accept(asmDetector);
	});

	if (layout.totalSlots() == 0 && !asmDetector.found)
		return;

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
	std::string const s64Name = "__eslot64";
	auto s64Var = [&]() {
		return awst::makeVarExpression(s64Name, awst::WType::uint64Type(), loc);
	};
	auto bindS64 = [&](awst::Block& _blk) {
		auto cast = awst::makeAsBytes(slotVar(), loc);
		auto cat = awst::makeLeftPad(std::move(cast), 8, loc);
		auto extract = awst::makeExtractLastN(std::move(cat), 8, loc);
		_blk.body.push_back(awst::makeAssignmentStatement(
			s64Var(), awst::makeBtoi(std::move(extract), loc), loc));
	};
	auto pageKey = [&]() {
		auto page = awst::makeUInt64BinOp(s64Var(),
			awst::UInt64BinaryOperator::FloorDiv,
			makeUint64(std::to_string(kEvmSlotsPerPage)), loc);
		return awst::makeConcat(makeBytes("p:"),
			awst::makeItob(std::move(page), loc), loc);
	};
	auto pageOff = [&]() {
		auto idx = awst::makeUInt64BinOp(s64Var(),
			awst::UInt64BinaryOperator::Mod,
			makeUint64(std::to_string(kEvmSlotsPerPage)), loc);
		return awst::makeUInt64BinOp(std::move(idx),
			awst::UInt64BinaryOperator::Mult, makeUint64("32"), loc);
	};
	auto sparseKey = [&]() {
		// page = slot / 64 over the FULL 256-bit slot number
		auto page = awst::makeBigUIntBinOp(slotVar(),
			awst::BigUIntBinaryOperator::FloorDiv,
			awst::makeBiguintConstant(std::to_string(kEvmSlotsPerPage), loc), loc);
		auto slotBytes = awst::makeLeftPadToN(
			awst::makeAsBytes(std::move(page), loc), 32, loc);
		return awst::makeConcat(makeBytes("s:"), std::move(slotBytes), loc);
	};
	// byte offset of this slot inside its sparse page
	auto sparseOff = [&]() {
		auto idx = awst::makeBigUIntBinOp(slotVar(),
			awst::BigUIntBinaryOperator::Mod,
			awst::makeBiguintConstant(std::to_string(kEvmSlotsPerPage), loc), loc);
		auto asU64 = awst::makeBtoi(
			awst::makeExtractLastN(
				awst::makeLeftPad(awst::makeAsBytes(std::move(idx), loc), 8, loc),
				8, loc), loc);
		return awst::makeUInt64BinOp(std::move(asU64),
			awst::UInt64BinaryOperator::Mult, makeUint64("32"), loc);
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
	{
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

		// Sparse: PAGED like the dense region (64 slots/box) so a dynamic
		// array's contiguous elements share box references instead of needing
		// one each; absent page reads as 0.
		auto sparseBlk = awst::makeBlock(loc);
		{
			auto key = sparseKey();
			auto thenBlk = awst::makeBlock(loc);
			thenBlk->body.push_back(awst::makeReturnStatement(
				awst::makeAsBiguint(
					awst::makeBoxExtract(key, sparseOff(), makeUint64("32"), loc), loc),
				loc));
			sparseBlk->body.push_back(awst::makeIfElse(
				boxExists(key), std::move(thenBlk), nullptr, loc));
			retZero(*sparseBlk);
		}

		body->body.push_back(awst::makeIfElse(
			denseCmp(), std::move(denseBlk), std::move(sparseBlk), loc));

		readSub.body = body;
		_contractNode->methods.push_back(std::move(readSub));
	}

	// ── __storage_write(slot: biguint, value: biguint) -> void ──
	{
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

		// Sparse: one PAGE box per 64 slots (box_create is a no-op when the box
		// already exists, so the first write in a page allocates the full page).
		auto sparseBlk = awst::makeBlock(loc);
		{
			auto key = sparseKey();
			sparseBlk->body.push_back(awst::makeExpressionStatement(
				awst::makeBoxCreate(key,
					makeUint64(std::to_string(kEvmSlotsPerPage * 32)), loc), loc));
			auto boxReplace = awst::makeIntrinsicCall("box_replace", awst::WType::voidType(), loc);
			boxReplace->stackArgs.push_back(key);
			boxReplace->stackArgs.push_back(sparseOff());
			boxReplace->stackArgs.push_back(paddedVal());
			sparseBlk->body.push_back(
				awst::makeExpressionStatement(std::move(boxReplace), loc));
			sparseBlk->body.push_back(awst::makeReturnStatement(nullptr, loc));
		}

		body->body.push_back(awst::makeIfElse(
			denseCmp(), std::move(denseBlk), std::move(sparseBlk), loc));

		writeSub.body = body;
		_contractNode->methods.push_back(std::move(writeSub));
	}

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
	{
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
	}

	// ── __evm_bytes_write(slot: biguint, val: bytes) -> void ──
	{
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
	}

	// ── __evm_dynarr_read(slot: biguint) -> bytes ──
	// Materialise a dynamic array of 32-byte-encoded elements as its ARC4
	// form [u16 count][elems]: count word at the slot, elements at
	// keccak256(slot32)+i. Callers cap/validate element width.
	{
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
		for (char const* an: {"__size", "__aw", "__per"})
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
		body->body.push_back(awst::makeAssignmentStatement(
			biguintVar("__chunk"), chunkBase(), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			bytesVar("__data"),
			awst::makeExtract(awst::makeItob(u64Var("__n"), loc), 6, 2, loc), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__i"), u64c(0), loc));
		auto cond = awst::makeNumericCompare(u64Var("__i"),
			awst::NumericComparison::Lt, u64Var("__n"), loc);
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
		loop->body.push_back(awst::makeAssignmentStatement(bytesVar("__wb"),
			awst::makeLeftPadToN(awst::makeAsBytes(readWordCall(
				awst::makeBigUIntBinOp(biguintVar("__chunk"),
					awst::BigUIntBinaryOperator::Add,
					u64ToBiguint(u64Var("__wi")), loc)), loc), 32, loc), loc));
		auto offExpr = awst::makeUInt64BinOp(u64c(32),
			awst::UInt64BinaryOperator::Sub,
			awst::makeUInt64BinOp(
				awst::makeUInt64BinOp(u64Var("__j"),
					awst::UInt64BinaryOperator::Add, u64c(1), loc),
				awst::UInt64BinaryOperator::Mult, u64Var("__size"), loc), loc);
		loop->body.push_back(awst::makeAssignmentStatement(bytesVar("__el"),
			awst::makeExtract3(bytesVar("__wb"), std::move(offExpr),
				u64Var("__size"), loc), loc));
		// left-pad the stored bytes up to the ARC4 element width
		loop->body.push_back(awst::makeAssignmentStatement(
			bytesVar("__data"),
			awst::makeConcat(bytesVar("__data"),
				awst::makeConcat(
					awst::makeBzero(
						awst::makeUInt64BinOp(u64Var("__aw"),
							awst::UInt64BinaryOperator::Sub,
							u64Var("__size"), loc), loc),
					bytesVar("__el"), loc), loc), loc));
		loop->body.push_back(awst::makeAssignmentStatement(u64Var("__i"),
			awst::makeUInt64BinOp(u64Var("__i"),
				awst::UInt64BinaryOperator::Add, u64c(1), loc), loc));
		body->body.push_back(awst::makeWhileLoop(std::move(cond), std::move(loop), loc));
		body->body.push_back(awst::makeReturnStatement(bytesVar("__data"), loc));

		sub.body = body;
		_contractNode->methods.push_back(std::move(sub));
	}

	// ── __evm_dynarr_write(slot: biguint, val: bytes) -> void ──
	// Inverse of __evm_dynarr_read: val is the ARC4 form [u16 count][32B
	// elems]. Writes the length word at the slot, elements at
	// keccak256(slot32)+i, and CLEARS the old tail when the array shrinks —
	// EVM assignment semantics, and a later push must see zeroed slots.
	{
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
		for (char const* an: {"__size", "__aw", "__per"})
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
		body->body.push_back(writeWordStmt(slotVar(),
			u64ToBiguint(u64Var("__n"))));
		body->body.push_back(awst::makeAssignmentStatement(
			biguintVar("__chunk"), chunkBase(), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__i"), u64c(0), loc));
		// write the new elements (packed: rebuild each word from zero at its
		// first element, so a partially-filled last word has clean high bytes)
		{
			auto cond = awst::makeNumericCompare(u64Var("__i"),
				awst::NumericComparison::Lt, u64Var("__n"), loc);
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
			// element bytes from the ARC4 payload: low `size` of its `aw` slice
			auto vOff = awst::makeUInt64BinOp(
				awst::makeUInt64BinOp(u64Var("__i"),
					awst::UInt64BinaryOperator::Mult, u64Var("__aw"), loc),
				awst::UInt64BinaryOperator::Add,
				awst::makeUInt64BinOp(u64c(2),
					awst::UInt64BinaryOperator::Add,
					awst::makeUInt64BinOp(u64Var("__aw"),
						awst::UInt64BinaryOperator::Sub, u64Var("__size"), loc),
					loc), loc);
			loop->body.push_back(awst::makeAssignmentStatement(bytesVar("__el"),
				awst::makeExtract3(valVar(), std::move(vOff),
					u64Var("__size"), loc), loc));
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
					awst::makeUInt64BinOp(u64Var("__n"),
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
				u64Var("__wi"), ceilDiv(u64Var("__n")), loc));
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__we"), ceilDiv(u64Var("__old")), loc));
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
	}

	// Promote to root-level Subroutines (see buildStorageDispatch's tail).
	std::vector<awst::ContractMethod> remainingMethods;
	for (auto& m: _contractNode->methods)
	{
		if (m.memberName == "__storage_read" || m.memberName == "__storage_write"
			|| m.memberName == "__evm_bytes_read" || m.memberName == "__evm_bytes_write"
			|| m.memberName == "__evm_dynarr_read"
			|| m.memberName == "__evm_dynarr_write")
		{
			auto sub = awst::makeSubroutine(
				std::string("__puyasol_") + m.memberName, m.memberName,
				m.args, m.returnType, m.body, /*pure=*/false, m.sourceLocation);
			m_dispatchSubroutines.push_back(std::move(sub));
		}
		else
			remainingMethods.push_back(std::move(m));
	}
	_contractNode->methods = std::move(remainingMethods);

	Logger::instance().debug(
		"Generated EVM-slot __storage_read/__storage_write (paged<"
		+ std::to_string(kEvmDenseSlotLimit) + "/sparse) for "
		+ std::to_string(layout.totalSlots()) + " dense slots", loc);
}

void ContractBuilder::buildStorageDispatch(
	solidity::frontend::ContractDefinition const& _contract,
	awst::Contract* _contractNode,
	std::string const& _contractName
)
{
	if (evmStorageLayout())
	{
		buildEvmSlotStorageDispatch(_contract, _contractNode, _contractName);
		return;
	}

	StorageLayout layout;
	layout.computeLayout(_contract, m_typeMapper);

	InlineAsmDetector asmDetector;
	forEachDefinedFunction(_contract, [&](auto const* func)
	{
		if (asmDetector.found) return;
		if (func->isImplemented())
			func->body().accept(asmDetector);
	});

	if (layout.totalSlots() == 0 && !asmDetector.found)
		return;

	std::string cref = m_sourceFile + "." + _contractName;
	awst::SourceLocation loc;
	loc.file = m_sourceFile;

	auto makeUint64 = [&](std::string const& val) {
		auto c = awst::makeIntegerConstant(val, loc);
		return c;
	};

	auto makeBytes = [&](std::string const& s) {
		return awst::makeUtf8BytesConstant(s, loc);
	};

	// EVM slot arithmetic wraps mod 2^256 (boundary fixtures repoint an array to
	// 2^256-5 so base+idx crosses zero and lands on named vars). biguint add does
	// NOT wrap, so reduce the incoming slot up front — the old uint64 truncation
	// used to provide this wrap by accident.
	auto makeSlotWrapStmt = [&]() {
		auto slotVar = awst::makeVarExpression("__slot", awst::WType::biguintType(), loc);
		auto wrapped = awst::makeBigUIntBinOp(
			awst::makeVarExpression("__slot", awst::WType::biguintType(), loc),
			awst::BigUIntBinaryOperator::Mod,
			awst::makeBiguintConstant(
				"115792089237316195423570985008687907853269984665640564039457584007913129639936",
				loc),
			loc);
		return awst::makeAssignmentStatement(std::move(slotVar), std::move(wrapped), loc);
	};

	// ── Packed-slot codec ────────────────────────────────────────────────────
	// EVM packs multiple sub-word vars into one 32-byte slot; our model stores
	// each var in its OWN typed cell (uint64 global / canonical-TC biguint /
	// bool / bytes[N] / account). sload must ASSEMBLE the EVM word from those
	// cells, sstore must SPLIT the word back — through each var's native repr
	// with exact inverse transforms (mirrors TransientStorage's blob codec).
	// A var at low-order byteOffset o, size s occupies big-endian word bytes
	// [32-o-s, 32-o).

	// The var's packed field: s big-endian bytes of its EVM-slot content
	// (typed cell read → SlotWordCodec).
	auto packedFieldBytes = [&](SlotVariable const* v) -> std::shared_ptr<awst::Expression> {
		auto read = m_storageMapper.createStateRead(
			v->name, v->wtype, awst::AppStorageKind::AppGlobal, loc);
		return SlotWordCodec::nativeToPackedBytes(std::move(read), v->wtype, v->byteSize, loc);
	};

	// Assemble the full 32-byte word for a packed slot (gaps zero-filled).
	auto packedWordBytes = [&](SlotInfo const& si) -> std::shared_ptr<awst::Expression> {
		std::vector<SlotVariable const*> vars;
		for (auto const* v: si.variables)
			if (v && v->wtype && v->wtype != awst::WType::voidType())
				vars.push_back(v);
		std::sort(vars.begin(), vars.end(), [](auto const* a, auto const* b) {
			return a->byteOffset > b->byteOffset;   // BE left→right
		});
		std::shared_ptr<awst::Expression> word;
		auto append = [&](std::shared_ptr<awst::Expression> piece) {
			word = word ? awst::makeConcat(std::move(word), std::move(piece), loc) : std::move(piece);
		};
		unsigned cursor = 0;
		for (auto const* v: vars)
		{
			unsigned start = 32 - v->byteOffset - v->byteSize;
			if (start > cursor)
				append(awst::makeBytesConstant(std::vector<uint8_t>(start - cursor, 0), loc));
			append(packedFieldBytes(v));
			cursor = start + v->byteSize;
		}
		if (cursor < 32)
			append(awst::makeBytesConstant(std::vector<uint8_t>(32 - cursor, 0), loc));
		return word;
	};

	// Split a stored word into per-var writes (appended to _blk).
	auto emitPackedStore = [&](SlotInfo const& si, awst::Block& _blk) {
		// Bind the padded 32-byte word once — every field extracts from it.
		std::string tmp = "__pk_word_" + std::to_string(awst::NameGen::next("StorageDispatch.pkWord"));
		auto valueVar = awst::makeVarExpression("__value", awst::WType::biguintType(), loc);
		auto padded = awst::makeLeftPadToN(awst::makeAsBytes(std::move(valueVar), loc), 32, loc);
		_blk.body.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(tmp, awst::WType::bytesType(), loc), std::move(padded), loc));
		auto wordVar = [&]() { return awst::makeVarExpression(tmp, awst::WType::bytesType(), loc); };

		for (auto const* v: si.variables)
		{
			if (!v || !v->wtype || v->wtype == awst::WType::voidType())
				continue;
			unsigned sz = v->byteSize;
			unsigned start = 32 - v->byteOffset - sz;
			auto raw = awst::makeExtract(wordVar(), static_cast<int>(start), static_cast<int>(sz), loc);
			auto native = SlotWordCodec::packedBytesToNative(std::move(raw), v->wtype, v->solType, sz, loc);
			if (!native)
				continue;   // codec errored loudly

			auto key = awst::makeUtf8BytesConstant(v->name, loc, awst::WType::stateKeyType());
			auto target = awst::makeAppStateExpression(std::move(key), v->wtype, loc);
			auto assign = awst::makeAssignmentExpression(
				std::move(target), std::move(native), loc, v->wtype);
			_blk.body.push_back(awst::makeExpressionStatement(std::move(assign), loc));
		}
	};

	// State vars stored as BOXES (structs with dynamic members etc.) vs
	// app-globals — struct-slot routing needs the right cell either way.
	std::map<std::string, bool> boxVars;
	std::map<std::string, solidity::frontend::StructType const*> structVars;
	forEachStateVar(_contract, [&](auto const* var)
	{
		if (!var || var->isConstant() || var->immutable()) return;
		boxVars[var->name()] = StorageMapper::shouldUseBoxStorage(*var);
		if (auto const* st = dynamic_cast<solidity::frontend::StructType const*>(var->type()))
			structVars[var->name()] = st;
	});

	// Can SlotWordCodec handle this field? Leaf scalars only — array/struct/
	// mapping members occupy their own slots (solc storageBytes==32 for them),
	// and their slots keep the box-per-slot fallback. byte[N] passes only when
	// the arc4 array length EQUALS the field's packed byte size (a true bytesN;
	// uint8[2] arrays report storageBytes 32 and are rejected here).
	auto codecSupported = [&](SlotHandleAccess::FieldPos const& f) {
		auto const* w = f.wtype;
		if (!w) return false;
		if (w == awst::WType::uint64Type() || w == awst::WType::boolType()
			|| w == awst::WType::biguintType() || w == awst::WType::accountType()
			|| w == awst::WType::arc4BoolType())
			return true;
		if (w->kind() == awst::WTypeKind::ARC4UIntN || w->kind() == awst::WTypeKind::Bytes)
			return true;
		if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(w))
			if (auto const* el = dynamic_cast<awst::ARC4UIntN const*>(sa->elementType()))
				return el->n() == 8
					&& sa->arraySize() == static_cast<int64_t>(f.size);   // bytesN-as-byte[N]
		return false;
	};

	// Read the struct var's cell (typed).
	auto structCellRead = [&](SlotVariable const* v) -> std::shared_ptr<awst::Expression> {
		if (boxVars[v->name])
			return StorageMapper::makeStateGetWithDefault(
				StorageMapper::makeTopLevelBoxExpr(v->name, v->wtype, loc), v->wtype, loc);
		return m_storageMapper.createStateRead(
			v->name, v->wtype, awst::AppStorageKind::AppGlobal, loc);
	};
	auto structCellTarget = [&](SlotVariable const* v) -> std::shared_ptr<awst::Expression> {
		if (boxVars[v->name])
			return StorageMapper::makeTopLevelBoxExpr(v->name, v->wtype, loc);
		auto key = awst::makeUtf8BytesConstant(v->name, loc, awst::WType::stateKeyType());
		return awst::makeAppStateExpression(std::move(key), v->wtype, loc);
	};

	// ── __storage_read(slot: uint64) -> biguint ──
	{
		awst::ContractMethod readSub;
		readSub.sourceLocation = loc;
		readSub.cref = cref;
		readSub.memberName = "__storage_read";
		readSub.returnType = awst::WType::biguintType();
		readSub.arc4MethodConfig = std::nullopt;
		readSub.pure = false;

		awst::SubroutineArgument slotArg;
		slotArg.name = "__slot";
		// FULL 256-bit slot: EVM slots are 2^256-wide (boundary fixtures probe
		// sub(0,5) = 2^256-5; keccak-derived slots are arbitrary). The old uint64
		// arg silently truncated them at every call site.
		slotArg.wtype = awst::WType::biguintType();
		slotArg.sourceLocation = loc;
		readSub.args.push_back(slotArg);

		auto body = awst::makeBlock(loc);
		body->body.push_back(makeSlotWrapStmt());

		// Build if/else chain for known slots (bottom-up; default = dynamic fallback).
		auto defaultBlock = awst::makeBlock(loc);
		{
			// Default: BOX-PER-SLOT keyed by the full 32-byte slot ("s:" ++ slot).
			// Replaces the mod-256 __dyn_storage fold (distinct slots aliased) —
			// arbitrary 256-bit slots now get their own 32-byte cell, matching EVM
			// storage semantics (zero-initialised, no collisions). box_create is a
			// no-op when the box already exists at the same (always 32) size.
			auto slotVar = awst::makeVarExpression("__slot", awst::WType::biguintType(), loc);
			auto slotBytes = awst::makeLeftPadToN(
				awst::makeAsBytes(std::move(slotVar), loc), 32, loc);
			auto key = awst::makeConcat(makeBytes("s:"), std::move(slotBytes), loc);

			auto boxCreate = awst::makeBoxCreate(key, makeUint64("32"), loc);
			defaultBlock->body.push_back(
				awst::makeExpressionStatement(std::move(boxCreate), loc));

			auto boxExtract = awst::makeBoxExtract(
				key, makeUint64("0"), makeUint64("32"), loc);
			auto cast = awst::makeAsBiguint(std::move(boxExtract), loc);
			defaultBlock->body.push_back(
				awst::makeReturnStatement(std::move(cast), loc));
		}

		std::shared_ptr<awst::Statement> current;
		std::shared_ptr<awst::Block> elseBlock = defaultBlock;

		for (auto const& si: layout.slots())
		{
			std::vector<SlotVariable const*> vars;
			for (auto const* v: si.variables)
				if (v && v->wtype && v->wtype != awst::WType::voidType())
					vars.push_back(v);
			if (vars.empty()) continue;

			auto slotVar = awst::makeVarExpression("__slot", awst::WType::biguintType(), loc);
			auto cmp = awst::makeNumericCompare(slotVar, awst::NumericComparison::Eq,
				awst::makeIntegerConstant(si.slotNumber.str(), loc, awst::WType::biguintType()), loc);

			// STRUCT state var: its slots hold packed FIELDS, and the cell is a
			// typed ARC4Struct (box or app-global) — assemble each slot's word
			// from the fields living there. One compare per internal slot whose
			// field group the codec fully supports; others keep the fallback.
			if (vars.size() == 1)
				if (auto stIt = structVars.find(vars[0]->name); stIt != structVars.end())
				{
					auto const* structW = dynamic_cast<awst::ARC4Struct const*>(vars[0]->wtype);
					if (structW)
					{
						auto fields = SlotHandleAccess::fieldPositions(stIt->second, structW);
						unsigned stride = static_cast<unsigned>(stIt->second->storageSize() > 64
							? 64 : static_cast<unsigned>(stIt->second->storageSize()));
						for (unsigned k = 0; k < stride; ++k)
						{
							std::vector<SlotHandleAccess::FieldPos const*> group;
							bool ok = true;
							for (auto const& f: fields)
								if (f.slot == k)
								{
									if (!codecSupported(f) || f.size == 0 || f.size > 32)
										ok = false;
									group.push_back(&f);
								}
							if (!ok || group.empty())
								continue;
							std::sort(group.begin(), group.end(), [](auto const* a, auto const* b) {
								return a->byteOffset > b->byteOffset;
							});
							auto cmpK = awst::makeNumericCompare(
								awst::makeVarExpression("__slot", awst::WType::biguintType(), loc),
								awst::NumericComparison::Eq,
								awst::makeIntegerConstant((si.slotNumber + k).str(), loc,
									awst::WType::biguintType()), loc);
							auto blkK = awst::makeBlock(loc);
							{
								std::shared_ptr<awst::Expression> word;
								auto append = [&](std::shared_ptr<awst::Expression> piece) {
									word = word ? awst::makeConcat(std::move(word), std::move(piece), loc)
												: std::move(piece);
								};
								unsigned cursor = 0;
								for (auto const* f: group)
								{
									unsigned start = 32 - f->byteOffset - f->size;
									if (start > cursor)
										append(awst::makeBytesConstant(
											std::vector<uint8_t>(start - cursor, 0), loc));
									auto fv = awst::makeFieldExpression(
										structCellRead(vars[0]), f->name, f->wtype, loc);
									append(SlotWordCodec::nativeToPackedBytes(
										std::move(fv), f->wtype, f->size, loc));
									cursor = start + f->size;
								}
								if (cursor < 32)
									append(awst::makeBytesConstant(
										std::vector<uint8_t>(32 - cursor, 0), loc));
								blkK->body.push_back(awst::makeReturnStatement(
									awst::makeAsBiguint(std::move(word), loc), loc));
							}
							auto ifElseK = awst::makeIfElse(
								std::move(cmpK), std::move(blkK), std::move(elseBlock), loc);
							auto newElseK = awst::makeBlock(loc);
							newElseK->body.push_back(std::move(ifElseK));
							elseBlock = std::move(newElseK);
						}
						continue;
					}
				}

			auto ifBlock = awst::makeBlock(loc);
			if (si.isDynamic || (vars.size() == 1 && vars[0]->isFullSlot))
			{
				// Full-slot single var (or box-backed dynamic root): raw cell bytes
				// low-aligned into the word — the pre-packing behavior, unchanged.
				auto get = awst::makeIntrinsicCall("app_global_get", awst::WType::bytesType(), loc);
				get->stackArgs.push_back(makeBytes(vars[0]->name));

				// Left-pad + take last 32 bytes (global slots may be <32 for short ints).
				auto cat = awst::makeLeftPad(std::move(get), 32, loc);
				auto extract = awst::makeExtractLastN(std::move(cat), 32, loc);
				auto cast = awst::makeAsBiguint(std::move(extract), loc);

				auto ret = awst::makeReturnStatement(std::move(cast), loc);
				ifBlock->body.push_back(std::move(ret));
			}
			else
			{
				// Packed slot: assemble the EVM word from each var's typed cell.
				auto word = awst::makeAsBiguint(packedWordBytes(si), loc);
				ifBlock->body.push_back(awst::makeReturnStatement(std::move(word), loc));
			}

			auto ifElse = awst::makeIfElse(
				std::move(cmp), std::move(ifBlock), std::move(elseBlock), loc);

			auto newElse = awst::makeBlock(loc);
			newElse->body.push_back(std::move(ifElse));
			elseBlock = std::move(newElse);
		}

		for (auto& stmt: elseBlock->body)
			body->body.push_back(std::move(stmt));

		readSub.body = body;
		_contractNode->methods.push_back(std::move(readSub));
	}

	// ── __storage_write(slot: uint64, value: biguint) -> void ──
	{
		awst::ContractMethod writeSub;
		writeSub.sourceLocation = loc;
		writeSub.cref = cref;
		writeSub.memberName = "__storage_write";
		writeSub.returnType = awst::WType::voidType();
		writeSub.arc4MethodConfig = std::nullopt;
		writeSub.pure = false;

		awst::SubroutineArgument slotArg;
		slotArg.name = "__slot";
		slotArg.wtype = awst::WType::biguintType();   // full 256-bit slot (see __storage_read)
		slotArg.sourceLocation = loc;
		writeSub.args.push_back(slotArg);

		awst::SubroutineArgument valArg;
		valArg.name = "__value";
		valArg.wtype = awst::WType::biguintType();
		valArg.sourceLocation = loc;
		writeSub.args.push_back(valArg);

		auto body = awst::makeBlock(loc);
		body->body.push_back(makeSlotWrapStmt());

		auto defaultBlock = awst::makeBlock(loc);
		{
			// BOX-PER-SLOT (see __storage_read): key = "s:" ++ 32-byte slot.
			auto slotVar = awst::makeVarExpression("__slot", awst::WType::biguintType(), loc);
			auto slotBytes = awst::makeLeftPadToN(
				awst::makeAsBytes(std::move(slotVar), loc), 32, loc);
			auto key = awst::makeConcat(makeBytes("s:"), std::move(slotBytes), loc);

			auto valueVar = awst::makeVarExpression("__value", awst::WType::biguintType(), loc);
			auto paddedVal = awst::makeLeftPadToN(
				awst::makeAsBytes(std::move(valueVar), loc), 32, loc);

			auto boxCreate = awst::makeBoxCreate(key, makeUint64("32"), loc);
			defaultBlock->body.push_back(
				awst::makeExpressionStatement(std::move(boxCreate), loc));

			auto boxReplace = awst::makeIntrinsicCall("box_replace", awst::WType::voidType(), loc);
			boxReplace->stackArgs.push_back(key);
			boxReplace->stackArgs.push_back(makeUint64("0"));
			boxReplace->stackArgs.push_back(std::move(paddedVal));
			defaultBlock->body.push_back(
				awst::makeExpressionStatement(std::move(boxReplace), loc));

			defaultBlock->body.push_back(awst::makeReturnStatement(nullptr, loc));
		}

		std::shared_ptr<awst::Block> elseBlock = defaultBlock;

		for (auto const& si: layout.slots())
		{
			std::vector<SlotVariable const*> vars;
			for (auto const* v: si.variables)
				if (v && v->wtype && v->wtype != awst::WType::voidType())
					vars.push_back(v);
			if (vars.empty()) continue;

			auto slotVar = awst::makeVarExpression("__slot", awst::WType::biguintType(), loc);
			auto cmp = awst::makeNumericCompare(slotVar, awst::NumericComparison::Eq,
				awst::makeIntegerConstant(si.slotNumber.str(), loc, awst::WType::biguintType()), loc);

			// STRUCT state var (see the read side): split the stored word into
			// the slot's fields via COW on the typed cell.
			if (vars.size() == 1)
				if (auto stIt = structVars.find(vars[0]->name); stIt != structVars.end())
				{
					auto const* structW = dynamic_cast<awst::ARC4Struct const*>(vars[0]->wtype);
					if (structW)
					{
						auto fields = SlotHandleAccess::fieldPositions(stIt->second, structW);
						unsigned stride = static_cast<unsigned>(stIt->second->storageSize() > 64
							? 64 : static_cast<unsigned>(stIt->second->storageSize()));
						for (unsigned k = 0; k < stride; ++k)
						{
							std::vector<SlotHandleAccess::FieldPos const*> group;
							bool ok = true;
							for (auto const& f: fields)
								if (f.slot == k)
								{
									if (!codecSupported(f) || f.size == 0 || f.size > 32)
										ok = false;
									group.push_back(&f);
								}
							if (!ok || group.empty())
								continue;
							auto cmpK = awst::makeNumericCompare(
								awst::makeVarExpression("__slot", awst::WType::biguintType(), loc),
								awst::NumericComparison::Eq,
								awst::makeIntegerConstant((si.slotNumber + k).str(), loc,
									awst::WType::biguintType()), loc);
							auto blkK = awst::makeBlock(loc);
							{
								// bind the padded word once
								std::string tmp = "__pk_sw_" + std::to_string(
									awst::NameGen::next("StorageDispatch.pkStructWord"));
								auto valueVar = awst::makeVarExpression(
									"__value", awst::WType::biguintType(), loc);
								blkK->body.push_back(awst::makeAssignmentStatement(
									awst::makeVarExpression(tmp, awst::WType::bytesType(), loc),
									awst::makeLeftPadToN(
										awst::makeAsBytes(std::move(valueVar), loc), 32, loc),
									loc));
								auto wordVar = [&]() {
									return awst::makeVarExpression(tmp, awst::WType::bytesType(), loc);
								};
								// COW: rebuild the struct with this slot's fields replaced
								std::set<std::string> replaced;
								for (auto const* f: group)
									replaced.insert(f->name);
								auto ns = awst::makeNewStruct(structW, loc);
								for (auto const& [fname, ftype]: structW->fields())
								{
									if (replaced.count(fname))
									{
										SlotHandleAccess::FieldPos const* fp = nullptr;
										for (auto const* g: group)
											if (g->name == fname) { fp = g; break; }
										unsigned start = 32 - fp->byteOffset - fp->size;
										auto raw = awst::makeExtract(wordVar(),
											static_cast<int>(start), static_cast<int>(fp->size), loc);
										auto native = SlotWordCodec::packedBytesToNative(
											std::move(raw), fp->wtype, fp->solType, fp->size, loc);
										if (native)
											ns->values[fname] = std::move(native);
									}
									else
										ns->values[fname] = awst::makeFieldExpression(
											structCellRead(vars[0]), fname, ftype, loc);
								}
								blkK->body.push_back(awst::makeExpressionStatement(
									awst::makeAssignmentExpression(
										structCellTarget(vars[0]), std::move(ns), loc,
										vars[0]->wtype), loc));
								blkK->body.push_back(awst::makeReturnStatement(nullptr, loc));
							}
							auto ifElseK = awst::makeIfElse(
								std::move(cmpK), std::move(blkK), std::move(elseBlock), loc);
							auto newElseK = awst::makeBlock(loc);
							newElseK->body.push_back(std::move(ifElseK));
							elseBlock = std::move(newElseK);
						}
						continue;
					}
				}

			auto ifBlock = awst::makeBlock(loc);
			if (si.isDynamic || (vars.size() == 1 && vars[0]->isFullSlot))
			{
				// Full-slot single var: raw 32-byte put — the pre-packing behavior, unchanged.
				auto valueVar = awst::makeVarExpression("__value", awst::WType::biguintType(), loc);
				auto cast = awst::makeAsBytes(std::move(valueVar), loc);
				auto cat = awst::makeLeftPad(std::move(cast), 32, loc);
				auto lenCall = awst::makeLen(cat, loc);
				auto sub32 = awst::makeUInt64BinOp(std::move(lenCall), awst::UInt64BinaryOperator::Sub, makeUint64("32"), loc);

				auto extract = awst::makeExtract3(cat, std::move(sub32), makeUint64("32"), loc);
				auto put = awst::makeAppGlobalPut(makeBytes(vars[0]->name), std::move(extract), loc);

				auto stmt = awst::makeExpressionStatement(std::move(put), loc);
				ifBlock->body.push_back(std::move(stmt));

				auto ret = awst::makeReturnStatement(nullptr, loc);
				ifBlock->body.push_back(std::move(ret));
			}
			else
			{
				// Packed slot: split the word into each var's typed cell.
				emitPackedStore(si, *ifBlock);
				ifBlock->body.push_back(awst::makeReturnStatement(nullptr, loc));
			}

			auto ifElse = awst::makeIfElse(
				std::move(cmp), std::move(ifBlock), std::move(elseBlock), loc);

			auto newElse = awst::makeBlock(loc);
			newElse->body.push_back(std::move(ifElse));
			elseBlock = std::move(newElse);
		}

		for (auto& stmt: elseBlock->body)
			body->body.push_back(std::move(stmt));

		writeSub.body = body;
		_contractNode->methods.push_back(std::move(writeSub));
	}

	// Promote to root-level Subroutines: library/free-function callers can't use
	// InstanceMethodTarget (puya rejects it outside a contract method).
	std::vector<awst::ContractMethod> remainingMethods;
	for (auto& m: _contractNode->methods)
	{
		if (m.memberName == "__storage_read" || m.memberName == "__storage_write")
		{
			auto sub = awst::makeSubroutine(
				std::string("__puyasol_") + m.memberName, m.memberName,
				m.args, m.returnType, m.body, /*pure=*/false, m.sourceLocation);
			m_dispatchSubroutines.push_back(std::move(sub));
		}
		else
		{
			remainingMethods.push_back(std::move(m));
		}
	}
	_contractNode->methods = std::move(remainingMethods);

	Logger::instance().debug(
		"Generated __storage_read/__storage_write dispatch for "
		+ std::to_string(layout.totalSlots()) + " slots", loc);
}


} // namespace puyasol::builder
