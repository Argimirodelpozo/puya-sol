#include "builder/contract/ContractBuilder.h"
#include "builder/context/BuildArtifacts.h"
#include "builder/context/ProgramAnalysis.h"
#include "builder/contract/StorageDispatchSupport.h"
#include "builder/storage/StateVarWalker.h"
#include "builder/target/EvmLayoutMode.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/StorageRuntimePlan.h"
#include "builder/codec/SlotWordCodec.h"
#include "builder/storage/slot/SlotHandleAccess.h"

#include <libsolidity/ast/Types.h>
#include "builder/types/TypeCoercion.h"
#include "builder/types/SolIntType.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <algorithm>
#include <set>

namespace puyasol::builder
{
namespace
{
/// Element-shape parameters of the dynamic-array codec, in argument order.
constexpr char const* kDynarrMetrics[] = {"__size", "__aw", "__per", "__mul", "__bp"};

/// `_args` followed by the five uint64 metric parameters.
std::vector<awst::SubroutineArgument> withMetrics(
	std::vector<awst::SubroutineArgument> _args,
	awst::SourceLocation const& _loc)
{
	for (char const* an: kDynarrMetrics)
		_args.emplace_back(an, awst::WType::uint64Type(), _loc);
	return _args;
}

/// The seven runtime subroutines of the EVM-slot storage model, plus the
/// expression factories they share.
///
/// These were seven immediately-invoked `[&]` lambdas inside one 1,100-line
/// function, so every helper below was a capture and the whole file had to be
/// read as a unit. As members the bodies are unchanged -- `loc`, `cref`,
/// `denseOnly` and the factories resolve as members rather than captures.
struct EvmSlotCodec
{
	TypeMapper& m_typeMapper;
	awst::SourceLocation loc;
	std::string cref;
	/// Every runtime slot is provably < 2^16, so the sparse arms and the
	/// mod-2^256 wrap are dead weight. Each variant has its own root identity.
	bool denseOnly = false;
	/// Dense-only AND <= 64 slots: constant page-0 key and no page-offset mod.
	bool singlePage = false;
	std::string s64Name = "__eslot64";

	auto makeUint64(std::string const& val) const
	{
		return awst::makeIntegerConstant(val, loc);
	}
	auto makeBytes(std::string const& s) const
	{
		return awst::makeUtf8BytesConstant(s, loc);
	}
	auto slotVar() const
	{
		return awst::makeVarExpression("__slot", awst::WType::biguintType(), loc);
	}

	// EVM slot arithmetic wraps mod 2^256 (see buildStorageDispatch).
	auto makeSlotWrapStmt() const
	{
		auto wrapped = awst::makeBigUIntBinOp(
			slotVar(),
			awst::BigUIntBinaryOperator::Mod,
			awst::makeBiguintConstant(
				"115792089237316195423570985008687907853269984665640564039457584007913129639936",
				loc),
			loc);
		return awst::makeAssignmentStatement(slotVar(), std::move(wrapped), loc);
	}

	// __slot < 2^16 → dense region (declared vars), page boxes of 64 slots.
	auto denseCmp() const
	{
		return awst::makeNumericCompare(slotVar(), awst::NumericComparison::Lt,
			awst::makeIntegerConstant(std::to_string(kEvmDenseSlotLimit), loc,
				awst::WType::biguintType()), loc);
	}


	// Bind the uint64 slot, then key = "p:" ++ itob(slot / 64), off = (slot % 64) * 32.
	// Dense-only, single-page layouts (≤64 slots): key is the constant page-0
	// name and the offset drops the mod. The btoi input still needs byte-width
	// normalisation: a small numeric slot may arrive in a 32-byte carrier.

