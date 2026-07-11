#include "builder/contract/ContractBuilder.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/storage/StorageLayout.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

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

		for (auto const& sv: layout.variables())
		{
			if (!sv.wtype || sv.wtype == awst::WType::voidType()) continue;

			auto slotVar = awst::makeVarExpression("__slot", awst::WType::biguintType(), loc);
			auto cmp = awst::makeNumericCompare(slotVar, awst::NumericComparison::Eq,
				awst::makeIntegerConstant(std::to_string(sv.slot), loc, awst::WType::biguintType()), loc);

			auto ifBlock = awst::makeBlock(loc);
			{
				auto get = awst::makeIntrinsicCall("app_global_get", awst::WType::bytesType(), loc);
				get->stackArgs.push_back(makeBytes(sv.name));

				// Left-pad + take last 32 bytes (global slots may be <32 for short ints).
				auto cat = awst::makeLeftPad(std::move(get), 32, loc);
				auto extract = awst::makeExtractLastN(std::move(cat), 32, loc);
				auto cast = awst::makeAsBiguint(std::move(extract), loc);

				auto ret = awst::makeReturnStatement(std::move(cast), loc);
				ifBlock->body.push_back(std::move(ret));
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

		for (auto const& sv: layout.variables())
		{
			if (!sv.wtype || sv.wtype == awst::WType::voidType()) continue;

			auto slotVar = awst::makeVarExpression("__slot", awst::WType::biguintType(), loc);
			auto cmp = awst::makeNumericCompare(slotVar, awst::NumericComparison::Eq,
				awst::makeIntegerConstant(std::to_string(sv.slot), loc, awst::WType::biguintType()), loc);

			auto ifBlock = awst::makeBlock(loc);
			{
				// app_global_put: pad value to 32 bytes (EVM slot width).
				auto valueVar = awst::makeVarExpression("__value", awst::WType::biguintType(), loc);
				auto cast = awst::makeAsBytes(std::move(valueVar), loc);
				auto cat = awst::makeLeftPad(std::move(cast), 32, loc);
				auto lenCall = awst::makeLen(cat, loc);
				auto sub32 = awst::makeUInt64BinOp(std::move(lenCall), awst::UInt64BinaryOperator::Sub, makeUint64("32"), loc);

				auto extract = awst::makeExtract3(cat, std::move(sub32), makeUint64("32"), loc);
				auto put = awst::makeAppGlobalPut(makeBytes(sv.name), std::move(extract), loc);

				auto stmt = awst::makeExpressionStatement(std::move(put), loc);
				ifBlock->body.push_back(std::move(stmt));

				auto ret = awst::makeReturnStatement(nullptr, loc);
				ifBlock->body.push_back(std::move(ret));
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
