/// @file StatementOps.cpp
/// Yul statement translation: variable declarations, assignments, expression statements, function definitions.

#include "builder/yul/AssemblyBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/lowering/proxies/Erc1967Lowering.h"
#include "builder/types/FunctionPointerKind.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <libsolutil/Numeric.h>

#include <sstream>
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>

namespace puyasol::builder
{

void AssemblyBuilder::buildStatement(
	solidity::yul::Statement const& _stmt,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (m_frame.yulSubroutine && m_frame.haltEmitted)
		return;
	std::visit(
		[this, &_out](auto const& _node) {
			using T = std::decay_t<decltype(_node)>;
			if constexpr (std::is_same_v<T, solidity::yul::VariableDeclaration>)
				buildVariableDeclaration(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Assignment>)
				buildAssignment(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::ExpressionStatement>)
				buildExpressionStatement(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::FunctionDefinition>)
				buildFunctionDefinition(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Block>)
			{
				for (auto const& innerStmt: _node.statements)
					buildStatement(innerStmt, _out);
			}
			else if constexpr (std::is_same_v<T, solidity::yul::If>)
				buildIfStatement(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::ForLoop>)
				buildForLoop(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Break>)
				buildBreakStatement(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Continue>)
				buildContinueStatement(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Leave>)
				buildLeaveStatement(_node, _out);
			else if constexpr (std::is_same_v<T, solidity::yul::Switch>)
				buildSwitchStatement(_node, _out);
		},
		_stmt
	);
}

void AssemblyBuilder::buildVariableDeclaration(
	solidity::yul::VariableDeclaration const& _decl,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_decl.debugData);

	// Check for special function call patterns: staticcall, user-defined functions
	if (_decl.value)
	{
		if (auto const* call = std::get_if<solidity::yul::FunctionCall>(_decl.value.get()))
		{
			std::string callName = getFunctionName(call->functionName);


			// User-defined Yul function: single (let x := f()) or multi (let a,b := f()) return.
			if (m_context->asmFunctions.count(callName))
			{
				auto const& funcDef = *m_context->asmFunctions.at(callName);

				for (auto const& var: _decl.variables)
				{
					std::string n = var.name.str();
					if (auto rit = m_frame.yulInlineRenames.find(n); rit != m_frame.yulInlineRenames.end())
						n = rit->second;
					m_frame.locals[n] = awst::WType::biguintType();
				}

				handleUserFunctionCall(*call, loc, _out);

				// Both call paths publish per-call return temps.
				bool fromSub = !m_frame.yulSubReturnTemps.empty();
				size_t numReturns = std::min(
					_decl.variables.size(), funcDef.returnVariables.size()
				);
				for (size_t i = 0; i < numReturns; ++i)
				{
					std::string retName = fromSub
						? m_frame.yulSubReturnTemps[i]
						: funcDef.returnVariables[i].name.str();
					std::string varName = _decl.variables[i].name.str();
					// Inline frames: declare under the frame's unique name.
					if (auto rit = m_frame.yulInlineRenames.find(varName); rit != m_frame.yulInlineRenames.end())
						varName = rit->second;

					auto retVar = awst::makeVarExpression(retName, awst::WType::biguintType(), loc);

					auto target = awst::makeVarExpression(varName, awst::WType::biguintType(), loc);

					auto assign = awst::makeAssignmentStatement(std::move(target), std::move(retVar), loc);
					_out.push_back(std::move(assign));
				}
				return;
			}
		}
	}