	auto s64Var() const
	{
		return awst::makeVarExpression(s64Name, awst::WType::uint64Type(), loc);
	}
	auto bindS64(awst::Block& _blk) const
	{
		// ALWAYS 8-byte-normalise before btoi: a biguint's byte length is not
		// bounded by its VALUE — slot args arrive 32-byte-padded from the
		// getter path (leftPadToN canonicals), and btoi rejects >8 bytes.
		auto cast = awst::makeAsBytes(slotVar(), loc);
		auto cat = awst::makeLeftPad(std::move(cast), 8, loc);
		auto extract = awst::makeExtractLastN(std::move(cat), 8, loc);
		_blk.body.push_back(awst::makeAssignmentStatement(
			s64Var(), awst::makeBtoi(std::move(extract), loc), loc));
	}
	std::shared_ptr<awst::Expression> pageKey() const
	{
		if (singlePage)
			return awst::makeConcat(makeBytes("p:"),
				awst::makeItob(makeUint64("0"), loc), loc);
		auto page = awst::makeUInt64BinOp(s64Var(),
			awst::UInt64BinaryOperator::FloorDiv,
			makeUint64(std::to_string(kEvmSlotsPerPage)), loc);
		return awst::makeConcat(makeBytes("p:"),
			awst::makeItob(std::move(page), loc), loc);
	}
	std::shared_ptr<awst::Expression> pageOff() const
	{
		if (singlePage)
			return awst::makeUInt64BinOp(s64Var(),
				awst::UInt64BinaryOperator::Mult, makeUint64("32"), loc);
		auto idx = awst::makeUInt64BinOp(s64Var(),
			awst::UInt64BinaryOperator::Mod,
			makeUint64(std::to_string(kEvmSlotsPerPage)), loc);
		return awst::makeUInt64BinOp(std::move(idx),
			awst::UInt64BinaryOperator::Mult, makeUint64("32"), loc);
	}
	auto sparseKey() const
	{
		auto slotBytes = awst::makeLeftPadToN(awst::makeAsBytes(slotVar(), loc), 32, loc);
		return awst::makeConcat(makeBytes("s:"), std::move(slotBytes), loc);
	}
	auto boxExists(std::shared_ptr<awst::Expression> _key) const
	{
		auto boxLen = StorageMapper::makeBoxLenTuple(m_typeMapper, std::move(_key), loc);
		return awst::makeTupleItem(std::move(boxLen), 1, awst::WType::boolType(), loc);
	}
	auto retZero(awst::Block& _blk) const
	{
		_blk.body.push_back(awst::makeReturnStatement(
			awst::makeIntegerConstant("0", loc, awst::WType::biguintType()), loc));
	}

	// ── __storage_read(slot: biguint) -> biguint ──
	void emitStorageRead(awst::Contract* _contractNode) const
	{
		auto readSub = awst::ContractMethod(cref, "__storage_read",
			awst::WType::biguintType(),
			{{"__slot", awst::WType::biguintType(), loc}}, loc);
		auto body = readSub.body;
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

		_contractNode->methods.push_back(std::move(readSub));
	}

	// ── __storage_write(slot: biguint, value: biguint) -> void ──
	void emitStorageWrite(awst::Contract* _contractNode) const
	{
		bool const shadowed = !denseOnly && !m_typeMapper.analysis().packedAddressOffsets.empty();
		if (shadowed)
		{
			auto checked = awst::ContractMethod(cref, "__storage_write", awst::WType::voidType(),
				{{"__slot", awst::WType::biguintType(), loc}, {"__value", awst::WType::biguintType(), loc}}, loc);
			auto& out = checked.body->body;
			out.push_back(makeSlotWrapStmt());
			out.push_back(awst::makeAssignmentStatement(bytesVar("__oldword"), wordBytes(readWordCall(slotVar())), loc));
			out.push_back(awst::makeAssignmentStatement(bytesVar("__newword"), wordBytes(biguintVar("__value")), loc));
			auto rawWrite = [&](auto slot, auto value) {
				auto call = awst::makeSubroutineCall(awst::SubroutineID{"__puyasol___storage_write_word"}, awst::WType::voidType(), loc);
				awst::pushCallArg(call->args, "__slot", std::move(slot));
				awst::pushCallArg(call->args, "__value", std::move(value));
				return awst::makeExpressionStatement(std::move(call), loc);
			};
			for (unsigned offset: m_typeMapper.analysis().packedAddressOffsets)
			{
				auto changed = awst::makeBytesComparison(awst::makeExtract(bytesVar("__oldword"), 12 - offset, 20, loc),
					awst::EqualityComparison::Ne, awst::makeExtract(bytesVar("__newword"), 12 - offset, 20, loc), loc);
				auto body = awst::makeBlock(loc);
				auto aux = biguintVar("__address_aux");
				body->body.push_back(awst::makeAssignmentStatement(aux,
					SlotHandleAccess::packedAddressAuxSlot(slotVar(), u64c(offset), loc), loc));
				auto clear = awst::makeBlock(loc);
				clear->body.push_back(rawWrite(aux, awst::makeBiguintConstant("0", loc)));
				body->body.push_back(awst::makeIfElse(awst::makeNumericCompare(readWordCall(aux),
					awst::NumericComparison::Ne, awst::makeBiguintConstant("0", loc), loc), std::move(clear), nullptr, loc));
				out.push_back(awst::makeIfElse(std::move(changed), std::move(body), nullptr, loc));
			}
			out.push_back(rawWrite(slotVar(), biguintVar("__value")));
			out.push_back(awst::makeReturnStatement(nullptr, loc));
			_contractNode->methods.push_back(std::move(checked));
		}
		auto writeSub = awst::ContractMethod(cref, "__storage_write",
			awst::WType::voidType(),
			{{"__slot", awst::WType::biguintType(), loc},
				{"__value", awst::WType::biguintType(), loc}},
			loc);
		if (shadowed) writeSub.memberName = "__storage_write_word";
		auto body = writeSub.body;
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

		_contractNode->methods.push_back(std::move(writeSub));
	}

