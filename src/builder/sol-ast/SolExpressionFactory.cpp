/// @file SolExpressionFactory.cpp
/// Factory that creates the right SolExpression subclass for a Solidity AST node.
/// Uses FunctionCallKind + FunctionType::Kind for dispatch.

#include "builder/sol-ast/SolExpressionFactory.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/itxn/CallResolver.h"
#include "builder/sol-ast/calls/SolRequireAssert.h"
#include "builder/sol-ast/calls/SolRevert.h"
#include "builder/sol-ast/calls/SolBuiltinCall.h"
#include "builder/sol-ast/calls/SolTypeConversion.h"
#include "builder/sol-ast/calls/SolWrapUnwrap.h"
#include "builder/sol-ast/calls/SolStructConstruction.h"
#include "builder/sol-ast/calls/SolTransferSend.h"
#include "builder/sol-ast/calls/SolBareCall.h"
#include "builder/sol-ast/calls/SolAbiEncode.h"
#include "builder/sol-ast/calls/SolAbiDecode.h"
#include "builder/sol-ast/calls/SolArrayMethod.h"
#include "builder/sol-ast/calls/SolInternalCall.h"
#include "builder/sol-ast/calls/SolExternalCall.h"
#include "builder/sol-ast/calls/SolNewExpression.h"
#include "builder/sol-ast/calls/SolBytesConcat.h"
#include "builder/sol-ast/calls/SolMetaType.h"
#include "builder/sol-ast/members/SolIntrinsicAccess.h"
#include "builder/sol-ast/members/SolEnumValueAccess.h"
#include "builder/sol-ast/members/SolSelectorAccess.h"
#include "builder/sol-ast/members/SolMetaTypeAccess.h"
#include "builder/sol-ast/members/SolLengthAccess.h"
#include "builder/sol-ast/members/SolFieldAccess.h"
#include "builder/sol-ast/members/SolAddressProperty.h"
#include "builder/sol-ast/members/SolConstantAccess.h"

#include <libsolidity/ast/ASTAnnotations.h>

namespace puyasol::builder::sol_ast
{

/// `.address` on an external function pointer value.
/// Extracts the 8-byte appId prefix from either profile-selected fn-ptr layout
/// and left-pads it to 32 bytes.
class SolFunctionAddressAccess : public SolMemberAccess
{
public:
	SolFunctionAddressAccess(eb::ContractContext& _ctx,
		solidity::frontend::MemberAccess const& _node)
		: SolMemberAccess(_ctx, _node) {}

	std::shared_ptr<awst::Expression> toAwst() override
	{
		// `this.f.address` (or `this.f{…}.address`) → CurrentApplicationAddress.
		{
			solidity::frontend::Expression const* base = &m_memberAccess.expression();
			while (auto const* opts = dynamic_cast<
					solidity::frontend::FunctionCallOptions const*>(base))
				base = &opts->expression();
			if (auto const* innerMA = dynamic_cast<solidity::frontend::MemberAccess const*>(base))
			{
				if (auto const* baseId = dynamic_cast<solidity::frontend::Identifier const*>(
						&innerMA->expression()))
				{
					if (baseId->name() == "this")
						return awst::makeGlobal(
							"CurrentApplicationAddress", awst::WType::accountType(), m_loc);
				}
			}
		}

		auto fnPtr = m_ctx.buildExpr(m_memberAccess.expression());
		if (fnPtr->wtype != awst::WType::bytesType())
		{
			auto cast = awst::makeAsBytes(std::move(fnPtr), m_loc);
			fnPtr = std::move(cast);
		}
		// Extract first 8 bytes = appId (big-endian uint64).
		auto appIdBytes = awst::makeExtract(std::move(fnPtr), 0, 8, m_loc);
		// Left-pad to 32 bytes to form an address.
		auto cat = awst::makeLeftPad(std::move(appIdBytes), 24, m_loc);
		// Reinterpret as account for assignment to an address-typed target.
		return awst::makeAsAccount(std::move(cat), m_loc);
	}
};

class SolFunctionPointerAccess : public SolMemberAccess
{
public:
	SolFunctionPointerAccess(eb::ContractContext& _ctx,
		solidity::frontend::MemberAccess const& _node,
		solidity::frontend::FunctionDefinition const* _funcDef,
		solidity::frontend::FunctionType const* _callerFuncType = nullptr)
		: SolMemberAccess(_ctx, _node), m_funcDef(_funcDef), m_callerFuncType(_callerFuncType) {}

