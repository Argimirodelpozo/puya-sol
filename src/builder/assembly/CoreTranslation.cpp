/// @file CoreTranslation.cpp
/// Core expression translation: dispatch, literals, identifiers, function calls.

#include "builder/assembly/AssemblyBuilder.h"
#include "builder/EvmFeaturePolicy.h"
#include "builder/sol-types/FunctionPointerKind.h"
#include "Logger.h"
#include "awst/NameGen.h"

#include <libevmasm/Instruction.h>
#include <libevmasm/SemanticInformation.h>

#include <algorithm>
#include <cctype>
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

std::string AssemblyBuilder::externalRefAwstName(
	solidity::frontend::InlineAssemblyAnnotation::ExternalIdentifierInfo const& _info,
	std::string const& _bareName,
	std::function<std::string(solidity::frontend::VariableDeclaration const&)> const& _declName)
{
	auto const* vd = dynamic_cast<solidity::frontend::VariableDeclaration const*>(_info.declaration);
	// Rename only outer Solidity LOCALS to the mangled AWST name (value refs +
	// fn-ptr .selector/.address — the dotted base must mangle for the downstream
	// dotPos split). State vars/constants/.slot/.offset/.length keep the bare Yul
	// name; their meaning comes from the storage/calldata machinery, not identity.
	bool const eligible = vd && _declName && !vd->isStateVariable() && !vd->isConstant()
		&& (_info.suffix.empty() || _info.suffix == "selector" || _info.suffix == "address");
	if (!eligible)
		return _bareName;
	return _declName(*vd) + (_info.suffix.empty() ? "" : "." + _info.suffix);
}