	// ── EVM bytes/string storage codec ──────────────────────────────────────
	// Solidity storage format: short (len<32) = data left-aligned ++ 2*len in
	// the low byte, all in the slot word; long = word 2*len+1 at the slot,
	// data in 32-byte chunks at keccak256(slot32)+i.
	std::shared_ptr<awst::Expression> readWordCall(std::shared_ptr<awst::Expression> _slot) const
	{
		auto call = awst::makeSubroutineCall(
			awst::SubroutineID{"__puyasol___storage_read"},
			awst::WType::biguintType(), loc);
		awst::pushCallArg(call->args, "__slot", std::move(_slot));
		return std::shared_ptr<awst::Expression>(std::move(call));
	}
	auto writeWordStmt(std::shared_ptr<awst::Expression> _slot, std::shared_ptr<awst::Expression> _word) const
	{
		auto call = awst::makeSubroutineCall(
			awst::SubroutineID{"__puyasol___storage_write"},
			awst::WType::voidType(), loc);
		awst::pushCallArg(call->args, "__slot", std::move(_slot));
		awst::pushCallArg(call->args, "__value", std::move(_word));
		return awst::makeExpressionStatement(std::move(call), loc);
	}
	auto chunkBase() const
	{
		// keccak256(slot32) as biguint — the data region of the long form.
		auto slotBytes = awst::makeLeftPadToN(
			awst::makeAsBytes(slotVar(), loc), 32, loc);
		return awst::makeAsBiguint(
			awst::makeKeccak256(std::move(slotBytes), loc), loc);
	}
	auto u64Var(std::string const& n) const
	{
		return awst::makeVarExpression(n, awst::WType::uint64Type(), loc);
	}
	std::shared_ptr<awst::VarExpression> bytesVar(std::string const& n) const
	{
		return awst::makeVarExpression(n, awst::WType::bytesType(), loc);
	}
	std::shared_ptr<awst::VarExpression> biguintVar(std::string const& n) const
	{
		return awst::makeVarExpression(n, awst::WType::biguintType(), loc);
	}
	std::shared_ptr<awst::IntegerConstant> u64c(uint64_t v) const { return awst::makeIntegerConstant(v, loc); }
	auto u64ToBiguint(std::shared_ptr<awst::Expression> e) const
	{
		return awst::makeAsBiguint(awst::makeItob(std::move(e), loc), loc);
	}

	/// A storage word as its 32-byte big-endian form.
	std::shared_ptr<awst::Expression> wordBytes(std::shared_ptr<awst::Expression> _word) const
	{
		return awst::makeLeftPadToN(awst::makeAsBytes(std::move(_word), loc), 32, loc);
	}
	/// Data word `_idx` (uint64 var) of the keccak region: __chunk + idx.
	auto chunkAt(std::string const& _idx) const
	{
		return awst::makeBigUIntBinOp(biguintVar("__chunk"),
			awst::BigUIntBinaryOperator::Add, u64ToBiguint(u64Var(_idx)), loc);
	}
	/// `_v = _v + 1`
	auto inc(std::string const& _v) const
	{
		return awst::makeAssignmentStatement(u64Var(_v),
			awst::makeUInt64BinOp(u64Var(_v),
				awst::UInt64BinaryOperator::Add, u64c(1), loc), loc);
	}

