#include "builder/ContractBuilder.h"
#include "builder/storage/StorageLayout.h"
#include "Logger.h"

namespace puyasol::builder
{

void ContractBuilder::buildStorageDispatch(
	solidity::frontend::ContractDefinition const& _contract,
	awst::Contract* _contractNode,
	std::string const& _contractName
)
{
	StorageLayout layout;
	layout.computeLayout(_contract, m_typeMapper);

	// Check if any function has inline assembly (might use .slot / sload / sstore)
	bool hasInlineAsm = false;
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
		for (auto const* func: base->definedFunctions())
			if (func->isImplemented())
				for (auto const& stmt: func->body().statements())
					if (dynamic_cast<solidity::frontend::InlineAssembly const*>(stmt.get()))
					{ hasInlineAsm = true; goto asmCheckDone; }
	asmCheckDone:

	if (layout.totalSlots() == 0 && !hasInlineAsm)
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

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;

		// Build if/else chain for known slots
		// Start from innermost (default case) and wrap outward
		// Default: read from global state using slot key "s" + itob(slot)
		// This supports dynamic slot-based storage references (assembly .slot)
		auto defaultBlock = std::make_shared<awst::Block>();
		defaultBlock->sourceLocation = loc;
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
			auto boxCreate = awst::makeIntrinsicCall("box_create", awst::WType::boolType(), loc);
			boxCreate->stackArgs.push_back(boxKey);
			boxCreate->stackArgs.push_back(makeUint64("8192"));

			auto popStmt = awst::makeExpressionStatement(std::move(boxCreate), loc);
			defaultBlock->body.push_back(std::move(popStmt));

			// box_extract("__dyn_storage", offset, 32)
			auto boxExtract = awst::makeIntrinsicCall("box_extract", awst::WType::bytesType(), loc);
			boxExtract->stackArgs.push_back(std::move(boxKey));
			boxExtract->stackArgs.push_back(std::move(offset));
			boxExtract->stackArgs.push_back(makeUint64("32"));

			auto cast = awst::makeReinterpretCast(std::move(boxExtract), awst::WType::biguintType(), loc);

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
			auto ifBlock = std::make_shared<awst::Block>();
			ifBlock->sourceLocation = loc;
			{
				auto get = awst::makeIntrinsicCall("app_global_get", awst::WType::bytesType(), loc);
				get->stackArgs.push_back(makeBytes(sv.name));

				// Pad to 32 bytes: concat(bzero(32), value), take last 32
				auto bz = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), loc);
				bz->stackArgs.push_back(makeUint64("32"));

				auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
				cat->stackArgs.push_back(std::move(bz));
				cat->stackArgs.push_back(std::move(get));