	for (auto const& var: _decl.variables)
	{
		std::string origName = var.name.str();
		// Inline-expanded bodies: declare under the frame's UNIQUE name so
		// sibling/nested calls reusing the same bare local don't share one
		// runtime var (reads already resolve through the same map).
		std::string name = origName;
		if (auto rit = m_frame.yulInlineRenames.find(origName); rit != m_frame.yulInlineRenames.end())
			name = rit->second;
		m_frame.locals[name] = awst::WType::biguintType();

		// Record the initializer constant only for SINGLE-ASSIGNMENT locals —
		// the fold is flow-insensitive, so a later `name := …` (loop counter,
		// pointer bump) would leave this entry stale. Erase on the non-constant
		// path: a shadowing `let` in a sibling scope must not inherit a stale
		// entry from an earlier same-named declaration. (The reassignment scan
		// keys on ORIGINAL names — check origName, record under name.)
		if (m_context->reassignedLocals.count(origName))
			m_frame.localConstants.erase(name);
		else if (_decl.value)
		{
			auto constVal = resolveConstantYulValue(*_decl.value);
			if (constVal)
				m_frame.localConstants[name] = *constVal;
			else
				m_frame.localConstants.erase(name);
		}
		else
		{
			m_frame.localConstants[name] = 0;
		}
		// Preserve the complete solc-folded word, not only literal SSA facts
		// or the subset that happens to fit a target offset.
		m_frame.localWideConstants.erase(name);
		if (!m_context->reassignedLocals.count(origName) && _decl.value)
			if (auto word = resolveConstantYulWord(*_decl.value))
				m_frame.localWideConstants[name] = *word;
		m_frame.localSlotConstants.erase(name); // same shadowing hygiene

		auto target = awst::makeVarExpression(name, awst::WType::biguintType(), makeLoc(var.debugData));

		std::shared_ptr<awst::Expression> value;
		if (_decl.value)
		{
			value = buildExpression(*_decl.value);
			drainPendingStatements(_out);
			if (!value)
				value = awst::makeZero(loc, awst::WType::biguintType());
		}
		else
		{
			value = awst::makeZero(loc, awst::WType::biguintType());
		}

		// 32-alignment of a single-assignment local, for the memory-slot straddle
		// proof: `let p := mul(i, 32)` or `add(base, 0x40)` carries forward so
		// every mload/mstore through p can drop its second-slot arm.
		bool aligned = value && alignmentMod32(*value).value_or(1u) == 0u;
		// `let pMem := mload(0x40)`: the AWST read is opaque, but the pointer
		// itself is 32-aligned whenever the block preserves that invariant.
		if (!aligned && m_context->fmpStaysAligned && _decl.value)
			if (auto const* c = std::get_if<solidity::yul::FunctionCall>(_decl.value.get()))
				aligned = getFunctionName(c->functionName) == "mload"
					&& c->arguments.size() == 1
					&& yulAlignmentMod32(*_decl.value, {}).value_or(1u) == 0u;
		if (!m_context->reassignedLocals.count(origName) && aligned)
			m_frame.alignedLocals.insert(name);
		else
			m_frame.alignedLocals.erase(name);

		// EIP-1967 slot bound to a single-assignment local: record + fold at
		// every bare reference (classify() then fires at the sload/sstore
		// site) and emit NO store — all references fold, and a magic constant
		// surviving in the AWST is reserved as the "escaped to runtime"
		// warning signal (Erc1967Lowering::warnEscapedSlotConstants).
		if (m_typeMapper.profile().proxyAdaptation && !m_context->reassignedLocals.count(origName))
			if (auto const* slotConst =
					dynamic_cast<awst::IntegerConstant const*>(value.get());
				slotConst && proxies::Erc1967Lowering::classify(slotConst)
					!= proxies::Erc1967Slot::None)
			{
				m_frame.localSlotConstants[name] = slotConst->value;
				continue;
			}

		// Yul values are always 256-bit.
		value = ensureBiguint(std::move(value), loc);

		auto assign = awst::makeAssignmentStatement(std::move(target), std::move(value), loc);
		_out.push_back(std::move(assign));
	}
}