	// ── __evm_bytes_read(slot: biguint) -> bytes ──
	// ── __evm_bytes_write(slot: biguint, val: bytes) -> void ──
	// One short/long layout feeds both directions: the slot word's low-byte
	// parity picks the form, the long length is (word-1)/2, chunk i lives at
	// keccak256(slot32)+i. A write loads the OLD word first (stale-chunk
	// cleanup needs the previous length) and zeroes the long chunks beyond
	// the new count, as the EVM does on shrink.
	void emitBytesCodec(awst::Contract* _contractNode, bool _write) const
	{
		std::vector<awst::SubroutineArgument> args{
			{"__slot", awst::WType::biguintType(), loc}};
		if (_write)
			args.emplace_back("__val", awst::WType::bytesType(), loc);
		auto sub = awst::ContractMethod(cref,
			_write ? "__evm_bytes_write" : "__evm_bytes_read",
			_write ? awst::WType::voidType() : awst::WType::bytesType(),
			std::move(args), loc);
		auto body = sub.body;
		auto valVar = [&]() { return bytesVar("__val"); };
		// low byte of the padded word `_w`, and its parity (even = short form)
		auto lastByte = [&](char const* _w) {
			return awst::makeBtoi(awst::makeExtract(bytesVar(_w), 31, 1, loc), loc);
		};
		auto parityIs = [&](std::shared_ptr<awst::Expression> _v, uint64_t _bit) {
			return awst::makeNumericCompare(
				awst::makeUInt64BinOp(std::move(_v),
					awst::UInt64BinaryOperator::Mod, u64c(2), loc),
				awst::NumericComparison::Eq, u64c(_bit), loc);
		};
		// ceil(len / 32) chunks
		auto chunkCount = [&](std::shared_ptr<awst::Expression> _len) {
			return awst::makeUInt64BinOp(
				awst::makeUInt64BinOp(std::move(_len),
					awst::UInt64BinaryOperator::Add, u64c(31), loc),
				awst::UInt64BinaryOperator::FloorDiv, u64c(32), loc);
		};
		// while (_cond) { _stmt; _idx += 1 }
		auto chunkLoop = [&](std::shared_ptr<awst::Expression> _cond,
			std::shared_ptr<awst::Statement> _stmt, char const* _idx) {
			auto loop = awst::makeBlock(loc);
			loop->body.push_back(std::move(_stmt));
			loop->body.push_back(inc(_idx));
			return awst::makeWhileLoop(std::move(_cond), std::move(loop), loc);
		};

		if (_write)
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__len"), awst::makeLen(valVar(), loc), loc));
		// the padded slot word: the value read, or the OLD word of a write
		char const* word = _write ? "__ow" : "__wb";
		body->body.push_back(awst::makeAssignmentStatement(
			bytesVar(word), wordBytes(readWordCall(slotVar())), loc));
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__last"), lastByte(word), loc));
		auto isShort = parityIs(u64Var("__last"), 0);
		auto shortLen = awst::makeUInt64BinOp(u64Var("__last"),
			awst::UInt64BinaryOperator::FloorDiv, u64c(2), loc);
		auto longLen = awst::makeBigUIntBinOp(awst::makeAsBiguint(bytesVar(word), loc),
			awst::BigUIntBinaryOperator::FloorDiv, awst::makeBiguintConstant("2", loc), loc);
		body->body.push_back(awst::makeAssignmentStatement(biguintVar("__storedLen"),
			awst::makeConditional(isShort, awst::makeAsBiguint(awst::makeItob(shortLen, loc), loc),
				std::move(longLen), awst::WType::biguintType(), loc), loc));
		// solc extract_byte_array_length: short lengths must be <32 and
		// long lengths >=32. Validate the full word before any uint64 cast.
		body->body.push_back(awst::makeExpressionStatement(awst::makeAssert(
			awst::makeConditional(isShort,
				awst::makeNumericCompare(biguintVar("__storedLen"), awst::NumericComparison::Lt, awst::makeBiguintConstant("32", loc), loc),
				awst::makeNumericCompare(biguintVar("__storedLen"), awst::NumericComparison::Gte, awst::makeBiguintConstant("32", loc), loc),
				awst::WType::boolType(), loc), loc, "invalid bytes storage encoding (Panic 0x22)"), loc));
		auto storedLen = TypeCoercion::checkedAllocationSizeToUint64(
			body->body, biguintVar("__storedLen"), loc);
		if (!_write)
		{
			// short form: even last byte → len = last/2, data = wb[0:len]
			auto thenBlk = awst::makeBlock(loc);
			thenBlk->body.push_back(awst::makeReturnStatement(
				awst::makeExtract3(bytesVar(word), u64c(0), shortLen, loc),
				loc));
			body->body.push_back(awst::makeIfElse(
				isShort, std::move(thenBlk), nullptr, loc));
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__len"), storedLen, loc));
		}
		body->body.push_back(awst::makeAssignmentStatement(
			biguintVar("__chunk"), chunkBase(), loc));
		if (!_write)
		{
			// gather the long chunks, then trim to len
			body->body.push_back(awst::makeAssignmentStatement(
				bytesVar("__data"), awst::makeBytesConstant({}, loc), loc));
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__i"), u64c(0), loc));
			auto cond = awst::makeNumericCompare(
				awst::makeUInt64BinOp(u64Var("__i"),
					awst::UInt64BinaryOperator::Mult, u64c(32), loc),
				awst::NumericComparison::Lt, u64Var("__len"), loc);
			auto append = awst::makeAssignmentStatement(
				bytesVar("__data"),
				awst::makeConcat(bytesVar("__data"),
					wordBytes(readWordCall(chunkAt("__i"))), loc), loc);
			body->body.push_back(chunkLoop(std::move(cond), std::move(append), "__i"));
			body->body.push_back(awst::makeReturnStatement(
				awst::makeExtract3(bytesVar("__data"), u64c(0), u64Var("__len"), loc), loc));
		}
		else
		{
			// old chunk count: odd old word → ceil(((word-1)/2)/32), else 0
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__oldChunks"), u64c(0), loc));
			{
				auto wasLong = awst::makeNot(isShort, loc);
				auto thenBlk = awst::makeBlock(loc);
				thenBlk->body.push_back(awst::makeAssignmentStatement(
					u64Var("__oldChunks"), chunkCount(storedLen), loc));
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
					u64Var("__newChunks"), chunkCount(u64Var("__len")), loc));
				elseBlk->body.push_back(awst::makeAssignmentStatement(
					bytesVar("__padded"),
					awst::makeConcat(valVar(), awst::makeBzero(32, loc), loc), loc));
				elseBlk->body.push_back(awst::makeAssignmentStatement(
					u64Var("__i"), u64c(0), loc));
				auto cond = awst::makeNumericCompare(u64Var("__i"),
					awst::NumericComparison::Lt, u64Var("__newChunks"), loc);
				auto chunk = awst::makeExtract3(bytesVar("__padded"),
					awst::makeUInt64BinOp(u64Var("__i"),
						awst::UInt64BinaryOperator::Mult, u64c(32), loc),
					u64c(32), loc);
				auto store = writeWordStmt(chunkAt("__i"),
					awst::makeAsBiguint(std::move(chunk), loc));
				elseBlk->body.push_back(chunkLoop(
					std::move(cond), std::move(store), "__i"));
				body->body.push_back(awst::makeIfElse(
					std::move(isShort), std::move(thenBlk), std::move(elseBlk), loc));
			}
			// clear stale long chunks beyond the new count (EVM zeroes on shrink)
			{
				body->body.push_back(awst::makeAssignmentStatement(
					u64Var("__j"), u64Var("__newChunks"), loc));
				auto cond = awst::makeNumericCompare(u64Var("__j"),
					awst::NumericComparison::Lt, u64Var("__oldChunks"), loc);
				auto clear = writeWordStmt(chunkAt("__j"),
					awst::makeIntegerConstant("0", loc, awst::WType::biguintType()));
				body->body.push_back(chunkLoop(
					std::move(cond), std::move(clear), "__j"));
			}
			body->body.push_back(awst::makeReturnStatement(nullptr, loc));
		}