				// Extract last 32 bytes
				auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), loc);
				lenCall->stackArgs.push_back(cat);

				auto sub = awst::makeUInt64BinOp(std::move(lenCall), awst::UInt64BinaryOperator::Sub, makeUint64("32"), loc);

				auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), loc);
				extract->stackArgs.push_back(cat);
				extract->stackArgs.push_back(std::move(sub));
				extract->stackArgs.push_back(makeUint64("32"));

				auto cast = awst::makeReinterpretCast(std::move(extract), awst::WType::biguintType(), loc);

				auto ret = awst::makeReturnStatement(std::move(cast), loc);
				ifBlock->body.push_back(std::move(ret));
			}

			auto ifElse = std::make_shared<awst::IfElse>();
			ifElse->sourceLocation = loc;
			ifElse->condition = std::move(cmp);
			ifElse->ifBranch = std::move(ifBlock);
			ifElse->elseBranch = std::move(elseBlock);

			auto newElse = std::make_shared<awst::Block>();
			newElse->sourceLocation = loc;
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

		auto body = std::make_shared<awst::Block>();
		body->sourceLocation = loc;

		// Build if/else chain for known slots
		auto defaultBlock = std::make_shared<awst::Block>();
		defaultBlock->sourceLocation = loc;
		// Default: write to global state using slot key "s" + itob(slot)
		{
			// Build key: concat("s", itob(__slot))
			auto prefix = makeBytes("s");
			auto slotItob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), loc);
			auto slotVar = awst::makeVarExpression("__slot", awst::WType::uint64Type(), loc);
			slotItob->stackArgs.push_back(std::move(slotVar));

			auto key = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
			key->stackArgs.push_back(std::move(prefix));
			key->stackArgs.push_back(std::move(slotItob));

			// Use single "__dyn_storage" box, same as read
			auto boxKey = makeBytes("__dyn_storage");

			// Compute offset: (__slot % 256) * 32
			auto slotVar2 = awst::makeVarExpression("__slot", awst::WType::uint64Type(), loc);

			auto mod256 = awst::makeUInt64BinOp(std::move(slotVar2), awst::UInt64BinaryOperator::Mod, makeUint64("256"), loc);

			auto offset = awst::makeUInt64BinOp(std::move(mod256), awst::UInt64BinaryOperator::Mult, makeUint64("32"), loc);

			// value as bytes (pad to 32)
			auto valueVar = awst::makeVarExpression("__value", awst::WType::biguintType(), loc);

			auto valBytes = awst::makeReinterpretCast(std::move(valueVar), awst::WType::bytesType(), loc);

			// Pad to 32 bytes
			auto bz = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), loc);
			bz->stackArgs.push_back(makeUint64("32"));

			auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
			cat->stackArgs.push_back(std::move(bz));
			cat->stackArgs.push_back(std::move(valBytes));

			auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), loc);
			lenCall->stackArgs.push_back(cat);

			auto sub32 = awst::makeUInt64BinOp(std::move(lenCall), awst::UInt64BinaryOperator::Sub, makeUint64("32"), loc);

			auto paddedVal = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), loc);
			paddedVal->stackArgs.push_back(cat);
			paddedVal->stackArgs.push_back(std::move(sub32));
			paddedVal->stackArgs.push_back(makeUint64("32"));

			// box_create("__dyn_storage", 8192) — ensure box exists
			auto boxCreate = awst::makeIntrinsicCall("box_create", awst::WType::boolType(), loc);
			boxCreate->stackArgs.push_back(boxKey);
			boxCreate->stackArgs.push_back(makeUint64("8192"));

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

			auto ifBlock = std::make_shared<awst::Block>();
			ifBlock->sourceLocation = loc;
			{
				// app_global_put(varName, pad32(value_as_bytes))
				// Pad to 32 bytes to match EVM slot semantics
				auto valueVar = awst::makeVarExpression("__value", awst::WType::biguintType(), loc);

				auto cast = awst::makeReinterpretCast(std::move(valueVar), awst::WType::bytesType(), loc);

				// concat(bzero(32), bytes) → take last 32 bytes
				auto bz = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), loc);
				bz->stackArgs.push_back(makeUint64("32"));

				auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
				cat->stackArgs.push_back(std::move(bz));
				cat->stackArgs.push_back(std::move(cast));

				auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), loc);
				lenCall->stackArgs.push_back(cat);

				auto sub32 = awst::makeUInt64BinOp(std::move(lenCall), awst::UInt64BinaryOperator::Sub, makeUint64("32"), loc);

				auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), loc);
				extract->stackArgs.push_back(cat);
				extract->stackArgs.push_back(std::move(sub32));
				extract->stackArgs.push_back(makeUint64("32"));

				auto put = awst::makeIntrinsicCall("app_global_put", awst::WType::voidType(), loc);
				put->stackArgs.push_back(makeBytes(sv.name));
				put->stackArgs.push_back(std::move(extract));

				auto stmt = awst::makeExpressionStatement(std::move(put), loc);
				ifBlock->body.push_back(std::move(stmt));

				auto ret = awst::makeReturnStatement(nullptr, loc);
				ifBlock->body.push_back(std::move(ret));
			}

			auto ifElse = std::make_shared<awst::IfElse>();
			ifElse->sourceLocation = loc;
			ifElse->condition = std::move(cmp);
			ifElse->ifBranch = std::move(ifBlock);
			ifElse->elseBranch = std::move(elseBlock);

			auto newElse = std::make_shared<awst::Block>();
			newElse->sourceLocation = loc;
			newElse->body.push_back(std::move(ifElse));
			elseBlock = std::move(newElse);
		}

		for (auto& stmt: elseBlock->body)
			body->body.push_back(std::move(stmt));

		writeSub.body = body;
		_contractNode->methods.push_back(std::move(writeSub));
	}

	Logger::instance().debug(
		"Generated __storage_read/__storage_write dispatch for "
		+ std::to_string(layout.totalSlots()) + " slots", loc);
}


} // namespace puyasol::builder
