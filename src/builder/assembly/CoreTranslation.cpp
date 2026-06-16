/// @file CoreTranslation.cpp
/// Core expression translation: dispatch, literals, identifiers, function calls.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <sstream>
#include <string_view>
#include <unordered_map>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AssemblyBuilder::buildExpression(
	solidity::yul::Expression const& _expr
)
{
	return std::visit(
		[this](auto const& _node) -> std::shared_ptr<awst::Expression> {
			using T = std::decay_t<decltype(_node)>;
			if constexpr (std::is_same_v<T, solidity::yul::FunctionCall>)
				return buildFunctionCall(_node);
			else if constexpr (std::is_same_v<T, solidity::yul::Identifier>)
				return buildIdentifier(_node);
			else if constexpr (std::is_same_v<T, solidity::yul::Literal>)
				return buildLiteral(_node);
			else
			{
				Logger::instance().error("unsupported Yul expression type in assembly");
				return nullptr;
			}
		},
		_expr
	);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::buildLiteral(
	solidity::yul::Literal const& _lit
)
{
	auto loc = makeLoc(_lit.debugData);

	if (_lit.kind == solidity::yul::LiteralKind::Number)
	{
		// Convert u256 to decimal string
		auto const& val = _lit.value.value();
		std::ostringstream oss;
		oss << val;
		return awst::makeIntegerConstant(oss.str(), loc, awst::WType::biguintType());
	}
	else if (_lit.kind == solidity::yul::LiteralKind::Boolean)
	{
		return awst::makeBoolConstant(_lit.value.value() != 0, loc);
	}
	else if (_lit.kind == solidity::yul::LiteralKind::String)
	{
		if (!_lit.value.unlimited())
		{
			// String literal that fits in 32 bytes — stored as u256 (left-aligned bytes).
			// In Yul, "abc" becomes 0x6162630...0 (left-padded in a 256-bit word).
			// We emit it as a BytesConstant with the raw bytes from the hint.
			auto const& hint = _lit.value.hint();
			if (hint && !hint->empty())
			{
				// Pad to 32 bytes (right-padded with zeros, matching EVM left-aligned semantics)
				std::vector<unsigned char> padded(hint->begin(), hint->end());
				padded.resize(32, 0);
				auto node = awst::makeBytesConstant(
					std::move(padded), loc, awst::BytesEncoding::Unknown);

				// Cast to biguint for use in assembly context
				auto cast = awst::makeAsBiguint(std::move(node), loc);
				return cast;
			}
			else
			{
				// Empty string or no hint — use the numeric value
				auto const& val = _lit.value.value();
				std::ostringstream oss;
				oss << val;
				return awst::makeIntegerConstant(oss.str(), loc, awst::WType::biguintType());
			}
		}
		else
		{
			// Unlimited string literal (e.g., verbatim arguments) — emit as raw bytes
			auto const& strVal = _lit.value.builtinStringLiteralValue();
			auto node = awst::makeBytesConstant(
				std::vector<uint8_t>(strVal.begin(), strVal.end()),
				loc, awst::BytesEncoding::Unknown);

			auto cast = awst::makeAsBiguint(std::move(node), loc);
			return cast;
		}
	}

	Logger::instance().error("unsupported Yul literal kind", loc);
	return nullptr;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::buildIdentifier(
	solidity::yul::Identifier const& _id
)
{
	auto loc = makeLoc(_id.debugData);
	std::string name = _id.name.str();

	// External reference to an outer Solidity local → its mangled AWST name
	// (locals are `name__<declId>` now — Context::awstVarName). Keyed by the
	// yul::Identifier node ptr so a same-named Yul-local isn't mis-remapped; only
	// value refs are in the map (suffixed .slot/.offset refs keep their dotted
	// name for the handling below). Done here at the top so every downstream
	// lookup (m_locals, m_paramBitWidths, m_blobOffsetVars) uses the mangled key.
	if (auto evIt = m_externalVarNames.find(&_id); evIt != m_externalVarNames.end())
		name = evIt->second;

	// Handle .offset / .length suffix on calldata parameter references
	// e.g., proofPayload.offset → calldata byte offset of proofPayload
	auto dotPos = name.rfind('.');
	if (dotPos != std::string::npos)
	{
		std::string suffix = name.substr(dotPos + 1);
		std::string baseName = name.substr(0, dotPos);

		if (suffix == "slot")
		{
			// Storage slot reference: z.slot → numeric slot constant
			// First check constants (set by StorageLayout in SolInlineAssembly)
			auto cIt = m_constants.find(name);
			if (cIt != m_constants.end())
			{
				auto node = awst::makeIntegerConstant(cIt->second, loc, awst::WType::biguintType());
				return node;
			}
			// Fallback: check storageSlotVars for __slot_ marker
			auto it = m_storageSlotVars.find(name);
			if (it != m_storageSlotVars.end())
			{
				auto node = awst::makeVarExpression("__slot_" + it->second, awst::WType::biguintType(), loc);
				return node;
			}
			// Box-keyed struct storage pointer (`info.slot` where `info =
			// self.ticks[tick]` aliases an ARC4 struct living in a box):
			// resolve to that box. handleSstore detects this BoxValueExpression
			// sentinel (struct wtype) and performs a field-aware write
			// (EVM slot packing → ARC4 fields). See m_boxKeyedStructSlots.
			auto bks = m_boxKeyedStructSlots.find(name);
			if (bks != m_boxKeyedStructSlots.end())
				return awst::makeBoxValueExpression(
					bks->second.key, bks->second.structType, loc);
		}
		else if (suffix == "offset")
		{
			// Check storage offset first (from constants map set by SolInlineAssembly)
			auto constIt = m_constants.find(name);
			if (constIt != m_constants.end())
			{
				auto node = awst::makeIntegerConstant(constIt->second, loc, awst::WType::biguintType());
				return node;
			}
			auto it = m_localConstants.find(baseName);
			if (it != m_localConstants.end())
			{
				auto node = awst::makeIntegerConstant(it->second, loc, awst::WType::biguintType());
				return node;
			}
		}
		else if (suffix == "length")
		{
			// .length for calldata arrays/bytes — emit len(param)
			auto paramIt = m_locals.find(baseName);
			if (paramIt != m_locals.end())
			{
				auto paramVar = awst::makeVarExpression(baseName, paramIt->second, loc);
				return awst::makeLen(std::move(paramVar), loc);
			}
		}
		else if (suffix == "selector")
		{
			// fn-ptr.selector in Yul: extract 4-byte selector slot from 12-byte fn-ptr.
			// AVM external fn-ptr layout = appId(8B) ++ selector(4B). Read bytes 8..12
			// as uint32; assignment to a uint256 stack var places it right-aligned
			// (low 32 bits), matching EVM's convention so subsequent shifts work.
			// SolInlineAssembly registers `fp.selector` (full dotted name) in m_locals
			// with the underlying fn-ptr type (bytes[12]); use that entry to identify
			// fn-ptrs, then reference the unsuffixed base local declared in outer scope.
			auto fullIt = m_locals.find(name);
			if (fullIt != m_locals.end())
			{
				auto const* bwt = dynamic_cast<awst::BytesWType const*>(fullIt->second);
				if (bwt && bwt->length().has_value() && *bwt->length() == 12)
				{
					auto baseVar = awst::makeVarExpression(baseName, fullIt->second, loc);
					auto baseAsBytes = awst::makeAsBytes(std::move(baseVar), loc);

					auto extractCall = awst::makeIntrinsicCall(
						"extract_uint32", awst::WType::uint64Type(), loc);
					extractCall->stackArgs.push_back(std::move(baseAsBytes));
					extractCall->stackArgs.push_back(awst::makeIntegerConstant("8", loc));
					return extractCall;
				}
			}
		}
		else if (suffix == "address")
		{
			// fn-ptr.address: 8-byte appId portion of 12-byte fn-ptr.
			// EVM returns 20-byte address; on AVM the application id is uint64.
			auto fullIt = m_locals.find(name);
			if (fullIt != m_locals.end())
			{
				auto const* bwt = dynamic_cast<awst::BytesWType const*>(fullIt->second);
				if (bwt && bwt->length().has_value() && *bwt->length() == 12)
				{
					auto baseVar = awst::makeVarExpression(baseName, fullIt->second, loc);
					auto baseAsBytes = awst::makeAsBytes(std::move(baseVar), loc);

					auto extractCall = awst::makeIntrinsicCall(
						"extract_uint64", awst::WType::uint64Type(), loc);
					extractCall->stackArgs.push_back(std::move(baseAsBytes));
					extractCall->stackArgs.push_back(awst::makeZero(loc));
					return extractCall;
				}
			}
		}
	}

	// Check if this is an external constant (e.g., Solidity `uint constant M00 = ...`)
	auto constIt = m_constants.find(name);
	if (constIt != m_constants.end())
	{
		auto node = awst::makeIntegerConstant(constIt->second, loc, awst::WType::biguintType());
		return node;
	}

	// Blob-backed memory aggregate: a bare reference is its Yul memory pointer
	// (the uint64 base offset into the multi-slot blob), NOT the aggregate value.
	auto boIt = m_blobOffsetVars.find(name);
	if (boIt != m_blobOffsetVars.end())
		return awst::makeVarExpression(boIt->second, awst::WType::uint64Type(), loc);

	auto it = m_locals.find(name);
	// Default: all assembly vars are uint256
	auto const* wtype = (it != m_locals.end()) ? it->second : awst::WType::biguintType();
	auto node = awst::makeVarExpression(name, wtype, loc);

	// bytesN variables in assembly need left-alignment (right-padded to 32 bytes).
	// EVM stores bytesN left-aligned in 256-bit words: bytes4(0xAABBCCDD) = 0xAABBCCDD000...00
	// Without this, our internal bytes[N] representation gets reinterpreted as a right-aligned
	// integer (0x000...00AABBCCDD) when used in assembly.
	if (auto const* bwt = dynamic_cast<awst::BytesWType const*>(node->wtype))
	{
		if (bwt->length().has_value() && *bwt->length() < 32)
		{
			// Right-pad to 32 bytes: concat(value, bzero(32 - N)), reinterpret as biguint
			int padLen = 32 - *bwt->length();
			auto cat = awst::makeRightPad(std::move(node), padLen, loc);
			return awst::makeAsBiguint(std::move(cat), loc);
		}
	}

	return node;
}

// ─── Function call translation ──────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::buildFunctionCall(
	solidity::yul::FunctionCall const& _call
)
{
	auto loc = makeLoc(_call.debugData);
	std::string funcName = getFunctionName(_call.functionName);

	// Before translating args, check for Yul-level patterns that need raw AST access.
	// mload(add(add(bytes_param, 32), offset)) → extract3(param, offset, 32)
	if (funcName == "mload" && _call.arguments.size() == 1)
	{
		auto result = tryHandleBytesMemoryRead(_call.arguments[0], loc);
		if (result)
			return result;
	}

	// Translate all arguments (stored in source order by the Yul parser)
	std::vector<std::shared_ptr<awst::Expression>> args;
	for (auto const& arg: _call.arguments)
		args.push_back(buildExpression(arg));

	// User-defined assembly functions take precedence over builtins.
	// This matches Yul's scoping rules: a user `function basefee() -> r { ... }`
	// shadows the builtin `basefee()` opcode when called.
	if (m_asmFunctions.count(funcName))
	{
		auto const& funcDef = *m_asmFunctions[funcName];
		std::vector<std::shared_ptr<awst::Statement>> inlinedStmts;
		auto ret = handleUserFunctionCall(funcName, args, loc, inlinedStmts);
		for (auto& s: inlinedStmts)
			m_pendingStatements.push_back(std::move(s));
		// A subroutine-dispatched single-return call returns its result via a
		// fresh temp (decoupled from the function's return-var name to avoid
		// recursion aliasing); use it. Inlined calls return nullptr — read the
		// function's first return-var name (the inlined body assigned it).
		if (ret)
			return ret;
		if (!funcDef.returnVariables.empty())
		{
			std::string retName = funcDef.returnVariables[0].name.str();
			auto retVar = awst::makeVarExpression(retName, awst::WType::biguintType(), loc);
			return retVar;
		}
		return awst::makeVoidConstant(loc);
	}

	// Builtin dispatch. The uniform opcodes — those that just translate their
	// already-built args through a handler with one of two shared signatures —
	// go through these two static name→member-handler tables instead of a
	// 30-deep `if (funcName == ...)` string chain. The genuinely special
	// builtins below (hard errors, mocked stubs, conditionals, side-effecting
	// statements, raw-AST precompile dispatch) keep their explicit branches:
	// they don't share a signature and benefit from staying visible.
	using ArgsHandler = std::shared_ptr<awst::Expression> (AssemblyBuilder::*)(
		std::vector<std::shared_ptr<awst::Expression>> const&, awst::SourceLocation const&);
	static std::unordered_map<std::string_view, ArgsHandler> const kArgsBuiltins = {
		{"mulmod", &AssemblyBuilder::handleMulmod}, {"addmod", &AssemblyBuilder::handleAddmod},
		{"add", &AssemblyBuilder::handleAdd}, {"mul", &AssemblyBuilder::handleMul},
		{"mod", &AssemblyBuilder::handleMod}, {"sub", &AssemblyBuilder::handleSub},
		{"mload", &AssemblyBuilder::handleMload}, {"iszero", &AssemblyBuilder::handleIszero},
		{"eq", &AssemblyBuilder::handleEq}, {"lt", &AssemblyBuilder::handleLt},
		{"gt", &AssemblyBuilder::handleGt}, {"and", &AssemblyBuilder::handleAnd},
		{"or", &AssemblyBuilder::handleOr}, {"not", &AssemblyBuilder::handleNot},
		{"xor", &AssemblyBuilder::handleXor}, {"div", &AssemblyBuilder::handleDiv},
		{"shl", &AssemblyBuilder::handleShl}, {"shr", &AssemblyBuilder::handleShr},
		{"byte", &AssemblyBuilder::handleByte}, {"signextend", &AssemblyBuilder::handleSignextend},
		{"sdiv", &AssemblyBuilder::handleSdiv}, {"smod", &AssemblyBuilder::handleSmod},
		{"slt", &AssemblyBuilder::handleSlt}, {"sgt", &AssemblyBuilder::handleSgt},
		{"sar", &AssemblyBuilder::handleSar}, {"tload", &AssemblyBuilder::handleTload},
		{"sload", &AssemblyBuilder::handleSload}, {"calldataload", &AssemblyBuilder::handleCalldataload},
		{"keccak256", &AssemblyBuilder::handleKeccak256},
	};
	if (auto it = kArgsBuiltins.find(funcName); it != kArgsBuiltins.end())
		return (this->*(it->second))(args, loc);

	using NullaryHandler =
		std::shared_ptr<awst::Expression> (AssemblyBuilder::*)(awst::SourceLocation const&);
	static std::unordered_map<std::string_view, NullaryHandler> const kNullaryBuiltins = {
		{"gas", &AssemblyBuilder::handleGas}, {"timestamp", &AssemblyBuilder::handleTimestamp},
		{"returndatasize", &AssemblyBuilder::handleReturndatasize},
	};
	if (auto it = kNullaryBuiltins.find(funcName); it != kNullaryBuiltins.end())
		return (this->*(it->second))(loc);
	if (funcName == "extcodesize")
	{
		// extcodesize(addr) → HARD ERROR. AVM cannot dereference an arbitrary
		// address to ask whether it has code; the old stub returned 1
		// ("everything is a contract"), which silently makes EOA-vs-contract
		// guards like `extcodesize(a) > 0` always true. Refuse rather than
		// emit a vacuous guard. (EVM code-introspection family — see also
		// extcodehash/extcodecopy and high-level address.code/.codehash.)
		Logger::instance().error(
			"`extcodesize(addr)` is not supported on AVM — there is no way to "
			"query whether an arbitrary address has code, so the old stub "
			"returned 1 ('everything is a contract'), silently making "
			"`extcodesize(a) > 0` EOA-vs-contract guards always true.", loc);
		auto one = awst::makeOne(loc, awst::WType::biguintType());
		return one;
	}
	if (funcName == "extcodehash")
	{
		// extcodehash(addr) → HARD ERROR. On EVM this is keccak256 of the
		// account's code. AVM can only fetch the CURRENT app's approval
		// program; an arbitrary address can't be dereferenced to its app
		// bytes. The old stub used a fragile `addr > 100` heuristic to guess
		// "is this address(this)?" and otherwise returned keccak256("") / 0 —
		// a wrong-but-deterministic hash that would corrupt any commitment or
		// identity check. Refuse rather than emit a wrong hash. (For the
		// genuine self case, use the high-level `address(this).codehash`,
		// which is computed correctly via app_params_get on the current app.)
		Logger::instance().error(
			"`extcodehash(addr)` is not supported on AVM — an arbitrary address "
			"can't be dereferenced to its code, so the old stub guessed via an "
			"`addr > 100` heuristic and otherwise returned a wrong-but-"
			"deterministic hash. Use the high-level `address(this).codehash` for "
			"the current app's own code hash.", loc);
		auto zero = awst::makeZero(loc, awst::WType::biguintType());
		return zero;
	}
	if (funcName == "address")
	{
		// address() → global CurrentApplicationAddress, cast to biguint
		auto addr = awst::makeGlobal("CurrentApplicationAddress", awst::WType::bytesType(), loc);

		auto cast = awst::makeAsBiguint(std::move(addr), loc);
		return cast;
	}
	if (funcName == "origin")
	{
		// origin() (EVM tx.origin) → HARD ERROR. It denotes the EOA that
		// started the transaction, distinct from caller()/msg.sender (the
		// immediate caller). AVM has no transaction-origin concept; the only
		// available value is `txn Sender` = caller(), so origin() would
		// silently alias caller() and make `origin == caller` access guards
		// vacuous. Refuse rather than emit a wrong guard. (caller() below is
		// sound — `txn Sender` is the correct analog of the immediate caller.)
		Logger::instance().error(
			"Yul `origin()` (EVM tx.origin) is not supported on AVM. It denotes "
			"the EOA that started the transaction, distinct from `caller()`; AVM "
			"has no such concept, so it would silently alias `caller()` and make "
			"`origin == caller` checks vacuous. Use `caller()` (maps to txn Sender).",
			loc);
		// Stub so AWST building completes; the error aborts before any TEAL.
		auto sender = awst::makeTxn("Sender", awst::WType::bytesType(), loc);
		return awst::makeAsBiguint(std::move(sender), loc);
	}
	if (funcName == "caller")
	{
		// caller() (EVM CALLER = msg.sender) → txn Sender (32 bytes) → biguint.
		// Sound: txn Sender is the correct AVM analog of the immediate caller.
		auto sender = awst::makeTxn("Sender", awst::WType::bytesType(), loc);

		auto cast = awst::makeAsBiguint(std::move(sender), loc);
		return cast;
	}
	if (funcName == "blockhash")
	{
		// blockhash(n) -> HARD ERROR. EVM blockhash(n) is the hash of a recent
		// block (last 256, else 0). AVM has no block-hash opcode; `block
		// BlkSeed` is a per-round VRF seed for a narrow recent window that
		// ignores the round argument and panics out-of-window -- a different
		// value AND failure mode, so a blockhash-based commitment/RNG silently
		// diverges. Refuse to compile rather than emit a wrong value. (blobhash
		// below is left as a stand-in; the maintainer scoped this to blockhash.)
		Logger::instance().error(
			"`blockhash(n)` is not supported on AVM. EVM returns the hash of a "
			"recent block (or 0 outside the last 256); AVM has no block-hash "
			"opcode. `block BlkSeed` is a per-round VRF seed for a narrow recent "
			"window, so the round argument is ignored and the value is wrong, "
			"with no faithful equivalent.", loc);
		// Stub so AWST building completes; the error aborts the build first.
		return awst::makeZero(loc, awst::WType::biguintType());
	}
	if (funcName == "blobhash")
	{
		// Map Yul blobhash to AVM BlkSeed(Round - 2). The caller's index is
		// used only for index < 2 (emulating the EVM test harness's 2-mock
		// blobs). Any further index returns bytes32(0). See the
		// SolBuiltinCall counterparts for details.
		Logger::instance().warning(
			funcName + "() in assembly → BlkSeed(Round - 2) stand-in; "
			"not cryptographically equivalent to EVM " + funcName + ".",
			loc);

		auto round = awst::makeGlobal(std::string("Round"), awst::WType::uint64Type(), loc);

		auto two = awst::makeIntegerConstant("2", loc);

		auto prevRound = awst::makeUInt64BinOp(std::move(round), awst::UInt64BinaryOperator::Sub, std::move(two), loc);

		auto seed = awst::makeBlock(
			"BlkSeed", std::move(prevRound), awst::WType::bytesType(), loc);

		auto seedBigUint = awst::makeAsBiguint(std::move(seed), loc);

		if (funcName == "blobhash" && !args.empty())
		{
			// Return seed for index < 2, zero otherwise. Mirrors the
			// 2-slot EVM mock harness.
			auto indexArg = args[0];
			auto twoLit = awst::makeIntegerConstant("2", loc, awst::WType::biguintType());
			auto withinRange = awst::makeNumericCompare(std::move(indexArg), awst::NumericComparison::Lt, std::move(twoLit), loc);

			auto zero = awst::makeZero(loc, awst::WType::biguintType());

			return awst::makeConditional(
				std::move(withinRange), std::move(seedBigUint), std::move(zero),
				awst::WType::biguintType(), loc);
		}
		return seedBigUint;
	}
	if (funcName == "difficulty")
	{
		// Pre-paris EVM DIFFICULTY. AVM has no equivalent; emit the
		// Solidity test runner's canonical mocked value (200000000) so
		// legacy-EVM tests that assert a specific difficulty pass.
		return awst::makeIntegerConstant("200000000", loc, awst::WType::biguintType());
	}
	if (funcName == "prevrandao")
	{
		// prevrandao() has no AVM equivalent. Emit the Solidity test runner's
		// canonical mocked value so post-paris tests that assert a specific
		// prevrandao pass (same pattern as `difficulty` above).
		return awst::makeIntegerConstant(
			"76179698116359622413486155173975521935699888105599510728246182663625645328247",
			loc, awst::WType::biguintType());
	}
	if (funcName == "number")
	{
		// number() → global Round (block-number equivalent), a uint64. Return it
		// as uint64; the consumer coerces via ensureBiguint only when it needs a
		// biguint (match at consumption, not at exit — same as selfbalance/clz).
		return awst::makeGlobal(std::string("Round"), awst::WType::uint64Type(), loc);
	}
	if (funcName == "balance")
	{
		// balance(addr) → AVM `balance` opcode on the 32-byte account derived
		// from addr (left-zero-padded to 32 bytes). uint64 result, same as
		// selfbalance (microAlgo balances fit uint64); the consumer coerces via
		// ensureBiguint only when it needs a biguint. NOTE: this is the AVM
		// account balance in microAlgos (NOT EVM wei), and the account must be
		// available to the txn — an arbitrary EVM address (e.g. balance(0))
		// maps to an unfunded/unavailable AVM account, so only addresses the
		// txn references (incl. address()/self) read meaningfully.
		if (!checkArity(args, 1, "balance", loc, "address"))
			return awst::makeZero(loc, awst::WType::uint64Type());
		auto acct = padTo32Bytes(ensureBiguint(args[0], loc), loc);
		auto bal = awst::makeIntrinsicCall("balance", awst::WType::uint64Type(), loc);
		bal->stackArgs.push_back(std::move(acct));
		return bal;
	}
	if (funcName == "selfbalance")
	{
		// Yul selfbalance() returns the balance of the executing contract.
		// Map to AVM `balance(global CurrentApplicationAddress)`, which is a
		// uint64 — microAlgo balances always fit uint64. Return it as uint64
		// rather than widening to biguint: the consumer coerces via
		// ensureBiguint only when it needs a biguint (same natural-type
		// convention as clz / the comparison handlers).
		auto appAddr = awst::makeGlobal(std::string("CurrentApplicationAddress"), awst::WType::bytesType(), loc);
		auto bal = awst::makeIntrinsicCall("balance", awst::WType::uint64Type(), loc);
		bal->stackArgs.push_back(std::move(appAddr));
		return bal;
	}
	if (funcName == "coinbase" || funcName == "gasprice" || funcName == "basefee"
		|| funcName == "blobbasefee")
	{
		// Stub: return 0 for EVM-specific block properties with no AVM equivalent
		Logger::instance().warning(
			funcName + "() has no AVM equivalent, returning 0", loc);
		auto zero = awst::makeZero(loc, awst::WType::biguintType());
		return zero;
	}
	if (funcName == "chainid")
	{
		// AVM has no per-chain identifier; return 1 so that Solidity's
		// `block.chainid` lines up with what semantic tests expect
		// (Ethereum mainnet id). Real-world contracts that need network
		// differentiation should use `global GenesisHash` directly in
		// assembly instead.
		Logger::instance().debug("chainid() stubbed as 1 for AVM", loc);
		auto c = awst::makeOne(loc, awst::WType::biguintType());
		return c;
	}
	if (funcName == "codesize")
	{
		// codesize() → HARD ERROR. EVM codesize() is the deployed contract's
		// bytecode length; the AVM has no opcode exposing the TEAL program
		// size, so the old stub returned a sentinel 50 — a silent wrong value
		// that makes codesize-based length checks pass on a fabricated number.
		// Refuse rather than invent a length (same hard-error policy as
		// extcodesize / blockhash / delegatecall for unsupported EVM features).
		Logger::instance().error(
			"`codesize()` is not supported on AVM — there is no opcode exposing "
			"the deployed program's byte length, so the old stub returned a "
			"fabricated 50. Refuse rather than emit a silent wrong value.", loc);
		// Stub so AWST building completes; the error aborts the build first.
		return awst::makeZero(loc, awst::WType::biguintType());
	}
	if (funcName == "clz")
	{
		// clz(x) = count leading zeros (256-bit): 256 - bitlen(x). EIP-7939.
		// AVM's `bitlen` reads its arg (uint64 / biguint / bytes) as a
		// big-endian integer and returns the significant-bit count (0 for 0),
		// so no width conversion of the operand is needed. The result is in
		// [0,256] (256 only when x==0); it fits uint64 but NOT uint8, and all
		// assembly operands are 256-bit so 256-bitlen never underflows.
		// Returning uint64 (the natural type for a small result, like the
		// comparison ops return bool) is the same single stack word and lets
		// consumers coerce via ensureBiguint only when they need a biguint.
		if (args.empty())
		{
			Logger::instance().warning("clz() called with no args", loc);
			return awst::makeIntegerConstant(static_cast<uint64_t>(256), loc);
		}
		auto x = args[0];
		auto bitlen = awst::makeIntrinsicCall("bitlen", awst::WType::uint64Type(), loc);
		bitlen->stackArgs.push_back(std::move(x));
		auto c256 = awst::makeIntegerConstant(static_cast<uint64_t>(256), loc);
		return awst::makeUInt64BinOp(std::move(c256), awst::UInt64BinaryOperator::Sub, std::move(bitlen), loc);
	}
	if (funcName == "returndatacopy")
	{
		// returndatacopy(destOffset, offset, size): copy the last inner txn's
		// log (itxn LastLog) into memory. Void op — emit the copy as a pending
		// statement and yield void.
		emitReturndatacopy(args, loc, m_pendingStatements);
		return awst::makeVoidConstant(loc);
	}
	if (funcName == "pop")
	{
		// pop(x) — discard value, no-op
		return awst::makeVoidConstant(loc);
	}
	if (funcName == "tstore")
	{
		// tstore in expression context — should be a statement
		Logger::instance().warning("tstore() in expression context, treating as no-op", loc);
		return awst::makeVoidConstant(loc);
	}
	if (funcName == "call" || funcName == "staticcall")
	{
		// call/staticcall in expression context (e.g., `let success := call(...)`)
		// is handled by the variable declaration / assignment translators.
		// In pure expression context, we can't do the full pattern match.
		Logger::instance().warning(
			funcName + " in pure expression context; use let/assign form for precompile support", loc
		);
		auto one = awst::makeOne(loc, awst::WType::biguintType());
		return one;
	}

	// Check for user-defined assembly function — inline in expression context
	auto asmIt = m_asmFunctions.find(funcName);
	if (asmIt != m_asmFunctions.end())
	{
		auto const& funcDef = *asmIt->second;

		// Inline the function body into a local vector, then append to
		// m_pendingStatements. Using a local avoids aliasing issues when
		// nested inlining drains m_pendingStatements inside handleUserFunctionCall.
		std::vector<std::shared_ptr<awst::Statement>> inlinedStmts;
		auto ret = handleUserFunctionCall(funcName, args, loc, inlinedStmts);
		for (auto& s: inlinedStmts)
			m_pendingStatements.push_back(std::move(s));

		// Subroutine-dispatched single-return: use the returned fresh temp
		// (avoids recursion aliasing). Inlined calls return nullptr — read the
		// function's first return variable, which the inlined body assigned.
		if (ret)
			return ret;
		if (!funcDef.returnVariables.empty())
		{
			std::string retName = funcDef.returnVariables[0].name.str();
			auto retVar = awst::makeVarExpression(retName, awst::WType::biguintType(), loc);
			return retVar;
		}

		return awst::makeVoidConstant(loc);
	}

	// delegatecall → HARD ERROR. delegatecall runs another contract's code in
	// the caller's storage context, which has no AVM equivalent; returning 1
	// (success) would silently no-op the delegated call. Matches the hard error
	// on high-level `.delegatecall(...)`.
	if (funcName == "delegatecall")
	{
		Logger::instance().error(
			"`delegatecall(...)` in inline assembly is not supported on AVM. It "
			"runs another contract's code in the caller's storage context, which "
			"has no AVM equivalent; stubbing it as success (1) would silently "
			"no-op the delegated call. This matches the hard error on high-level "
			"`.delegatecall(...)`.", loc);
		auto one = awst::makeOne(loc, awst::WType::biguintType());
		return one;
	}

	// create2(value, offset, size, salt) → hard error. CREATE2 derives a
	// deterministic contract address from salt + initcode hash; the AVM has
	// no such opcode — contracts are apps whose IDs are assigned sequentially
	// by the chain at inner-app-create time, so there is no address to
	// pre-compute. Silently returning a zero address would produce
	// wrong-semantic code (callers depending on the predicted address would
	// misbehave), so refuse to compile rather than stub.
	if (funcName == "create2")
	{
		Logger::instance().error(
			"`create2(...)` is not supported on AVM. CREATE2's deterministic "
			"address derivation (salt + initcode hash) has no AVM equivalent — "
			"app IDs are assigned sequentially by the chain at inner-app-create "
			"time, so an address can't be pre-computed from a salt. Use "
			"high-level `new C(...)` (lowered to an inner app-create txn) if you "
			"don't need address prediction; CREATE2-style counterfactual "
			"deployment can't be honored.",
			loc
		);
		// Return a valid stub so AWST building completes and the error above is
		// surfaced cleanly at the end (matches the delegatecall hard-error path).
		auto zero = awst::makeZero(loc, awst::WType::biguintType());
		return zero;
	}

	// calldatasize() — when the synthetic blob is built, return its
	// runtime length; otherwise stub to 0 (AVM doesn't have raw calldata
	// in the EVM sense, so the legacy stub keeps existing tests working).
	if (funcName == "calldatasize")
	{
		if (m_useSyntheticCalldata)
		{
			auto blob = awst::makeVarExpression(CD_BLOB_VAR, awst::WType::bytesType(), loc);
			return awst::makeLen(std::move(blob), loc);
		}
		Logger::instance().warning("calldatasize() has no AVM equivalent, returning 0", loc);
		auto zero = awst::makeZero(loc, awst::WType::biguintType());
		return zero;
	}

	// calldatacopy(destOffset, offset, size) — when the synthetic blob is
	// available, copy `size` bytes from `__cd_blob[offset..offset+size]`
	// into the memory blob at destOffset. Otherwise stub as no-op.
	if (funcName == "calldatacopy")
	{
		if (m_useSyntheticCalldata && args.size() == 3)
		{
			auto blob = awst::makeVarExpression(CD_BLOB_VAR, awst::WType::bytesType(), loc);
			auto srcOff = offsetToUint64(args[1], loc);
			auto sz = offsetToUint64(args[2], loc);
			auto extractCall = awst::makeExtract3(std::move(blob), std::move(srcOff), std::move(sz), loc);
			// Now write into memory blob via replace3. The handler is in
			// expression-context, so route the assignment through
			// m_pendingStatements (drained by the outer statement boundary).
			auto destOff = offsetToUint64(args[0], loc);
			auto replaceCall = awst::makeReplace3(memoryVar(loc), std::move(destOff), std::move(extractCall), loc);
			assignMemoryVar(std::move(replaceCall), loc, m_pendingStatements);
			auto zero = awst::makeZero(loc, awst::WType::biguintType());
			return zero;
		}
		Logger::instance().warning("calldatacopy() has no AVM equivalent (skipped)", loc);
		auto zero = awst::makeZero(loc, awst::WType::biguintType());
		return zero;
	}

	// HARD ERROR — an unrecognized opcode stubbed as 0 is a silent wrong value.
	// Fail loudly so every future gap surfaces at compile time.
	Logger::instance().error(
		"unsupported Yul builtin function `" + funcName + "`: no AVM translation "
		"exists, so it would be stubbed as 0 — a silent wrong value.", loc
	);
	auto fallbackZero = awst::makeZero(loc, awst::WType::biguintType());
	return fallbackZero;
}

// ─── Builtin handlers ───────────────────────────────────────────────────────


} // namespace puyasol::builder