	std::shared_ptr<awst::Expression> toAwst() override
	{
		std::string awstName = m_scope.findSuperTarget(m_funcDef->id());

		// Prefer caller's function type: functionType(true) on external-only fns
		// may return a placeholder with empty params → useless dispatch name.
		auto const* regType = m_callerFuncType
			? m_callerFuncType
			: m_funcDef->functionType(true);
		if (!regType)
			regType = m_funcDef->functionType(false);
		eb::FunctionPointerBuilder::registerTarget(
			m_ctx,
			m_funcDef,
			regType,
			awstName);

		// `C(addr).fn`: receiver address must flow into the fn pointer so
		// `.address` returns the caller-supplied addr, not the self-sentinel (0).
		std::shared_ptr<awst::Expression> receiverAddr;
		if (m_callerFuncType
			&& m_callerFuncType->kind() == solidity::frontend::FunctionType::Kind::External)
		{
			auto const& baseExpr = m_memberAccess.expression();
			bool isSelf = false;
			if (auto const* ident = dynamic_cast<solidity::frontend::Identifier const*>(&baseExpr))
				if (ident->name() == "this")
					isSelf = true;
			if (!isSelf)
			{
				// `C(address(0x1234))` — evaluate the inner arg for the address bytes.
				if (auto const* baseCall = dynamic_cast<solidity::frontend::FunctionCall const*>(&baseExpr))
				{
					if (baseCall->annotation().kind.set()
						&& *baseCall->annotation().kind
							== solidity::frontend::FunctionCallKind::TypeConversion
						&& baseCall->arguments().size() == 1)
						receiverAddr = m_ctx.buildExpr(*baseCall->arguments()[0]);
				}
				if (!receiverAddr)
					receiverAddr = m_ctx.buildExpr(baseExpr);
			}
		}

		return eb::FunctionPointerBuilder::buildFunctionReference(
			m_ctx, m_funcDef, m_loc, m_callerFuncType, receiverAddr, awstName);
	}
private:
	solidity::frontend::FunctionDefinition const* m_funcDef;
	solidity::frontend::FunctionType const* m_callerFuncType;
};

SolExpressionFactory::SolExpressionFactory(eb::ContractContext& _ctx)
	: m_ctx(_ctx)
{
}

std::unique_ptr<SolFunctionCall> SolExpressionFactory::createFunctionCall(
	solidity::frontend::FunctionCall const& _node)
{
	using FunctionCallKind = solidity::frontend::FunctionCallKind;
	using Kind = solidity::frontend::FunctionType::Kind;

	auto callKind = *_node.annotation().kind;

	// High-level classification
	switch (callKind)
	{
	case FunctionCallKind::TypeConversion:
		return std::make_unique<SolTypeConversion>(m_ctx, _node);

	case FunctionCallKind::StructConstructorCall:
		return std::make_unique<SolStructConstruction>(m_ctx, _node);

	case FunctionCallKind::FunctionCall:
		break; // fall through to FunctionType::Kind dispatch below
	}

	// Get the resolved function type for detailed dispatch
	auto const* funcType = dynamic_cast<solidity::frontend::FunctionType const*>(
		_node.expression().annotation().type);
	if (!funcType)
		return nullptr;

	switch (funcType->kind())
	{
	// ── Builtins ──
	case Kind::Require:
	case Kind::Assert:
		return std::make_unique<SolRequireAssert>(m_ctx, _node);

	case Kind::Revert:
	case Kind::Error:
		return std::make_unique<SolRevert>(m_ctx, _node);

	case Kind::KECCAK256:
		return std::make_unique<SolBuiltinCall>(m_ctx, _node, "keccak256");
	case Kind::SHA256:
		return std::make_unique<SolBuiltinCall>(m_ctx, _node, "sha256");
	case Kind::AddMod:
		return std::make_unique<SolBuiltinCall>(m_ctx, _node, "addmod");
	case Kind::MulMod:
		return std::make_unique<SolBuiltinCall>(m_ctx, _node, "mulmod");
	case Kind::GasLeft:
		return std::make_unique<SolBuiltinCall>(m_ctx, _node, "gasleft");
	case Kind::Selfdestruct:
		return std::make_unique<SolBuiltinCall>(m_ctx, _node, "selfdestruct");
	case Kind::BlockHash:
		return std::make_unique<SolBuiltinCall>(m_ctx, _node, "blockhash");
	case Kind::ECRecover:
		return std::make_unique<SolBuiltinCall>(m_ctx, _node, "ecrecover");

	case Kind::ERC7201:
		return std::make_unique<SolBuiltinCall>(m_ctx, _node, "erc7201");

	// ── ABI ──
	case Kind::ABIEncode:
	case Kind::ABIEncodePacked:
	case Kind::ABIEncodeWithSelector:
	case Kind::ABIEncodeCall:
	case Kind::ABIEncodeWithSignature:
		return std::make_unique<SolAbiEncode>(m_ctx, _node);

	case Kind::ABIDecode:
		return std::make_unique<SolAbiDecode>(m_ctx, _node);

	// ── Address calls ──
	case Kind::BareCall:
	case Kind::BareCallCode:
		return std::make_unique<SolBareCall>(m_ctx, _node);

	case Kind::BareStaticCall:
		return std::make_unique<SolBareCall>(m_ctx, _node);

	case Kind::BareDelegateCall:
		return std::make_unique<SolBareCall>(m_ctx, _node);

	case Kind::Transfer:
		return std::make_unique<SolTransferSend>(m_ctx, _node);

	case Kind::Send:
		return std::make_unique<SolTransferSend>(m_ctx, _node);

	// ── Array methods ──
	case Kind::ArrayPush:
	case Kind::ArrayPop:
		return std::make_unique<SolArrayMethod>(m_ctx, _node);

	// ── Type operations ──
	case Kind::Wrap:
	case Kind::Unwrap:
		return std::make_unique<SolWrapUnwrap>(m_ctx, _node);

	case Kind::ObjectCreation:
		return std::make_unique<SolNewExpression>(m_ctx, _node);

	case Kind::Event:
		// Events are handled as statements in EmitBuilder.cpp.
		// Kind::Event FunctionCalls are not reached in practice.
		return nullptr;

	case Kind::MetaType:
		return std::make_unique<SolMetaType>(m_ctx, _node);

	// ── Regular calls ──
	case Kind::Internal:
		return std::make_unique<SolInternalCall>(m_ctx, _node);

	case Kind::External:
	case Kind::DelegateCall:
	{
		auto plan = eb::CallResolver::plan(_node);
		if (plan.transport == eb::CallTransport::Internal)
			return std::make_unique<SolInternalCall>(m_ctx, _node);
		return std::make_unique<SolExternalCall>(m_ctx, _node);
	}

	case Kind::Creation:
		// Contract creation via new Contract(args) — deploy stub inner app
		return std::make_unique<SolNewExpression>(m_ctx, _node);

	case Kind::BlobHash:
		// blobhash(n) — EIP-4844; AVM has no blobs → stub returns bzero(32).
		return std::make_unique<SolBuiltinCall>(m_ctx, _node, "blobhash");
	case Kind::RIPEMD160:
		// No AVM RIPEMD-160 opcode; SolBuiltinCall synthesizes via Ripemd160Builder.
		return std::make_unique<SolBuiltinCall>(m_ctx, _node, "ripemd160");

	// ── Misc ──
	case Kind::SetGas:
	case Kind::SetValue:
	case Kind::Declaration:
	case Kind::BytesConcat:
	case Kind::StringConcat:
		return std::make_unique<SolBytesConcat>(m_ctx, _node);
	}

	return nullptr;
}

std::unique_ptr<SolMemberAccess> SolExpressionFactory::createMemberAccess(
	solidity::frontend::MemberAccess const& _node)
{
	using namespace solidity::frontend;

	std::string member = _node.memberName();
	auto const& baseExpr = _node.expression();
	auto const* baseType = baseExpr.annotation().type;

	// 1. Intrinsics: msg.*/block.*/tx.* — dispatched directly without an exploratory call.
	if (auto const* baseId = dynamic_cast<Identifier const*>(&baseExpr))
	{
		std::string const& baseName = baseId->name();
		bool isIntrinsic =
			(baseName == "block" && (member == "difficulty" || member == "prevrandao"
				|| member == "basefee" || member == "blobbasefee" || member == "gaslimit"
				|| member == "timestamp" || member == "number" || member == "chainid"
				|| member == "coinbase"))
			|| (baseName == "msg" && (member == "value" || member == "sig"
				|| member == "data" || member == "sender"))
			|| (baseName == "tx" && (member == "origin" || member == "gasprice"));
		if (isIntrinsic)
			return std::make_unique<SolIntrinsicAccess>(m_ctx, _node);
	}

	// 2. Enum value: MyEnum.Value
	if (dynamic_cast<EnumValue const*>(_node.annotation().referencedDeclaration))
		return std::make_unique<SolEnumValueAccess>(m_ctx, _node);

	// 3. Selector: f.selector, E.selector
	if (member == "selector")
		return std::make_unique<SolSelectorAccess>(m_ctx, _node);

	// 4. Event member access + constant inlining + state variable via contract name
	if (auto const* refDecl = _node.annotation().referencedDeclaration)
	{
		if (dynamic_cast<EventDefinition const*>(refDecl))
			return std::make_unique<SolConstantAccess>(m_ctx, _node);
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(refDecl))
		{
			if (varDecl->isConstant() && varDecl->value())
				return std::make_unique<SolConstantAccess>(m_ctx, _node);
			// Non-constant state variable via Contract.stateVar
			if (varDecl->isStateVariable())
				return std::make_unique<SolConstantAccess>(m_ctx, _node);
		}
		// `import "x" as M; M.L` — module-aliased contract ref. As a VALUE
		// (`address(M.L)`) emit 32-byte zero (AVM has no deployed address). For
		// `M.L.f(...)` calls, SolInternalCall's resolver dispatches directly.
		if (dynamic_cast<ContractDefinition const*>(refDecl))
			return std::make_unique<SolConstantAccess>(m_ctx, _node);
	}