void AssemblyBuilder::buildAssignment(
	solidity::yul::Assignment const& _assign,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_assign.debugData);

	if (_assign.variableNames.size() > 1) // multi-var: a, b, c := f(...)
	{
		if (_assign.value)
		{
			if (auto const* call = std::get_if<solidity::yul::FunctionCall>(_assign.value.get()))
			{
				std::string callName = getFunctionName(call->functionName);
				if (m_context->asmFunctions.count(callName))
				{
					auto const& funcDef = *m_context->asmFunctions.at(callName);

					handleUserFunctionCall(*call, loc, _out);

					bool fromSub = !m_frame.yulSubReturnTemps.empty();
					size_t numReturns = std::min(
						_assign.variableNames.size(), funcDef.returnVariables.size()
					);
					for (size_t i = 0; i < numReturns; ++i)
					{
						std::string retName = fromSub
							? m_frame.yulSubReturnTemps[i]
							: funcDef.returnVariables[i].name.str();
						std::string varName = resolveVarRef(_assign.variableNames[i]);
						if (!m_frame.calldataParamNames.count(varName)
							&& !m_frame.calldataStaticPtrNames.count(varName))
							m_frame.localConstants.erase(varName);
						m_frame.localSlotConstants.erase(varName);

						auto retIt = m_frame.locals.find(retName);
						auto const* retType = (retIt != m_frame.locals.end())
							? retIt->second : awst::WType::biguintType();
						emitPlainYulAssignment(
							varName,
							awst::makeVarExpression(retName, retType, loc),
							loc, _out);
					}
					return;
				}
			}
		}

		Logger::instance().error(
			"multi-variable assignment not yet supported in assembly translation", loc
		);
		return;
	}

	std::string name = resolveVarRef(_assign.variableNames[0]);

	// Reassigned → any recorded constant is stale. Calldata param/pointer names
	// are exempt: their entries are HEAD OFFSETS owned by the calldata machinery
	// (repoints go through the mutable __cd_off_/__cd_len_ locals instead).
	if (!m_frame.calldataParamNames.count(name) && !m_frame.calldataStaticPtrNames.count(name))
		m_frame.localConstants.erase(name);
	m_frame.localSlotConstants.erase(name);

	// Bare STATIC calldata pointer write (`s := s2`, `s2 := 4`): repoint —
	// assign the mutable __cd_off_<name> local; later reads (asm or Solidity
	// member access through the live pointer) follow the new offset.
	if (m_frame.useSyntheticCalldata && m_frame.calldataStaticPtrNames.count(name) && _assign.value)
	{
		auto rhs = buildExpression(*_assign.value);
		drainPendingStatements(_out);
		if (!rhs)
			return;
		if (m_frame.seededCalldataPointers)
			m_frame.seededCalldataPointers->insert(name);
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression("__cd_off_" + name, awst::WType::biguintType(), loc),
			std::move(rhs), loc));
		return;
	}

	// fn-ptr writes: fp.selector := expr / fp.address := expr
	// → replace3 the public selector/address slice of the profile-selected layout.
	{
		auto dotIdx = name.rfind('.');
		if (dotIdx != std::string::npos && _assign.value)
		{
			std::string suffix = name.substr(dotIdx + 1);
			std::string baseName = name.substr(0, dotIdx);
			// Dynamic calldata param: `x.offset := V` / `x.length := L` repoints x within __cd_blob —
			// write the mutable pointer local so later reads / value-extracts see the new range.
			if ((suffix == "offset" || suffix == "length") && _assign.value)
			{
				auto typeIt = m_frame.locals.find(baseName);
				bool isCdPtr = (typeIt != m_frame.locals.end() && isDynamicCalldataType(typeIt->second))
					|| m_frame.calldataPointerNames.count(baseName);
				if (m_frame.useSyntheticCalldata && isCdPtr)
				{
					auto rhs = buildExpression(*_assign.value);
					drainPendingStatements(_out);
					if (!rhs)
						return;
					std::string local = (suffix == "offset" ? "__cd_off_" : "__cd_len_") + baseName;
					// Mark the pointer locals LIVE: later blocks must not re-seed over
					// this write, and value reads of the param (return x) now go
					// through extract3(__cd_blob, off, len).
					if (m_frame.seededCalldataPointers)
						m_frame.seededCalldataPointers->insert(baseName);
					_out.push_back(awst::makeAssignmentStatement(
						awst::makeVarExpression(local, awst::WType::biguintType(), loc),
						std::move(rhs), loc));
					return;
				}
			}
			if (suffix == "selector" || suffix == "address")
			{
				auto fullIt = m_frame.locals.find(name);
				if (fullIt != m_frame.locals.end())
				{
					auto const* bwt = dynamic_cast<awst::BytesWType const*>(fullIt->second);
					if (bwt && bwt->length().has_value()
						&& *bwt->length() == externalFunctionPointerWidth(
							m_typeMapper.profile()))
					{
						auto rhs = buildExpression(*_assign.value);
						drainPendingStatements(_out);
						if (!rhs)
							return;

						int sliceWidth = (suffix == "selector") ? 4 : 8;
						int sliceOffset = (suffix == "selector") ? 8 : 0;
						if (suffix == "selector"
							&& m_typeMapper.profile().evmSelectors)
							Logger::instance().warning(
								"inline assembly assignment to an external function "
								"pointer selector updates its Solidity selector but "
								"retains the original ARC-4 routing selector; an "
								"arbitrary selector cannot be mapped without the "
								"callee ABI", loc);

						// Slice to exactly sliceWidth bytes:
						// - bytes/account: take low sliceWidth bytes (EVM right-aligns addresses).
						// - numeric: itob(8) then extract low sliceWidth.
						std::shared_ptr<awst::Expression> sliceBytes;
						bool rhsIsBytesLike =
							rhs->wtype == awst::WType::accountType()
							|| (rhs->wtype && rhs->wtype->kind() == awst::WTypeKind::Bytes);

						if (rhsIsBytesLike)
						{
							std::shared_ptr<awst::Expression> rhsBytes =
								(rhs->wtype == awst::WType::bytesType())
								? rhs
								: awst::makeAsBytes(rhs, loc);
							auto rhsBytesForLen = rhsBytes;

							auto lenCall = awst::makeIntrinsicCall(
								"len", awst::WType::uint64Type(), loc);
							lenCall->stackArgs.push_back(std::move(rhsBytesForLen));
							auto offsetExpr = awst::makeUInt64BinOp(
								std::move(lenCall), awst::UInt64BinaryOperator::Sub,
								awst::makeIntegerConstant(sliceWidth, loc), loc);

							auto extractCall = awst::makeIntrinsicCall(
								"extract3", awst::WType::bytesType(), loc);
							extractCall->stackArgs.push_back(std::move(rhsBytes));
							extractCall->stackArgs.push_back(std::move(offsetExpr));
							extractCall->stackArgs.push_back(awst::makeIntegerConstant(
								std::to_string(sliceWidth), loc));
							sliceBytes = std::move(extractCall);
						}
						else
						{
							if (rhs->wtype == awst::WType::biguintType())
								rhs = safeBtoi(std::move(rhs), loc);

							auto itobCall = awst::makeIntrinsicCall(
								"itob", awst::WType::bytesType(), loc);
							itobCall->stackArgs.push_back(std::move(rhs));

							if (sliceWidth == 8)
							{
								sliceBytes = std::move(itobCall);
							}
							else
							{
								auto extractCall = awst::makeIntrinsicCall(
									"extract3", awst::WType::bytesType(), loc);
								extractCall->stackArgs.push_back(std::move(itobCall));
								extractCall->stackArgs.push_back(awst::makeIntegerConstant(
									std::to_string(8 - sliceWidth), loc));
								extractCall->stackArgs.push_back(awst::makeIntegerConstant(
									std::to_string(sliceWidth), loc));
								sliceBytes = std::move(extractCall);
							}
						}

						auto baseVar = awst::makeVarExpression(baseName, fullIt->second, loc);
						auto baseAsBytes = awst::makeAsBytes(std::move(baseVar), loc);

						auto replaceCall = awst::makeIntrinsicCall(
							"replace3", awst::WType::bytesType(), loc);
						replaceCall->stackArgs.push_back(std::move(baseAsBytes));
						replaceCall->stackArgs.push_back(awst::makeIntegerConstant(
							std::to_string(sliceOffset), loc));
						replaceCall->stackArgs.push_back(std::move(sliceBytes));

						auto castBack = awst::makeReinterpretCast(
							std::move(replaceCall), fullIt->second, loc);

						auto target = awst::makeVarExpression(baseName, fullIt->second, loc);
						auto assign = awst::makeAssignmentStatement(
							std::move(target), std::move(castBack), loc);
						_out.push_back(std::move(assign));
						return;
					}
				}
			}
		}
	}

	if (name.ends_with(".slot"))
	{
		if (_assign.value)
		{
			auto value = buildExpression(*_assign.value);
			drainPendingStatements(_out);
			emitPlainYulAssignment(std::move(name), std::move(value), loc, _out);
		}
		return;
	}

	if (_assign.value)
	{
		if (auto const* call = std::get_if<solidity::yul::FunctionCall>(_assign.value.get()))
		{
			std::string callName = getFunctionName(call->functionName);
			if (callName == "staticcall" || callName == "call")
			{
				handlePrecompileCall(*call, name, loc, _out, /*_isCall=*/callName == "call");
				return;
			}
		}
	}

	auto value = buildExpression(*_assign.value);
	drainPendingStatements(_out);
	emitPlainYulAssignment(std::move(name), std::move(value), loc, _out);
}