std::string AssemblyBuilder::resolveVarRef(solidity::yul::Identifier const& _id) const
{
	// solc lists every outer-var Yul reference in externalReferences (yul id →
	// {decl, suffix}); name it via the shared decl path (m_declName = awstVarName).
	// Yul-internal ids (let-locals, Yul-fn params — not in the map) keep their name.
	auto it = m_externalRefs.find(&_id);
	if (it == m_externalRefs.end())
	{
		// Yul-internal id (let-local or user-fn param/return). If this name is being
		// inline-expanded under a per-call rename, use the unique name so sibling/nested
		// calls that reuse the same bare name don't clobber each other's runtime vars.
		auto rit = m_yulInlineRenames.find(_id.name.str());
		if (rit != m_yulInlineRenames.end())
			return rit->second;
		return _id.name.str();
	}
	return externalRefAwstName(it->second, _id.name.str(), m_declName);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::buildIdentifier(
	solidity::yul::Identifier const& _id
)
{
	auto loc = makeLoc(_id.debugData);
	// resolveVarRef remaps outer-var refs to the mangled AWST name up front, so every
	// downstream lookup (m_locals, m_blobOffsetVars, the dotPos split) uses that key.
	std::string name = resolveVarRef(_id);

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
			// Struct-storage-ref local modeled as a biguint slot handle: `ptr.slot`
			// is the handle itself (see SolInlineAssembly::structRefSlotLocals).
			auto srit = m_structRefSlotLocals.find(name);
			if (srit != m_structRefSlotLocals.end())
				return awst::makeVarExpression(
					srit->second, awst::WType::biguintType(), loc);
			// Struct storage-ref PARAM passed as a box-key handle (bytes): `s.slot`
			// is a BoxValueExpression over the param's box key — handleSload/handleSstore
			// do the field-aware box read/write, exactly like m_boxKeyedStructSlots.
			// (solady storage-lib idiom; see setBoxKeyStructParams.)
			auto bkp = m_boxKeyStructParams.find(baseName);
			if (bkp != m_boxKeyStructParams.end())
				return awst::makeBoxValueExpression(
					awst::makeReinterpretCast(
						awst::makeVarExpression(baseName, awst::WType::bytesType(), loc),
						awst::WType::boxKeyType(), loc),
					bkp->second, loc);
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
				// Dynamic calldata param: .offset is the mutable __cd_off_<name> local (seeded from
				// __cd_blob at block entry, reassignable via `x.offset := V`). Static params keep the
				// constant head position.
				auto typeIt = m_locals.find(baseName);
				if (m_useSyntheticCalldata && typeIt != m_locals.end()
					&& isDynamicCalldataType(typeIt->second))
					return awst::makeVarExpression("__cd_off_" + baseName, awst::WType::biguintType(), loc);
				return awst::makeIntegerConstant(it->second, loc, awst::WType::biguintType());
			}
		}
		else if (suffix == "length")
		{
			auto paramIt = m_locals.find(baseName);
			if (paramIt != m_locals.end())
			{
				// Dynamic calldata param: .length is the mutable __cd_len_<name> local (seeded from the
				// EVM-ABI length word in __cd_blob, reassignable via `x.length := L`).
				auto cdIt = m_localConstants.find(baseName);
				if (m_useSyntheticCalldata && cdIt != m_localConstants.end()
					&& isDynamicCalldataType(paramIt->second))
					return awst::makeVarExpression("__cd_len_" + baseName, awst::WType::biguintType(), loc);
				// Fallback: len() of the decoded value (correct for bytes/string without the blob).
				auto paramVar = awst::makeVarExpression(baseName, paramIt->second, loc);
				return awst::makeLen(std::move(paramVar), loc);
			}
		}
		else if (suffix == "selector")
		{
			// fn-ptr.selector in Yul: extract the Solidity-visible 4-byte slot.
			// It remains at bytes 8..12 in both external-pointer layouts.
			// as uint32; assignment to a uint256 stack var places it right-aligned
			// (low 32 bits), matching EVM's convention so subsequent shifts work.
			// SolInlineAssembly registers `fp.selector` (full dotted name) in m_locals
			// with the underlying fn-ptr type; use that entry to identify
			// fn-ptrs, then reference the unsuffixed base local declared in outer scope.
			auto fullIt = m_locals.find(name);
			if (fullIt != m_locals.end())
			{
				auto const* bwt = dynamic_cast<awst::BytesWType const*>(fullIt->second);
				if (bwt && bwt->length().has_value()
					&& *bwt->length() == externalFunctionPointerWidth(
						m_typeMapper.profile()))
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
			// fn-ptr.address: leading 8-byte appId portion.
			// EVM returns 20-byte address; on AVM the application id is uint64.
			auto fullIt = m_locals.find(name);
			if (fullIt != m_locals.end())
			{
				auto const* bwt = dynamic_cast<awst::BytesWType const*>(fullIt->second);
				if (bwt && bwt->length().has_value()
					&& *bwt->length() == externalFunctionPointerWidth(
						m_typeMapper.profile()))
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

	// Bare STATIC calldata pointer (struct / fixed array): its Yul value is the
	// byte offset of its data in __cd_blob — the mutable __cd_off_<name> local
	// (seeded from the constant head position, reassignable via `s := V`).
	if (m_useSyntheticCalldata && m_calldataStaticPtrNames.count(name))
		return awst::makeVarExpression("__cd_off_" + name, awst::WType::biguintType(), loc);

	// Blob-backed memory aggregate: a bare reference is its Yul memory pointer
	// (the uint64 base offset into the multi-slot blob), NOT the aggregate value.
	auto boIt = m_blobOffsetVars.find(name);
	if (boIt != m_blobOffsetVars.end())
		return awst::makeVarExpression(boIt->second, awst::WType::uint64Type(), loc);

	// Signed intN (N<=64) Solidity local: reads hit its biguint shadow — the full
	// 256-bit Yul word, seeded sign-extended at block entry (buildBlock prologue).
	if (auto shIt = m_signedShadow.find(name); shIt != m_signedShadow.end())
		return awst::makeVarExpression(shIt->second, awst::WType::biguintType(), loc);

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

	// sload(v.slot) on a scalar app-global state var → read v's own storage,
	// matching the sstore routing (tryHandleStateVarSstore), not __dyn_storage.
	if (funcName == "sload" && _call.arguments.size() == 1)
		if (auto routed = tryHandleStateVarSload(_call, loc))
			return routed;

	// Translate arguments RIGHT-TO-LEFT: Yul evaluates call arguments in
	// right-to-left order, so side effects of inlined user-function args
	// (whose bodies land in m_pendingStatements during the build) must be
	// sequenced right-first — `sub(bump(1), bump(10))` runs bump(10) first.
	std::vector<std::shared_ptr<awst::Expression>> args(_call.arguments.size());
	for (size_t ai = _call.arguments.size(); ai-- > 0; )
		args[ai] = buildExpression(_call.arguments[ai]);

	// Memory writers the content-constant tracker can't model precisely: drop
	// all "mem_0x*" entries. Classified by solc's own per-instruction effect
	// table (SemanticInformation::memory == Write) instead of a hand-list
	// that drifts as builtins gain handlers. mstore itself tracks/invalidates
	// per-offset; Yul-object builtins with no EVM opcode (datacopy) keep a
	// one-entry supplement. (After arg translation, so entries recorded by
	// inlined arg builds die too.)
	{
		bool clobbersMemory = false;
		if (funcName == "datacopy")
			clobbersMemory = true;
		else if (funcName != "mstore")
		{
			std::string upper = funcName;
			std::transform(upper.begin(), upper.end(), upper.begin(),
				[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			auto it = solidity::evmasm::c_instructions.find(upper);
			if (it != solidity::evmasm::c_instructions.end())
				clobbersMemory =
					solidity::evmasm::SemanticInformation::memory(it->second)
						== solidity::evmasm::SemanticInformation::Write;
		}
		if (clobbersMemory)
			invalidateMemConstants();
	}

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
		{"exp", &AssemblyBuilder::handleExp},
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
	if (funcName == "extcodesize" && args.size() == 1)
	{
		// extcodesize(addr) → the target app's approval-program length, which
		// is exactly the lowering `address(addr).code.length` already uses
		// (SolAddressProperty): resolve the app id from the address's last 8
		// bytes — this compiler's contract-value convention — and read
		// AppApprovalProgram. A real lookup, NOT the old stub that returned 1
		// ("everything is a contract") and silently made `extcodesize(a) > 0`
		// guards always true.
		//
		// This was a hard error, justified as "no way to query whether an
		// arbitrary address has code". `.code.length` disproves that, and the
		// two spellings ask the same question — so refusing this one only
		// decided the answer by how the user wrote it. The assembly spelling
		// is the one OZ's `Address.isContract` compiles to, vendored into a
		// large share of real contracts (it is what blocks BMEX's `Vesting`).
		auto addrBytes = awst::makeAsBytes(ensureBiguint(args[0], loc), loc);
		auto appId = awst::makeAsApplication(
			awst::makeWord32ToUInt64(std::move(addrBytes), loc), loc);
		auto* tupleType = m_typeMapper.createType<awst::WTuple>(
			std::vector<awst::WType const*>{
				awst::WType::bytesType(), awst::WType::boolType()});
		auto appParamsGet = awst::makeAppParamsGet(
			"AppApprovalProgram", std::move(appId), tupleType, loc);

		// Stash the (bytes, bool) pair in a temp before reading element 0:
		// puya miscompiles TupleItemExpression pop ordering over a raw
		// IntrinsicCall. Same dance as SolAddressProperty, counter-named so two
		// extcodesize reads in one expression don't share a temp.
		std::string tmpName = "__asm_extcodesize_" + std::to_string(
			awst::NameGen::next("AssemblyBuilder.s_extcodesizeTmpCounter") + 1);
		auto tmpTarget = awst::makeVarExpression(tmpName, tupleType, loc);
		m_pendingStatements.push_back(
			awst::makeAssignmentStatement(tmpTarget, std::move(appParamsGet), loc));

		Logger::instance().warning(
			"`extcodesize(addr)` resolves the app id from the address's last 8 "
			"bytes (this compiler's contract-value convention) and returns that "
			"application's approval-program length, matching "
			"`address(addr).code.length`. An address NOT in that form reads as "
			"size 0.", loc);

		// MUST branch on the exists flag. For a non-existent app the AVM pushes
		// a uint64 zero as the value REGARDLESS of the field's type, so `len`
		// on it fails at runtime with "wanted []byte but got uint64" — which is
		// every EOA, i.e. exactly the case an isContract guard asks about.
		auto exists = awst::makeTupleItem(
			awst::makeVarExpression(tmpName, tupleType, loc), 1,
			awst::WType::boolType(), loc);
		auto program = awst::makeTupleItem(
			awst::makeVarExpression(tmpName, tupleType, loc), 0,
			awst::WType::bytesType(), loc);
		return awst::makeConditional(
			std::move(exists), awst::makeLen(std::move(program), loc),
			awst::makeZero(loc, awst::WType::uint64Type()),
			awst::WType::uint64Type(), loc);
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
		EvmFeaturePolicy::report(
			EvmFeature::TxOrigin, m_typeMapper.profile(), loc);
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
		EvmFeaturePolicy::report(
			EvmFeature::BlockHash, m_typeMapper.profile(), loc);
		// Stub so AWST building completes; the error aborts the build first.
		return awst::makeZero(loc, awst::WType::biguintType());
	}
	if (funcName == "blobhash")
	{
		EvmFeaturePolicy::report(
			EvmFeature::BlobHash, m_typeMapper.profile(), loc);
		return awst::makeZero(loc, awst::WType::biguintType());
	}
	if (funcName == "difficulty")
	{
		EvmFeaturePolicy::report(
			EvmFeature::BlockDifficulty, m_typeMapper.profile(), loc);
		return awst::makeZero(loc, awst::WType::biguintType());
	}
	if (funcName == "prevrandao")
	{
		EvmFeaturePolicy::report(
			EvmFeature::BlockPrevrandao, m_typeMapper.profile(), loc);
		// Round - 2, clamped: uint64 Sub panics on underflow and the first
		// rounds of a fresh chain (create at round 1) would hard-panic.
		auto round = awst::makeGlobal(
			std::string("Round"), awst::WType::uint64Type(), loc);
		auto isEarly = awst::makeNumericCompare(
			awst::makeGlobal(std::string("Round"), awst::WType::uint64Type(), loc),
			awst::NumericComparison::Lt,
			awst::makeIntegerConstant("2", loc), loc);
		auto prevRound = awst::makeConditional(
			std::move(isEarly),
			awst::makeZero(loc),
			awst::makeUInt64BinOp(
				std::move(round), awst::UInt64BinaryOperator::Sub,
				awst::makeIntegerConstant("2", loc), loc),
			awst::WType::uint64Type(), loc);
		return awst::makeAsBiguint(awst::makeBlock(
			"BlkSeed", std::move(prevRound), awst::WType::bytesType(), loc), loc);
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
	if (funcName == "coinbase")
	{
		EvmFeaturePolicy::report(
			EvmFeature::BlockCoinbase, m_typeMapper.profile(), loc);
		if (!m_typeMapper.profile().evmCoinbase)
			return awst::makeZero(loc, awst::WType::biguintType());
		auto const& hex = *m_typeMapper.profile().evmCoinbase;
		// Mirrors SolIntrinsicAccess's decoder exactly — case-insensitive, so
		// a future profile producer that skips CliOptions' lowercasing cannot
		// make the asm and Solidity paths emit different addresses.
		auto nibble = [](char c) -> uint8_t {
			if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
			return static_cast<uint8_t>(std::tolower(
				static_cast<unsigned char>(c)) - 'a' + 10);
		};
		std::vector<uint8_t> bytes(20);
		for (size_t i = 0; i < bytes.size(); ++i)
			bytes[i] = static_cast<uint8_t>(
				(nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
		return awst::makeAsBiguint(
			awst::makeBytesConstant(std::move(bytes), loc), loc);
	}
	if (funcName == "gasprice")
	{
		EvmFeaturePolicy::report(
			EvmFeature::TxGasPrice, m_typeMapper.profile(), loc);
		return awst::makeTxn("Fee", awst::WType::uint64Type(), loc);
	}
	if (funcName == "basefee" || funcName == "blobbasefee")
	{
		EvmFeaturePolicy::report(
			funcName == "basefee" ? EvmFeature::BlockBaseFee
				: EvmFeature::BlockBlobBaseFee,
			m_typeMapper.profile(), loc);
		return awst::makeZero(loc, awst::WType::biguintType());
	}
	if (funcName == "chainid")
	{
		EvmFeaturePolicy::report(
			EvmFeature::BlockChainId, m_typeMapper.profile(), loc);
		if (m_typeMapper.profile().evmChainId)
			return awst::makeIntegerConstant(
				*m_typeMapper.profile().evmChainId, loc,
				awst::WType::biguintType());
		return awst::makeAsBiguint(awst::makeGlobal(
			"GenesisHash", awst::WType::bytesType(), loc), loc);
	}
	if (funcName == "gaslimit")
	{
		EvmFeaturePolicy::report(
			EvmFeature::BlockGasLimit, m_typeMapper.profile(), loc);
		if (m_typeMapper.profile().evmBlockGasLimit)
			return awst::makeIntegerConstant(
				*m_typeMapper.profile().evmBlockGasLimit, loc,
				awst::WType::biguintType());
		return awst::makeGlobal(
			"OpcodeBudget", awst::WType::uint64Type(), loc);
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
		// call/staticcall in let/assign/pop/statement form is pattern-matched
		// by those translators. In pure expression context (e.g. the Solady
		// `if iszero(call(...)) { revert }` ETH-transfer idiom) no such match
		// exists — folding to constant success would silently drop the call,
		// so this is the invented-success class the policy hard-errors on.
		EvmFeaturePolicy::report(
			EvmFeature::UnknownLowLevelCall, m_typeMapper.profile(), loc);
		return awst::makeOne(loc, awst::WType::biguintType());
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
			// EVM calldatacopy ZERO-PADS past calldatasize (same convention as
			// the calldataload fix above): extract3(blob ++ bzero(sz),
			// min(off,len), sz) — real bytes then appended zeros.
			auto blob = awst::makeEvalOnce(
				awst::makeVarExpression(CD_BLOB_VAR, awst::WType::bytesType(), loc), loc);
			auto srcOff = awst::makeEvalOnce(offsetToUint64(args[1], loc), loc);
			auto sz = awst::makeEvalOnce(offsetToUint64(args[2], loc), loc);
			auto len = awst::makeLen(blob, loc);
			auto safeOff = awst::makeConditional(
				awst::makeNumericCompare(srcOff, awst::NumericComparison::Lt, len, loc),
				srcOff, awst::makeLen(blob, loc), awst::WType::uint64Type(), loc);
			auto padded = awst::makeConcat(blob, awst::makeBzero(sz, loc), loc);
			auto extractCall = awst::makeExtract3(
				std::move(padded), std::move(safeOff), sz, loc);
			// Write via the slot-routed length-driven primitive (M7): destOff
			// ≥ SLOT_SIZE lands in the right slot instead of clobbering slot 0.
			// Expression-context: route through m_pendingStatements (drained
			// at the outer statement boundary).
			writeMemWordDyn(args[0], std::move(extractCall), loc, m_pendingStatements);
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