	// 5. type(X).max / type(X).min / type(C).name / type(I).interfaceId
	//    type(C).creationCode / type(C).runtimeCode (stubbed as 32 zero bytes)
	if (baseType)
	{
		bool isMagicOrTypeType = dynamic_cast<MagicType const*>(baseType)
			|| dynamic_cast<TypeType const*>(baseType);
		if (isMagicOrTypeType
			&& (member == "max" || member == "min" || member == "name"
				|| member == "interfaceId"
				|| member == "creationCode" || member == "runtimeCode"))
			return std::make_unique<SolMetaTypeAccess>(m_ctx, _node);
	}

	// 6. .length on arrays/bytes
	if (member == "length")
		return std::make_unique<SolLengthAccess>(m_ctx, _node);

	// 6b. .address on external function pointer values
	if (member == "address")
	{
		auto const* baseT = baseExpr.annotation().type;
		if (auto const* bft = dynamic_cast<FunctionType const*>(baseT))
			if (bft->kind() == FunctionType::Kind::External)
				return std::make_unique<SolFunctionAddressAccess>(m_ctx, _node);
	}

	// 7. Function pointer via contract: C.f used as a value
	if (auto const* refDecl = _node.annotation().referencedDeclaration)
	{
		if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(refDecl))
		{
			auto const* exprType = _node.annotation().type;
			if (auto const* ft = dynamic_cast<FunctionType const*>(exprType))
			{
				if (ft->kind() == FunctionType::Kind::Internal
					|| ft->kind() == FunctionType::Kind::External)
					return std::make_unique<SolFunctionPointerAccess>(m_ctx, _node, funcDef, ft);
			}
		}
	}

	// 8. Contract member name (token.transfer in abi.encodeCall)
	if (baseType && baseType->category() == Type::Category::Contract)
		return std::make_unique<SolConstantAccess>(m_ctx, _node);

	// 9. Address properties (.code, .balance)
	if (baseType && baseType->category() == Type::Category::Address)
		return std::make_unique<SolAddressProperty>(m_ctx, _node);

	// 10. Struct/tuple field access (SolFieldAccess builds base first to check ARC4Struct/WTuple).
	return std::make_unique<SolFieldAccess>(m_ctx, _node);
}

} // namespace puyasol::builder::sol_ast
