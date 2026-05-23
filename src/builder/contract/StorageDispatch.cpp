#include "builder/ContractBuilder.h"
#include "builder/storage/StorageLayout.h"
#include "Logger.h"

#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder
{

namespace {
/// Recursive visitor — true if any function in the linearised hierarchy
/// contains an InlineAssembly node (anywhere, including nested in
/// if/for/etc.). The previous hand-rolled loop only checked top-level
/// statements, so deeply-nested asm slipped past.
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
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
		for (auto const* func: base->definedFunctions())
			if (func->isImplemented())
			{
				func->body().accept(asmDetector);
				if (asmDetector.found) break;
			}

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
		slotArg.wtype = awst::WType::uint64Type();
		slotArg.sourceLocation = loc;
		readSub.args.push_back(slotArg);

		auto body = awst::makeBlock(loc);

		// Build if/else chain for known slots
		// Start from innermost (default case) and wrap outward
		// Default: read from global state using slot key "s" + itob(slot)
		// This supports dynamic slot-based storage references (assembly .slot)
		auto defaultBlock = awst::makeBlock(loc);
		{
			// Use a single large box "__dyn_storage" for all dynamic slots.
			// Each slot occupies 32 bytes at offset (slot % 256) * 32.
			// This avoids per-slot box reference limits (max 8 per txn).
			auto boxKey = makeBytes("__dyn_storage");

			// Compute offset: (__slot % 256) * 32
			auto slotVar = awst::makeVarExpression("__slot", awst::WType::uint64Type(), loc);

			auto mod256 = awst::makeUInt64BinOp(std::move(slotVar), awst::UInt64BinaryOperator::Mod, makeUint64("256"), loc);

			auto offset = awst::makeUInt64BinOp(std::move(mod256), awst::UInt64BinaryOperator::Mult, makeUint64("32"), loc);

			// box_create("__dyn_storage", 8192) — 256 slots * 32 bytes
			auto boxCreate = awst::makeBoxCreate(boxKey, makeUint64("8192"), loc);

			auto popStmt = awst::makeExpressionStatement(std::move(boxCreate), loc);
			defaultBlock->body.push_back(std::move(popStmt));

			// box_extract("__dyn_storage", offset, 32)
			auto boxExtract = awst::makeBoxExtract(
				std::move(boxKey), std::move(offset), makeUint64("32"), loc);

			auto cast = awst::makeAsBiguint(std::move(boxExtract), loc);

			auto ret = awst::makeReturnStatement(std::move(cast), loc);
			defaultBlock->body.push_back(std::move(ret));
		}

		std::shared_ptr<awst::Statement> current;
		// Build the chain bottom-up
		std::shared_ptr<awst::Block> elseBlock = defaultBlock;

		for (auto const& sv: layout.variables())
		{
			if (!sv.wtype || sv.wtype == awst::WType::voidType()) continue;

			// Condition: __slot == slotNumber
			auto slotVar = awst::makeVarExpression("__slot", awst::WType::uint64Type(), loc);

			auto cmp = awst::makeNumericCompare(slotVar, awst::NumericComparison::Eq, makeUint64(std::to_string(sv.slot)), loc);

			// If branch: return app_global_get(varName) as biguint
			auto ifBlock = awst::makeBlock(loc);
			{
				auto get = awst::makeIntrinsicCall("app_global_get", awst::WType::bytesType(), loc);
				get->stackArgs.push_back(makeBytes(sv.name));

				// Left-pad then take last 32 — yields a fixed-width 32-byte
				// value regardless of the global state slot's original size
				// (which can be <32 for short ints).
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

		// The outermost block is the body
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
		slotArg.wtype = awst::WType::uint64Type();
		slotArg.sourceLocation = loc;
		writeSub.args.push_back(slotArg);

		awst::SubroutineArgument valArg;
		valArg.name = "__value";
		valArg.wtype = awst::WType::biguintType();
		valArg.sourceLocation = loc;
		writeSub.args.push_back(valArg);

		auto body = awst::makeBlock(loc);

		// Build if/else chain for known slots
		auto defaultBlock = awst::makeBlock(loc);
		// Default: write to global state using slot key "s" + itob(slot)
		{
			// Build key: concat("s", itob(__slot))
			auto prefix = makeBytes("s");
			auto slotVar = awst::makeVarExpression("__slot", awst::WType::uint64Type(), loc);
			auto slotItob = awst::makeItob(std::move(slotVar), loc);

			auto key = awst::makeConcat(std::move(prefix), std::move(slotItob), loc);

			// Use single "__dyn_storage" box, same as read
			auto boxKey = makeBytes("__dyn_storage");

			// Compute offset: (__slot % 256) * 32
			auto slotVar2 = awst::makeVarExpression("__slot", awst::WType::uint64Type(), loc);

			auto mod256 = awst::makeUInt64BinOp(std::move(slotVar2), awst::UInt64BinaryOperator::Mod, makeUint64("256"), loc);

			auto offset = awst::makeUInt64BinOp(std::move(mod256), awst::UInt64BinaryOperator::Mult, makeUint64("32"), loc);

			// value as bytes (pad to 32)
			auto valueVar = awst::makeVarExpression("__value", awst::WType::biguintType(), loc);

			auto valBytes = awst::makeAsBytes(std::move(valueVar), loc);

			// Pad to 32 bytes
			auto cat = awst::makeLeftPad(std::move(valBytes), 32, loc);

			auto lenCall = awst::makeLen(cat, loc);

			auto sub32 = awst::makeUInt64BinOp(std::move(lenCall), awst::UInt64BinaryOperator::Sub, makeUint64("32"), loc);

			auto paddedVal = awst::makeExtract3(cat, std::move(sub32), makeUint64("32"), loc);
			// box_create("__dyn_storage", 8192) — ensure box exists
			auto boxCreate = awst::makeBoxCreate(boxKey, makeUint64("8192"), loc);

			auto createStmt = awst::makeExpressionStatement(std::move(boxCreate), loc);
			defaultBlock->body.push_back(std::move(createStmt));

			// box_replace("__dyn_storage", offset, padded_value)
			auto boxReplace = awst::makeIntrinsicCall("box_replace", awst::WType::voidType(), loc);
			boxReplace->stackArgs.push_back(std::move(boxKey));
			boxReplace->stackArgs.push_back(std::move(offset));
			boxReplace->stackArgs.push_back(std::move(paddedVal));

			auto replaceStmt = awst::makeExpressionStatement(std::move(boxReplace), loc);
			defaultBlock->body.push_back(std::move(replaceStmt));

			auto ret = awst::makeReturnStatement(nullptr, loc);
			defaultBlock->body.push_back(std::move(ret));
		}

		std::shared_ptr<awst::Block> elseBlock = defaultBlock;

		for (auto const& sv: layout.variables())
		{
			if (!sv.wtype || sv.wtype == awst::WType::voidType()) continue;

			auto slotVar = awst::makeVarExpression("__slot", awst::WType::uint64Type(), loc);

			auto cmp = awst::makeNumericCompare(slotVar, awst::NumericComparison::Eq, makeUint64(std::to_string(sv.slot)), loc);

			auto ifBlock = awst::makeBlock(loc);
			{
				// app_global_put(varName, pad32(value_as_bytes))
				// Pad to 32 bytes to match EVM slot semantics
				auto valueVar = awst::makeVarExpression("__value", awst::WType::biguintType(), loc);

				auto cast = awst::makeAsBytes(std::move(valueVar), loc);

				// concat(bzero(32), bytes) → take last 32 bytes
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

	// Move both dispatchers out of contract methods into root-level Subroutines.
	// Library / free-function callers can't issue InstanceMethodTarget — puya
	// rejects "invocation of instance method outside of a contract method" —
	// and a SubroutineID call to a root-level Subroutine works in every
	// context, including from contract methods themselves. The body uses only
	// `app_global_get`, `box_extract`, etc. (global ops), which operate
	// against the currently executing app's state regardless of caller scope.
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