		_contractNode->methods.push_back(std::move(sub));
	}

	// ── __evm_dynarr_read(slot: biguint) -> bytes ──
	// ── __evm_dynarr_write(slot: biguint, val: bytes) -> void ──
	// One lane layout feeds both directions of a dynamic array of 32-byte-
	// encoded elements in its ARC4 form [u16 count][elems]: count word at the
	// slot, elements at keccak256(slot32)+i. Callers cap/validate element
	// width. A write CLEARS the old tail when the array shrinks — EVM
	// assignment semantics, and a later push must see zeroed slots.
	//
	// __size = storage bytes per element, __aw = ARC4 bytes per element
	// (differs for address: 20 stored, 32 encoded), __per = elements per
	// slot (EVM packs from the LOW end of the word). __mul = lanes per
	// ELEMENT (fixed-array / uniform-struct elements are lane concatenations
	// in both slot and ARC4 layouts); the loop runs over LANES while the
	// count word/prefix stays in elements. __bp marks fixed bool[N] elements:
	// EVM stores byte lanes, ARC4 stores MSB-first bits in an __aw-byte
	// region reset for each outer element.
	void emitDynamicArrayCodec(awst::Contract* _contractNode, bool _write) const
	{
		std::vector<awst::SubroutineArgument> args{
			{"__slot", awst::WType::biguintType(), loc}};
		if (_write)
			args.emplace_back("__val", awst::WType::bytesType(), loc);
		auto sub = awst::ContractMethod(cref,
			_write ? "__evm_dynarr_write" : "__evm_dynarr_read",
			_write ? awst::WType::voidType() : awst::WType::bytesType(),
			withMetrics(std::move(args), loc), loc);
		auto body = sub.body;
		auto valVar = [&]() { return bytesVar("__val"); };
		// lanes of an element count
		auto lanes = [&](char const* _n) {
			return awst::makeUInt64BinOp(u64Var(_n),
				awst::UInt64BinaryOperator::Mult, u64Var("__mul"), loc);
		};
		// lane __j of a word sits at byte 32 - (j+1)*size
		auto laneOff = [&]() {
			return awst::makeUInt64BinOp(u64c(32),
				awst::UInt64BinaryOperator::Sub,
				awst::makeUInt64BinOp(
					awst::makeUInt64BinOp(u64Var("__j"),
						awst::UInt64BinaryOperator::Add, u64c(1), loc),
					awst::UInt64BinaryOperator::Mult, u64Var("__size"), loc), loc);
		};
		// an ARC4 lane is aw - size zero bytes ahead of the storage bytes
		auto arc4Pad = [&]() {
			return awst::makeUInt64BinOp(u64Var("__aw"),
				awst::UInt64BinaryOperator::Sub, u64Var("__size"), loc);
		};
		// bit-packed: bit __bj of outer element i/mul, whose __aw-byte ARC4
		// region follows the u16 prefix
		auto bitIndex = [&]() {
			auto elemNo = awst::makeUInt64BinOp(u64Var("__i"),
				awst::UInt64BinaryOperator::FloorDiv, u64Var("__mul"), loc);
			return awst::makeUInt64BinOp(
				awst::makeUInt64BinOp(
					awst::makeUInt64BinOp(u64c(2),
						awst::UInt64BinaryOperator::Add,
						awst::makeUInt64BinOp(std::move(elemNo),
							awst::UInt64BinaryOperator::Mult, u64Var("__aw"), loc), loc),
					awst::UInt64BinaryOperator::Mult, u64c(8), loc),
				awst::UInt64BinaryOperator::Add, u64Var("__bj"), loc);
		};
		auto atWordStart = [&]() {
			return awst::makeNumericCompare(u64Var("__j"),
				awst::NumericComparison::Eq, u64c(0), loc);
		};

		// count word at the slot: the length read, or the OLD length of a
		// write (for the shrink-clear tail)
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var(_write ? "__old" : "__n"),
			TypeCoercion::checkedIndexToUint64(body->body, readWordCall(slotVar()), loc), loc));
		if (_write)
			// new length from the ARC4 u16 header
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__n"),
				awst::makeBtoi(awst::makeExtract(valVar(), 0, 2, loc), loc), loc));
		else
		{
			auto width = awst::makeConditional(awst::makeNumericCompare(u64Var("__bp"),
				awst::NumericComparison::Ne, u64c(0), loc), u64Var("__aw"),
				awst::makeUInt64BinOp(u64Var("__aw"), awst::UInt64BinaryOperator::Mult,
					u64Var("__mul"), loc), awst::WType::uint64Type(), loc);
			body->body.push_back(awst::makeExpressionStatement(awst::makeAssert(
				awst::makeNumericCompare(u64Var("__n"), awst::NumericComparison::Lte,
					awst::makeUInt64BinOp(u64c(4094), awst::UInt64BinaryOperator::FloorDiv,
						std::move(width), loc), loc), loc, "storage array exceeds AVM value capacity"), loc));
		}
		body->body.push_back(awst::makeAssignmentStatement(
			u64Var("__nl"), lanes("__n"), loc));
		if (_write)
		{
			body->body.push_back(awst::makeAssignmentStatement(
				u64Var("__oldl"), lanes("__old"), loc));
			body->body.push_back(writeWordStmt(slotVar(),
				u64ToBiguint(u64Var("__n"))));
		}
		body->body.push_back(awst::makeAssignmentStatement(
			biguintVar("__chunk"), chunkBase(), loc));
		if (!_write)
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
		if (_write)
		{
			loop->body.push_back(awst::makeAssignmentStatement(
				biguintVar("__ws"), chunkAt("__wi"), loc));
			// fresh word at each word boundary — no read needed, because every
			// element of the word is (re)written before it is stored
			loop->body.push_back(awst::makeAssignmentStatement(bytesVar("__wb"),
				awst::makeConditional(atWordStart(),
					awst::makeBzero(u64c(32), loc),
					bytesVar("__wb"),
					awst::WType::bytesType(), loc), loc));
		}
		else
		{
			// Read once at the first lane of each storage word.  Packed elements —
			// especially fixed bool arrays, where one outer element expands to many
			// lanes — otherwise paid for the same storage read on every lane.
			auto loadWord = awst::makeBlock(loc);
			loadWord->body.push_back(awst::makeAssignmentStatement(bytesVar("__wb"),
				wordBytes(readWordCall(chunkAt("__wi"))), loc));
			loop->body.push_back(awst::makeIfElse(
				atWordStart(), std::move(loadWord), nullptr, loc));
			loop->body.push_back(awst::makeAssignmentStatement(bytesVar("__el"),
				awst::makeExtract3(bytesVar("__wb"), laneOff(),
					u64Var("__size"), loc), loc));
		}
		// Byte-aligned elements move one ARC4 lane: a read appends it, a
		// write takes the low `size` bytes of it. Fixed bool[N] elements go
		// through ARC4 bits: a read appends a zeroed region once per outer
		// element and sets its bits from the canonical low-byte EVM lanes, a
		// write reads one bit and turns it back into Solidity's 0/1 byte.
		{
			auto bitBlk = awst::makeBlock(loc);
			bitBlk->body.push_back(awst::makeAssignmentStatement(u64Var("__bj"),
				awst::makeUInt64BinOp(u64Var("__i"),
					awst::UInt64BinaryOperator::Mod, u64Var("__mul"), loc), loc));
			auto byteBlk = awst::makeBlock(loc);
			if (_write)
			{
				bitBlk->body.push_back(awst::makeAssignmentStatement(bytesVar("__el"),
					awst::makeExtract(awst::makeItob(
						awst::makeGetbit(valVar(), bitIndex(), loc), loc),
						7, 1, loc), loc));
				auto vOff = awst::makeUInt64BinOp(
					awst::makeUInt64BinOp(u64Var("__i"),
						awst::UInt64BinaryOperator::Mult, u64Var("__aw"), loc),
					awst::UInt64BinaryOperator::Add,
					awst::makeUInt64BinOp(u64c(2),
						awst::UInt64BinaryOperator::Add, arc4Pad(), loc), loc);
				byteBlk->body.push_back(awst::makeAssignmentStatement(bytesVar("__el"),
					awst::makeExtract3(valVar(), std::move(vOff),
						u64Var("__size"), loc), loc));
			}
			else
			{
				auto beginElem = awst::makeBlock(loc);
				beginElem->body.push_back(awst::makeAssignmentStatement(
					bytesVar("__data"), awst::makeConcat(bytesVar("__data"),
						awst::makeBzero(u64Var("__aw"), loc), loc), loc));
				bitBlk->body.push_back(awst::makeIfElse(
					awst::makeNumericCompare(u64Var("__bj"),
						awst::NumericComparison::Eq, u64c(0), loc),
					std::move(beginElem), nullptr, loc));
				auto isTrue = awst::makeNumericCompare(
					awst::makeBtoi(bytesVar("__el"), loc),
					awst::NumericComparison::Ne, u64c(0), loc);
				bitBlk->body.push_back(awst::makeAssignmentStatement(
					bytesVar("__data"), awst::makeSetbit(bytesVar("__data"),
						bitIndex(), std::move(isTrue), loc), loc));
				byteBlk->body.push_back(awst::makeAssignmentStatement(
					bytesVar("__data"),
					awst::makeConcat(bytesVar("__data"),
						awst::makeConcat(awst::makeBzero(arc4Pad(), loc),
							bytesVar("__el"), loc), loc), loc));
			}
			loop->body.push_back(awst::makeIfElse(
				awst::makeNumericCompare(u64Var("__bp"),
					awst::NumericComparison::Ne, u64c(0), loc),
				std::move(bitBlk), std::move(byteBlk), loc));
		}
		if (_write)
		{
			loop->body.push_back(awst::makeAssignmentStatement(bytesVar("__wb"),
				awst::makeReplace3(bytesVar("__wb"), laneOff(),
					bytesVar("__el"), loc), loc));
			// store the word at its last lane, or at the last lane of all
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
		loop->body.push_back(inc("__i"));
		body->body.push_back(awst::makeWhileLoop(std::move(cond), std::move(loop), loc));
		if (!_write)
			body->body.push_back(awst::makeReturnStatement(bytesVar("__data"), loc));
		else
		{
			// clear the shrink tail: whole WORDS from ceil(n/per) to ceil(old/per)
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
			auto tailCond = awst::makeNumericCompare(u64Var("__wi"),
				awst::NumericComparison::Lt, u64Var("__we"), loc);
			auto tail = awst::makeBlock(loc);
			tail->body.push_back(writeWordStmt(chunkAt("__wi"),
				awst::makeZero(loc, awst::WType::biguintType())));
			tail->body.push_back(inc("__wi"));
			body->body.push_back(awst::makeWhileLoop(
				std::move(tailCond), std::move(tail), loc));
		}
		_contractNode->methods.push_back(std::move(sub));
	}

	// ── Recursive dynamic-array read / write ──
	// Every dynamic-array layer has the same storage and ARC4 structure. Depth
	// one delegates to the leaf codec above; greater depths recursively compose
	// u16 heads and inner tails. T[][][] is therefore the same path as T[][].
	void emitNestedDynamicArrayMethods(awst::Contract* _contractNode) const
	{
		auto metricArgs = [&](std::vector<awst::CallArg>& _args) {
			for (char const* an: kDynarrMetrics)
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
		// READ
		{
			auto sub = awst::ContractMethod(cref, "__evm_dynarr_recursive_read",
				awst::WType::bytesType(),
				withMetrics({{"__slot", awst::WType::biguintType(), loc},
					{"__depth", awst::WType::uint64Type(), loc}}, loc), loc);
			auto body = sub.body;
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
				u64Var("__n"), TypeCoercion::checkedIndexToUint64(body->body, readWordCall(slotVar()), loc), loc));
			body->body.push_back(awst::makeExpressionStatement(awst::makeAssert(
				awst::makeNumericCompare(u64Var("__n"), awst::NumericComparison::Lte,
					u64c(2047), loc), loc, "storage array exceeds AVM value capacity"), loc));
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
			_contractNode->methods.push_back(std::move(sub));
		}

		// WRITE
		{
			auto sub = awst::ContractMethod(cref, "__evm_dynarr_recursive_write",
				awst::WType::voidType(),
				withMetrics({{"__slot", awst::WType::biguintType(), loc},
					{"__val", awst::WType::bytesType(), loc},
					{"__depth", awst::WType::uint64Type(), loc}}, loc), loc);
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
			auto body = sub.body;
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
				u64Var("__old"), TypeCoercion::checkedIndexToUint64(body->body, readWordCall(slotVar()), loc), loc));
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
			_contractNode->methods.push_back(std::move(sub));
		}
	}

};
}  // namespace

