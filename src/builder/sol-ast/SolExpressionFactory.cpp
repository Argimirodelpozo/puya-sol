/// @file SolExpressionFactory.cpp
/// Factory that creates the right SolExpression subclass for a Solidity AST node.
/// Uses FunctionCallKind + FunctionType::Kind for dispatch.

#include "builder/sol-ast/SolExpressionFactory.h"
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
#include "builder/sol-intrinsics/IntrinsicMapper.h"
#include "Logger.h"

#include <libsolidity/ast/ASTAnnotations.h>

namespace puyasol::builder::sol_ast
{

SolExpressionFactory::SolExpressionFactory(eb::BuilderContext& _ctx)
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
		return std::make_unique<SolExternalCall>(m_ctx, _node);

	case Kind::Creation:
		// Contract creation — not applicable on Algorand
		return nullptr;

	// ── Misc ──
	case Kind::SetGas:
	case Kind::SetValue:
	case Kind::Declaration:
	case Kind::BlobHash:
	case Kind::RIPEMD160:
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

	// 1. Intrinsics: msg.sender, block.timestamp, block.difficulty, block.prevrandao
	if (auto const* baseId = dynamic_cast<Identifier const*>(&baseExpr))
	{
		std::string baseName = baseId->name();
		if (baseName == "block" && (member == "difficulty" || member == "prevrandao"))
			return std::make_unique<SolIntrinsicAccess>(m_ctx, _node);

		if (builder::IntrinsicMapper::tryMapMemberAccess(baseName, member,
				awst::SourceLocation{}))
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
	}

	// 5. type(X).max / type(X).min / type(C).name / type(I).interfaceId
	if (baseType)
	{
		bool isMagicOrTypeType = dynamic_cast<MagicType const*>(baseType)
			|| dynamic_cast<TypeType const*>(baseType);
		if (isMagicOrTypeType
			&& (member == "max" || member == "min" || member == "name"
				|| member == "interfaceId"))
			return std::make_unique<SolMetaTypeAccess>(m_ctx, _node);
	}

	// 6. .length on arrays/bytes
	if (member == "length")
		return std::make_unique<SolLengthAccess>(m_ctx, _node);

	// 7. Contract member name (token.transfer in abi.encodeCall)
	if (baseType && baseType->category() == Type::Category::Contract)
		return std::make_unique<SolConstantAccess>(m_ctx, _node);

	// 8. Address properties (.code, .balance)
	if (baseType && baseType->category() == Type::Category::Address)
		return std::make_unique<SolAddressProperty>(m_ctx, _node);

	// 9. Struct/tuple field access — try after building base
	// (Must check if result is ARC4Struct/WTuple, which requires building the base first.
	//  Use SolFieldAccess which handles this check internally.)
	return std::make_unique<SolFieldAccess>(m_ctx, _node);
}

} // namespace puyasol::builder::sol_ast