/// One plain-name Yul write (`x := V`), applying every representation
/// redirect a Solidity-backed name may carry — signed shadow, blob-backed
/// pointer, static calldata pointer, storage slot — plus target-typed coercion. Shared by
/// the single-var fallback and the multi-var return loop so the redirects
/// cannot diverge between them again (the multi-var arm previously wrote raw
/// biguint locals, silently bypassing all three).
void AssemblyBuilder::emitPlainYulAssignment(
	std::string name,
	std::shared_ptr<awst::Expression> value,
	awst::SourceLocation const& loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// `.slot` writes and reads must use the same declaration binding, including
	// shadowed locals and multi-return Yul assignments.
	if (name.ends_with(".slot"))
	{
		if (!value) return;
		auto binding = m_context->structRefSlotLocals.find(name);
		name = binding != m_context->structRefSlotLocals.end()
			? binding->second : name.substr(0, name.size() - 5);
		// Default-layout identity aliases may carry a native storage sentinel
		// (e.g. string storage), not an arithmetic operand for ensureBiguint.
		if (value->wtype == awst::WType::uint64Type())
			value = awst::makeItob(std::move(value), loc);
		if (value->wtype != awst::WType::biguintType())
			value = awst::makeAsBiguint(std::move(value), loc);
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(name, awst::WType::biguintType(), loc),
			std::move(value), loc));
		return;
	}

	// Bare STATIC calldata pointer: repoint through its mutable offset local.
	if (m_frame.useSyntheticCalldata && m_frame.calldataStaticPtrNames.count(name))
	{
		if (!value)
			return;
		if (m_frame.seededCalldataPointers)
			m_frame.seededCalldataPointers->insert(name);
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression("__cd_off_" + name, awst::WType::biguintType(), loc),
			ensureBiguint(std::move(value), loc), loc));
		return;
	}

	// Signed intN (N<=64) local: writes land on its biguint shadow (the raw Yul
	// word — see the buildBlock prologue); the typed local refreshes at block exit.
	if (auto shIt = m_frame.signedShadow.find(name); shIt != m_frame.signedShadow.end())
		name = shIt->second;

	// Blob-backed memory aggregate: `ret := ptr` REPOINTS the aggregate. READS
	// of the bare name already resolve to its offset var (CoreTranslation), so
	// the WRITE must land there too — otherwise the repoint is invisible and
	// value-use/return materialization reads the stale original allocation
	// (TypedMemView.clone returned its pre-copy 0-length bytes this way).
	if (auto boIt = m_frame.blobOffsetVars.find(name); boIt != m_frame.blobOffsetVars.end())
	{
		if (!value)
			value = awst::makeZero(loc, awst::WType::uint64Type());
		_out.push_back(awst::makeAssignmentStatement(
			awst::makeVarExpression(boIt->second, awst::WType::uint64Type(), loc),
			offsetToUint64(std::move(value), loc), loc));
		return;
	}

	auto it = m_frame.locals.find(name);
	auto const* wtype = (it != m_frame.locals.end()) ? it->second : awst::WType::biguintType();
	auto target = awst::makeVarExpression(name, wtype, loc);

	if (!value)
		value = awst::makeZero(loc, target->wtype);

	if (target->wtype != value->wtype)
	{
		if (target->wtype == awst::WType::biguintType())
		{
			value = ensureBiguint(std::move(value), loc);
		}
		else if (target->wtype == awst::WType::boolType())
		{
			value = ensureBool(std::move(value), loc);
		}
		else if (target->wtype->kind() == awst::WTypeKind::Bytes)
		{
			auto const* bytesType = dynamic_cast<awst::BytesWType const*>(target->wtype);
			// bytes[N]: pad biguint to 32 bytes then extract first N (EVM: left-aligned).
			if (bytesType && bytesType->length() && *bytesType->length() > 0)
			{
				int n = *bytesType->length();
				auto biguintVal = ensureBiguint(std::move(value), loc);
				// padTo32Bytes: ensures exactly 32 bytes big-endian
				auto padded = padTo32Bytes(std::move(biguintVal), loc);
				// Extract first N bytes (EVM left-aligned)
				auto zero = awst::makeZero(loc);
				auto lenConst = awst::makeIntegerConstant(n, loc);
				auto extract = awst::makeExtract3(std::move(padded), std::move(zero), std::move(lenConst), loc);
				auto cast = awst::makeReinterpretCast(std::move(extract), target->wtype, loc);
				value = std::move(cast);
			}
			else
			{
				auto biguintVal = ensureBiguint(std::move(value), loc);
				auto cast = awst::makeReinterpretCast(std::move(biguintVal), target->wtype, loc);
				value = std::move(cast);
			}
		}
		else if (target->wtype == awst::WType::accountType())
		{
			auto biguintVal = ensureBiguint(std::move(value), loc);
			auto padded = padTo32Bytes(std::move(biguintVal), loc);
			auto cast = awst::makeAsAccount(std::move(padded), loc);
			value = std::move(cast);
		}
		else if (target->wtype == awst::WType::uint64Type())
		{
			// Truncate biguint→uint64 via safeBtoi (keeps phi-node types consistent).
			if (value->wtype == awst::WType::biguintType())
			{
				// Sub-64-bit Solidity type (uint8/16/32): mask before btoi.
				auto bwIt = m_context->paramBitWidths.find(name);
				if (bwIt != m_context->paramBitWidths.end() && bwIt->second < 64)
				{
					solidity::u256 mask = (solidity::u256(1) << bwIt->second) - 1;
					std::ostringstream maskStr;
					maskStr << mask;

					auto maskConst = awst::makeIntegerConstant(maskStr.str(), loc, awst::WType::biguintType());

					auto andOp = awst::makeBigUIntBinOp(std::move(value), awst::BigUIntBinaryOperator::BitAnd, std::move(maskConst), loc);
					value = std::move(andOp);
				}
				value = safeBtoi(std::move(value), loc);
			}
		}
		else if (target->wtype == awst::WType::accountType())
		{
			if (value->wtype == awst::WType::biguintType())
			{
				value = awst::makeAsAccount(awst::makeAsBytes(std::move(value), loc), loc);
			}
			else if (value->wtype != awst::WType::accountType())
			{
				auto cast = awst::makeAsAccount(std::move(value), loc);
				value = std::move(cast);
			}
		}
		else
		{
			Logger::instance().debug(
				"assembly type coercion: " + value->wtype->name() + " → " + target->wtype->name()
			);
			// Don't mutate IntegerConstant wtype to struct/array — puya rejects it.
			// Wrap with ReinterpretCast instead.
			if (dynamic_cast<awst::IntegerConstant const*>(value.get()))
				value = awst::makeReinterpretCast(std::move(value), target->wtype, loc);
			else
				value->wtype = target->wtype;
		}
	}

	auto assign = awst::makeAssignmentStatement(std::move(target), std::move(value), loc);
	_out.push_back(std::move(assign));
}