void ContractBuilder::buildEvmSlotStorageDispatch(
	StorageRuntimePlan const& _storagePlan,
	awst::Contract* _contractNode,
	std::string const& _contractName
)
{
	auto const& layout = _storagePlan.solidityLayout;
	auto const& cref = m_contractId;
	awst::SourceLocation loc(m_sourceFile);
	// Generic roots remain usable by library/free functions without a host.
	// Dense variants have distinct IDs and are selected only for concrete
	// hosts whose solc layout and reachable-call facts exclude sparse slots.
	for (unsigned shape = 0; shape < 3; ++shape)
	{
		std::string prefix = shape == 0 ? storage_dispatch::genericSlotPrefix
			: shape == 1 ? storage_dispatch::denseSlotPrefix : storage_dispatch::singlePageSlotPrefix;
		EvmSlotCodec codec{m_typeMapper, loc, cref, shape != 0, shape == 2, "__eslot64"};
		codec.emitStorageRead(_contractNode);
		codec.emitStorageWrite(_contractNode);
		if (shape == 0)
		{
			codec.emitBytesCodec(_contractNode, false);
			codec.emitBytesCodec(_contractNode, true);
			codec.emitDynamicArrayCodec(_contractNode, false);
			codec.emitDynamicArrayCodec(_contractNode, true);
			codec.emitNestedDynamicArrayMethods(_contractNode);
		}
		storage_dispatch::promoteMethods(*_contractNode, m_dispatchSubroutines, prefix,
			{"__storage_read", "__storage_write", "__storage_write_word", "__evm_bytes_read", "__evm_bytes_write",
				"__evm_dynarr_read", "__evm_dynarr_write", "__evm_dynarr_recursive_read", "__evm_dynarr_recursive_write"});
	}

	Logger::instance().debug(
		"Generated EVM-slot __storage_read/__storage_write (paged<"
		+ std::to_string(kEvmDenseSlotLimit) + "/sparse) for "
		+ std::to_string(layout.totalSlots()) + " dense slots", loc);
}
} // namespace puyasol::builder
