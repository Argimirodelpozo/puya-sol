#include "builder/contract/ContractBuilder.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/SolIntType.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

#include <algorithm>

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

void ContractBuilder::buildStorageDispatch(
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

	// The var's packed field: s big-endian bytes of its EVM-slot content.
	auto packedFieldBytes = [&](SlotVariable const* v) -> std::shared_ptr<awst::Expression> {
		unsigned sz = v->byteSize;
		auto read = m_storageMapper.createStateRead(
			v->name, v->wtype, awst::AppStorageKind::AppGlobal, loc);
		if (v->wtype == awst::WType::uint64Type() || v->wtype == awst::WType::boolType())
		{
			// uint64-backed (incl. sub-64 signed: cell holds 64-bit TC, whose low
			// s bytes ARE the packed TC). bool → 0/1.
			std::shared_ptr<awst::Expression> u64 = std::move(read);
			if (v->wtype == awst::WType::boolType())
				u64 = awst::makeConditional(std::move(u64), makeUint64("1"), makeUint64("0"),
					awst::WType::uint64Type(), loc);
			auto itob = awst::makeItob(std::move(u64), loc);
			if (sz == 8)
				return itob;
			if (sz > 8)
				return awst::makeLeftPad(std::move(itob), sz - 8, loc);
			return awst::makeExtract(std::move(itob), static_cast<int>(8 - sz), static_cast<int>(sz), loc);
		}
		if (v->wtype == awst::WType::biguintType())
		{
			// Canonical 256-bit TC (signed) / plain magnitude (unsigned): the
			// trailing s bytes of the 32-byte form are the packed content.
			auto padded = awst::makeZeroExtendToN(awst::makeAsBytes(std::move(read), loc), 32, loc);
			return awst::makeExtract(std::move(padded), static_cast<int>(32 - sz), static_cast<int>(sz), loc);
		}
		if (v->wtype == awst::WType::accountType())
		{
			// AVM account = 32 bytes; EVM address = trailing 20 (transient-codec convention).
			return awst::makeExtract(awst::makeAsBytes(std::move(read), loc),
				static_cast<int>(32 - sz), static_cast<int>(sz), loc);
		}
		if (v->wtype && v->wtype->kind() == awst::WTypeKind::Bytes)
			return awst::makeAsBytes(std::move(read), loc);   // bytes[N]: raw N bytes
		Logger::instance().error(
			"packed storage slot " + std::to_string(v->slot) + " holds '" + v->name
			+ "' of unsupported type for asm slot access", loc);
		return awst::makeBytesConstant(std::vector<uint8_t>(sz, 0), loc);
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

			std::shared_ptr<awst::Expression> native;
			if (v->wtype == awst::WType::uint64Type() || v->wtype == awst::WType::boolType())
			{
				std::shared_ptr<awst::Expression> u64;
				if (sz > 8)   // e.g. contract type packed as 20 bytes: numeric low 8
					u64 = awst::makeBtoi(awst::makeExtract(std::move(raw),
						static_cast<int>(sz - 8), 8, loc), loc);
				else
					u64 = awst::makeBtoi(std::move(raw), loc);
				// Sub-64 signed: cell convention is 64-bit TC — sign-extend from s bytes.
				if (auto it = SolIntType::fromSol(v->solType);
					it && it->isSigned && it->bits < 64 && v->wtype == awst::WType::uint64Type())
				{
					uint64_t half = 1ULL << (it->bits - 1);
					uint64_t addend = ~((1ULL << it->bits) - 1);
					auto isNeg = awst::makeNumericCompare(u64, awst::NumericComparison::Gte,
						makeUint64(std::to_string(half)), loc);
					auto extended = awst::makeUInt64BinOp(u64, awst::UInt64BinaryOperator::Add,
						makeUint64(std::to_string(addend)), loc);
					u64 = awst::makeConditional(std::move(isNeg), std::move(extended), u64,
						awst::WType::uint64Type(), loc);
				}
				if (v->wtype == awst::WType::boolType())
					native = awst::makeNumericCompare(std::move(u64), awst::NumericComparison::Ne,
						makeUint64("0"), loc);
				else
					native = std::move(u64);
			}
			else if (v->wtype == awst::WType::biguintType())
			{
				native = awst::makeAsBiguint(std::move(raw), loc);
				// 64 < bits < 256 signed: extend to the canonical 256-bit TC cell form.
				native = TypeCoercion::signExtendSignedElement(std::move(native), v->solType, loc);
			}
			else if (v->wtype == awst::WType::accountType())
			{
				native = awst::makeAsAccount(awst::makeLeftPad(std::move(raw), 32 - sz, loc), loc);
			}
			else if (v->wtype->kind() == awst::WTypeKind::Bytes)
			{
				native = awst::makeReinterpretCast(std::move(raw), v->wtype, loc);
			}
			else
			{
				Logger::instance().error(
					"packed storage slot " + std::to_string(v->slot) + " holds '" + v->name
					+ "' of unsupported type for asm slot access", loc);
				continue;
			}

			auto key = awst::makeUtf8BytesConstant(v->name, loc, awst::WType::stateKeyType());
			auto target = awst::makeAppStateExpression(std::move(key), v->wtype, loc);
			auto assign = awst::makeAssignmentExpression(
				std::move(target), std::move(native), loc, v->wtype);
			_blk.body.push_back(awst::makeExpressionStatement(std::move(assign), loc));
		}
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
				awst::makeIntegerConstant(std::to_string(si.slotNumber), loc, awst::WType::biguintType()), loc);

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
				awst::makeIntegerConstant(std::to_string(si.slotNumber), loc, awst::WType::biguintType()), loc);

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