void AssemblyBuilder::buildExpressionStatement(
	solidity::yul::ExpressionStatement const& _stmt,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto loc = makeLoc(_stmt.debugData);

	if (auto const* call = std::get_if<solidity::yul::FunctionCall>(&_stmt.expression))
	{
		std::string funcName = getFunctionName(call->functionName);
		if (m_context->asmFunctions.count(funcName))
		{
			handleUserFunctionCall(*call, loc, _out);
			return;
		}

		// Statement-position memory writers must drop the "mem_0x*" content
		// constants exactly like the expression path does (buildFunctionCall)
		// — every builtin dispatched below (mstore8, mcopy, calldatacopy,
		// returndatacopy, codecopy, call/staticcall output copies) reached its
		// handler WITHOUT invalidating, so a later keccak256/mload folded over
		// a STALE word: `mstore(0,1) mstore8(0,0xff) sstore(keccak256(0,0x20), v)`
		// hashed the pre-mstore8 word and wrote the wrong storage slot,
		// silently. `pop(call(...))` is classified on the inner call, whose
		// output copy is what writes memory. mstore is excluded (it tracks
		// per-offset itself); over-invalidating only costs folding, never
		// correctness.
		{
			std::string effective = funcName;
			if (funcName == "pop" && call->arguments.size() == 1)
				if (auto const* inner =
						std::get_if<solidity::yul::FunctionCall>(&call->arguments[0]))
					effective = getFunctionName(inner->functionName);
			if (builtinClobbersMemory(effective))
				invalidateMemConstants();
		}

		// Pattern checks that need the raw Yul AST must run before arg translation.
		if (funcName == "mstore" && tryHandleBytesMemoryWrite(*call, loc, _out))
			return;
		if (funcName == "mstore8" && tryHandleBytesMemoryWrite8(*call, loc, _out))
			return;
		if (funcName == "mcopy" && tryHandleBytesMemoryMcopy(*call, loc, _out))
			return;

		// pop(call(...)) / pop(staticcall(...)): the discarded value is the call's
		// success flag, but the call's SIDE EFFECTS (inner txn + returndata
		// output-copy) must still happen. Route to the full app-call lowering with
		// NO assign target (its output-copy is not gated on the target). This is
		// exactly the shape UnusedPruner produces from `let unused := call(...)`,
		// and the common `pop(call(...))` idiom for calls whose success is ignored.
		if (funcName == "pop" && call->arguments.size() == 1)
			if (auto const* inner = std::get_if<solidity::yul::FunctionCall>(&call->arguments[0]))
			{
				std::string innerName = getFunctionName(inner->functionName);
				if (innerName == "call" || innerName == "staticcall")
				{
					handlePrecompileCall(*inner, "", loc, _out, /*_isCall=*/innerName == "call");
					return;
				}
			}

		if (funcName == "staticcall" || funcName == "call")
		{
			handlePrecompileCall(*call, "", loc, _out, /*_isCall=*/funcName == "call");
			return;
		}
		bool statementBuiltin = funcName == "mstore" || funcName == "mstore8"
			|| funcName == "return" || funcName == "revert" || funcName == "tstore"
			|| funcName == "sstore" || funcName == "invalid" || funcName == "stop"
			|| funcName == "returndatacopy" || funcName == "pop"
			|| funcName == "delegatecall" || funcName == "mcopy"
			|| (funcName.size() == 4 && funcName.starts_with("log"));
		if (!statementBuiltin)
		{
			auto expr = buildExpression(_stmt.expression);
			drainPendingStatements(_out);
			if (expr) _out.push_back(awst::makeExpressionStatement(std::move(expr), loc));
			return;
		}
		auto args = buildCallOperands(*call, _out);
		drainPendingStatements(_out);

		if (funcName == "mstore")
		{
			handleMstore(args, loc, _out);
			return;
		}
		if (funcName == "mstore8")
		{
			handleMstore8(args, loc, _out);
			return;
		}
		if (funcName == "return")
		{
			handleReturn(args, loc, _out);
			return;
		}
		if (funcName == "revert")
		{
			handleRevert(args, loc, _out);
			return;
		}
		if (funcName == "tstore")
		{
			handleTstore(args, loc, _out);
			return;
		}
		if (funcName == "sstore")
		{
			if (!tryHandleStateVarSstore(*call, args, loc, _out))
				handleSstore(args, loc, _out);
			return;
		}
		if (funcName.size() == 4 && funcName.compare(0, 3, "log") == 0
			&& funcName[3] >= '0' && funcName[3] <= '4')
		{
			handleLog(args, funcName[3] - '0', loc, _out);
			return;
		}
		if (funcName == "invalid")
		{
			auto stmt = awst::makeExpressionStatement(awst::makeAssert(awst::makeFalse(loc), loc, "invalid"), loc);
			_out.push_back(std::move(stmt));
			return;
		}
		if (funcName == "stop")
		{

			auto halt = awst::makeIntrinsicCall(
				"return", awst::WType::voidType(), loc);
			halt->stackArgs.push_back(awst::makeTrue(loc));
			_out.push_back(
				awst::makeExpressionStatement(std::move(halt), loc));
			m_frame.haltEmitted = true;
			return;
		}
		if (funcName == "returndatacopy")
		{
			emitReturndatacopy(args, loc, _out);
			return;
		}
		if (funcName == "pop")
			return; // discard value, no-op

		if (funcName == "delegatecall")
		{
			// No AVM equivalent; stubbing as no-op would silently drop the call.
			Logger::instance().error(
				"`delegatecall(...)` in inline assembly is not supported on AVM. "
				"It runs another contract's code in the caller's storage context, "
				"which has no AVM equivalent; stubbing it as a no-op would silently "
				"drop the call. This matches the hard error on high-level "
				"`.delegatecall(...)`.",
				loc
			);
			return;
		}
		if (funcName == "mcopy")
		{
			if (!checkArity(args, 3, "mcopy", loc)) return;
			auto data = readMemRangeDyn(args[1], args[2], loc, _out);
			writeMemRangeDyn(args[0], std::move(data), loc, _out);
			return;
		}

		auto expr = buildExpression(_stmt.expression);
		if (expr)
		{
			auto exprStmt = awst::makeExpressionStatement(std::move(expr), loc);
			_out.push_back(std::move(exprStmt));
		}
	}
	else
	{
		auto expr = buildExpression(_stmt.expression);
		if (expr)
		{
			auto exprStmt = awst::makeExpressionStatement(std::move(expr), loc);
			_out.push_back(std::move(exprStmt));
		}
	}
}

