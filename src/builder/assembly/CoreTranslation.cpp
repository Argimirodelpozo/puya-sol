/// @file CoreTranslation.cpp
/// Core expression translation: dispatch, literals, identifiers, function calls.

#include "builder/assembly/AssemblyBuilder.h"
#include "Logger.h"

#include <sstream>

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
		auto node = std::make_shared<awst::IntegerConstant>();
		node->sourceLocation = loc;
		node->wtype = awst::WType::biguintType();

		// Convert u256 to decimal string
		auto const& val = _lit.value.value();
		std::ostringstream oss;
		oss << val;
		node->value = oss.str();
		return node;
	}
	else if (_lit.kind == solidity::yul::LiteralKind::Boolean)
	{
		auto node = std::make_shared<awst::BoolConstant>();
		node->sourceLocation = loc;
		node->wtype = awst::WType::boolType();
		node->value = (_lit.value.value() != 0);
		return node;
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
				auto node = std::make_shared<awst::BytesConstant>();
				node->sourceLocation = loc;
				node->wtype = awst::WType::bytesType();
				// Pad to 32 bytes (right-padded with zeros, matching EVM left-aligned semantics)
				std::vector<unsigned char> padded(hint->begin(), hint->end());
				padded.resize(32, 0);
				node->value = std::move(padded);

				// Cast to biguint for use in assembly context
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = awst::WType::biguintType();
				cast->expr = std::move(node);
				return cast;
			}
			else
			{
				// Empty string or no hint — use the numeric value
				auto node = std::make_shared<awst::IntegerConstant>();
				node->sourceLocation = loc;
				node->wtype = awst::WType::biguintType();
				auto const& val = _lit.value.value();
				std::ostringstream oss;
				oss << val;
				node->value = oss.str();
				return node;
			}
		}
		else
		{
			// Unlimited string literal (e.g., verbatim arguments) — emit as raw bytes
			auto const& strVal = _lit.value.builtinStringLiteralValue();
			auto node = std::make_shared<awst::BytesConstant>();
			node->sourceLocation = loc;
			node->wtype = awst::WType::bytesType();
			node->value = std::vector<unsigned char>(strVal.begin(), strVal.end());

			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(node);
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
				auto node = std::make_shared<awst::IntegerConstant>();
				node->sourceLocation = loc;
				node->wtype = awst::WType::biguintType();
				node->value = cIt->second;
				return node;
			}
			// Fallback: check storageSlotVars for __slot_ marker
			auto it = m_storageSlotVars.find(name);
			if (it != m_storageSlotVars.end())
			{
				auto node = std::make_shared<awst::VarExpression>();
				node->sourceLocation = loc;
				node->wtype = awst::WType::biguintType();
				node->name = "__slot_" + it->second;
				return node;
			}
		}
		else if (suffix == "offset")
		{
			// Check storage offset first (from constants map set by SolInlineAssembly)
			auto constIt = m_constants.find(name);
			if (constIt != m_constants.end())
			{
				auto node = std::make_shared<awst::IntegerConstant>();
				node->sourceLocation = loc;
				node->wtype = awst::WType::biguintType();
				node->value = constIt->second;
				return node;
			}
			auto it = m_localConstants.find(baseName);
			if (it != m_localConstants.end())
			{
				auto node = std::make_shared<awst::IntegerConstant>();
				node->sourceLocation = loc;
				node->wtype = awst::WType::biguintType();
				node->value = std::to_string(it->second);
				return node;
			}
		}
		else if (suffix == "length")
		{
			// .length for calldata arrays/bytes — emit len(param)
			auto paramIt = m_locals.find(baseName);
			if (paramIt != m_locals.end())
			{
				auto paramVar = std::make_shared<awst::VarExpression>();
				paramVar->sourceLocation = loc;
				paramVar->name = baseName;
				paramVar->wtype = paramIt->second;

				auto lenCall = std::make_shared<awst::IntrinsicCall>();
				lenCall->sourceLocation = loc;
				lenCall->wtype = awst::WType::uint64Type();
				lenCall->opCode = "len";
				lenCall->stackArgs.push_back(std::move(paramVar));
				return lenCall;
			}
		}
	}

	// Check if this is an external constant (e.g., Solidity `uint constant M00 = ...`)
	auto constIt = m_constants.find(name);
	if (constIt != m_constants.end())
	{
		auto node = std::make_shared<awst::IntegerConstant>();
		node->sourceLocation = loc;
		node->wtype = awst::WType::biguintType();
		node->value = constIt->second;
		return node;
	}

	auto node = std::make_shared<awst::VarExpression>();
	node->sourceLocation = loc;
	node->name = name;

	auto it = m_locals.find(name);
	if (it != m_locals.end())
		node->wtype = it->second;
	else
		node->wtype = awst::WType::biguintType(); // Default: all assembly vars are uint256

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
			auto pad = std::make_shared<awst::IntrinsicCall>();
			pad->sourceLocation = loc;
			pad->wtype = awst::WType::bytesType();
			pad->opCode = "bzero";
			auto padSize = std::make_shared<awst::IntegerConstant>();
			padSize->sourceLocation = loc;
			padSize->wtype = awst::WType::uint64Type();
			padSize->value = std::to_string(padLen);
			pad->stackArgs.push_back(std::move(padSize));

			auto cat = std::make_shared<awst::IntrinsicCall>();
			cat->sourceLocation = loc;
			cat->wtype = awst::WType::bytesType();
			cat->opCode = "concat";
			cat->stackArgs.push_back(std::move(node));
			cat->stackArgs.push_back(std::move(pad));

			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(cat);
			return cast;
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
		handleUserFunctionCall(funcName, args, loc, inlinedStmts);
		for (auto& s: inlinedStmts)
			m_pendingStatements.push_back(std::move(s));
		if (!funcDef.returnVariables.empty())
		{
			std::string retName = funcDef.returnVariables[0].name.str();
			auto retVar = std::make_shared<awst::VarExpression>();
			retVar->sourceLocation = loc;
			retVar->name = retName;
			retVar->wtype = awst::WType::biguintType();
			return retVar;
		}
		return std::make_shared<awst::VoidConstant>();
	}

	// Builtin dispatch
	if (funcName == "mulmod")
		return handleMulmod(args, loc);
	if (funcName == "addmod")
		return handleAddmod(args, loc);
	if (funcName == "add")
		return handleAdd(args, loc);
	if (funcName == "mul")
		return handleMul(args, loc);
	if (funcName == "mod")
		return handleMod(args, loc);
	if (funcName == "sub")
		return handleSub(args, loc);
	if (funcName == "mload")
		return handleMload(args, loc);
	if (funcName == "iszero")
		return handleIszero(args, loc);
	if (funcName == "eq")
		return handleEq(args, loc);
	if (funcName == "lt")
		return handleLt(args, loc);
	if (funcName == "gt")
		return handleGt(args, loc);
	if (funcName == "and")
		return handleAnd(args, loc);
	if (funcName == "or")
		return handleOr(args, loc);
	if (funcName == "not")
		return handleNot(args, loc);
	if (funcName == "xor")
		return handleXor(args, loc);
	if (funcName == "div")
		return handleDiv(args, loc);
	if (funcName == "shl")
		return handleShl(args, loc);
	if (funcName == "shr")
		return handleShr(args, loc);
	if (funcName == "byte")
		return handleByte(args, loc);
	if (funcName == "signextend")
		return handleSignextend(args, loc);
	if (funcName == "sdiv")
		return handleSdiv(args, loc);
	if (funcName == "smod")
		return handleSmod(args, loc);
	if (funcName == "slt")
		return handleSlt(args, loc);
	if (funcName == "sgt")
		return handleSgt(args, loc);
	if (funcName == "sar")
		return handleSar(args, loc);
	if (funcName == "tload")
		return handleTload(args, loc);
	if (funcName == "sload")
		return handleSload(args, loc);
	if (funcName == "gas")
		return handleGas(loc);
	if (funcName == "extcodesize")
	{
		// extcodesize(addr) → on AVM, return 1 (treat all addresses as having code)
		Logger::instance().warning("extcodesize() stubbed as 1 (no AVM equivalent)", loc);
		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = loc;
		one->wtype = awst::WType::biguintType();
		one->value = "1";
		return one;
	}
	if (funcName == "address")
	{
		// address() → global CurrentApplicationAddress, cast to biguint
		auto addr = std::make_shared<awst::IntrinsicCall>();
		addr->sourceLocation = loc;
		addr->wtype = awst::WType::bytesType();
		addr->opCode = "global";
		addr->immediates.push_back("CurrentApplicationAddress");

		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(addr);
		return cast;
	}
	if (funcName == "caller" || funcName == "origin")
	{
		// caller() / origin() → txn Sender (32 bytes) → reinterpret as biguint
		auto sender = std::make_shared<awst::IntrinsicCall>();
		sender->sourceLocation = loc;
		sender->wtype = awst::WType::bytesType();
		sender->opCode = "txn";
		sender->immediates.push_back("Sender");

		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(sender);
		return cast;
	}
	if (funcName == "timestamp")
		return handleTimestamp(loc);
	if (funcName == "prevrandao" || funcName == "difficulty")
	{
		// prevrandao()/difficulty() → deterministic hash as biguint stub
		// AVM has no PREVRANDAO; return sha256 of a constant as non-zero placeholder.
		Logger::instance().warning(
			"prevrandao()/difficulty() has no AVM equivalent, returning deterministic stub", loc);
		auto hashInput = std::make_shared<awst::BytesConstant>();
		hashInput->sourceLocation = loc;
		hashInput->wtype = awst::WType::bytesType();
		hashInput->encoding = awst::BytesEncoding::Utf8;
		hashInput->value = std::vector<uint8_t>{'p','r','e','v','r','a','n','d','a','o'};
		auto hash = std::make_shared<awst::IntrinsicCall>();
		hash->sourceLocation = loc;
		hash->wtype = awst::WType::bytesType();
		hash->opCode = "sha256";
		hash->stackArgs.push_back(std::move(hashInput));
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(hash);
		return cast;
	}
	if (funcName == "number")
	{
		// number() → global Round (block number equivalent)
		auto round = std::make_shared<awst::IntrinsicCall>();
		round->sourceLocation = loc;
		round->wtype = awst::WType::uint64Type();
		round->opCode = "global";
		round->immediates = {std::string("Round")};
		// Convert to biguint (EVM returns uint256)
		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(round));
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(itob);
		return cast;
	}
	if (funcName == "coinbase" || funcName == "gasprice" || funcName == "basefee"
		|| funcName == "blobbasefee" || funcName == "selfbalance")
	{
		// Stub: return 0 for EVM-specific block properties with no AVM equivalent
		Logger::instance().warning(
			funcName + "() has no AVM equivalent, returning 0", loc);
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";
		return zero;
	}
	if (funcName == "chainid")
	{
		// chainid() → global GenesisHash (32 bytes) → reinterpret as biguint
		// AVM has no chain ID; GenesisHash uniquely identifies the network.
		Logger::instance().debug(
			"chainid() mapped to global GenesisHash (network identifier)", loc);
		auto ghCall = std::make_shared<awst::IntrinsicCall>();
		ghCall->sourceLocation = loc;
		ghCall->wtype = awst::WType::bytesType();
		ghCall->opCode = "global";
		ghCall->immediates = {std::string("GenesisHash")};

		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = loc;
		cast->wtype = awst::WType::biguintType();
		cast->expr = std::move(ghCall);
		return cast;
	}
	if (funcName == "calldataload")
		return handleCalldataload(args, loc);
	if (funcName == "keccak256")
		return handleKeccak256(args, loc);
	if (funcName == "returndatasize")
		return handleReturndatasize(loc);
	if (funcName == "returndatacopy")
	{
		// returndatacopy(destOffset, offset, size) — no-op on AVM (no return data)
		return std::make_shared<awst::VoidConstant>();
	}
	if (funcName == "pop")
	{
		// pop(x) — discard value, no-op
		return std::make_shared<awst::VoidConstant>();
	}
	if (funcName == "tstore")
	{
		// tstore in expression context — should be a statement
		Logger::instance().warning("tstore() in expression context, treating as no-op", loc);
		return std::make_shared<awst::VoidConstant>();
	}
	if (funcName == "call" || funcName == "staticcall")
	{
		// call/staticcall in expression context (e.g., `let success := call(...)`)
		// is handled by the variable declaration / assignment translators.
		// In pure expression context, we can't do the full pattern match.
		Logger::instance().warning(
			funcName + " in pure expression context; use let/assign form for precompile support", loc
		);
		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = loc;
		one->wtype = awst::WType::biguintType();
		one->value = "1";
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
		handleUserFunctionCall(funcName, args, loc, inlinedStmts);
		for (auto& s: inlinedStmts)
			m_pendingStatements.push_back(std::move(s));

		// Return a reference to the first return variable
		if (!funcDef.returnVariables.empty())
		{
			std::string retName = funcDef.returnVariables[0].name.str();
			auto retVar = std::make_shared<awst::VarExpression>();
			retVar->sourceLocation = loc;
			retVar->name = retName;
			retVar->wtype = awst::WType::biguintType();
			return retVar;
		}

		return std::make_shared<awst::VoidConstant>();
	}

	// delegatecall → stub: return 1 (success)
	if (funcName == "delegatecall")
	{
		Logger::instance().warning("delegatecall() stubbed as success (1)", loc);
		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = loc;
		one->wtype = awst::WType::biguintType();
		one->value = "1";
		return one;
	}

	// create2(value, offset, size, salt) → stub: return 0 (no EVM contract deployment on AVM)
	if (funcName == "create2")
	{
		Logger::instance().warning(
			"create2() has no AVM equivalent (requires inner app creation txn), returning zero address",
			loc
		);
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";
		return zero;
	}

	// calldatasize() → 0 (AVM doesn't have raw calldata in the EVM sense)
	if (funcName == "calldatasize")
	{
		Logger::instance().warning("calldatasize() has no AVM equivalent, returning 0", loc);
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";
		return zero;
	}

	// calldatacopy(destOffset, offset, size) → no-op (EVM calldata forwarding)
	if (funcName == "calldatacopy")
	{
		Logger::instance().warning("calldatacopy() has no AVM equivalent (skipped)", loc);
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = loc;
		zero->wtype = awst::WType::biguintType();
		zero->value = "0";
		return zero;
	}

	Logger::instance().warning(
		"unsupported Yul builtin function: " + funcName + ", returning 0", loc
	);
	auto fallbackZero = std::make_shared<awst::IntegerConstant>();
	fallbackZero->sourceLocation = loc;
	fallbackZero->wtype = awst::WType::biguintType();
	fallbackZero->value = "0";
	return fallbackZero;
}

// ─── Builtin handlers ───────────────────────────────────────────────────────


} // namespace puyasol::builder