bool AssemblyBuilder::tryHandleStateVarSstore(
	solidity::yul::FunctionCall const& _call,
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	if (_call.arguments.empty() || _args.size() < 2)
		return false;
	auto const* id = std::get_if<solidity::yul::Identifier>(&_call.arguments[0]);
	if (!id)
		return false;
	auto it = m_context->stateVarSlots.find(id->name.str());
	if (it == m_context->stateVarSlots.end())
		return false;
	auto const& sv = it->second;
	auto key = awst::makeUtf8BytesConstant(sv.varName, _loc, awst::WType::stateKeyType());
	auto target = awst::makeAppStateExpression(key, sv.wtype, _loc);
	auto value = ensureBiguint(_args[1], _loc);
	auto assign = awst::makeAssignmentExpression(std::move(target), std::move(value), _loc, sv.wtype);
	_out.push_back(awst::makeExpressionStatement(std::move(assign), _loc));
	return true;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::tryHandleStateVarSload(
	solidity::yul::FunctionCall const& _call,
	awst::SourceLocation const& _loc)
{
	if (_call.arguments.empty())
		return nullptr;
	auto const* id = std::get_if<solidity::yul::Identifier>(&_call.arguments[0]);
	if (!id)
		return nullptr;
	auto it = m_context->stateVarSlots.find(id->name.str());
	if (it == m_context->stateVarSlots.end())
		return nullptr;
	auto const& sv = it->second;
	auto key = awst::makeUtf8BytesConstant(sv.varName, _loc, awst::WType::stateKeyType());
	return StorageMapper::makeStateGetWithDefault(
		awst::makeAppStateExpression(std::move(key), sv.wtype, _loc), sv.wtype, _loc);
}


} // namespace puyasol::builder
