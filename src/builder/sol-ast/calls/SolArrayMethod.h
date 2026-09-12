#pragma once

#include "builder/sol-ast/SolFunctionCall.h"

namespace puyasol::builder::sol_ast
{

/// array.push(val), array.push(), and array.pop().
/// Solc storage arrays lowered through slot, box or mutable-value carriers.
class SolArrayMethod: public SolFunctionCall
{
public:
	using SolFunctionCall::SolFunctionCall;
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	std::shared_ptr<awst::Expression> buildArrayTarget(solidity::frontend::Expression const& source);
	/// Typed source conversion followed by the selected element encoding.
	std::shared_ptr<awst::Expression> buildPushValue(
		solidity::frontend::Type const* elementType, awst::WType const* representation);
	// ── toAwst base-shape rungs (SolArrayMethod.cpp) ────────────────────
	std::shared_ptr<awst::Expression> buildBytesPushPop(
		std::string const& memberName,
		solidity::frontend::Expression const& baseExpr,
		solidity::frontend::ArrayType const& array);
	std::shared_ptr<awst::Expression> buildSlotModeArrayPushPop(
		std::string const& memberName,
		solidity::frontend::Expression const& baseExpr,
		solidity::frontend::ArrayType const* arrT);
	std::shared_ptr<awst::Expression> tryBoxedElementPushPop(
		std::string const& memberName,
		solidity::frontend::Expression const& baseExpr);
	std::shared_ptr<awst::Expression> tryStoragePointerPushPop(
		std::string const& memberName,
		solidity::frontend::Expression const& baseExpr);
	std::shared_ptr<awst::Expression> emitArrayPushPop(
		std::string const& memberName,
		std::shared_ptr<awst::Expression> baseAwst,
		solidity::frontend::ArrayType const& solArrType);
	std::shared_ptr<awst::Expression> tryChainedFieldPushPop(
		std::string const& memberName,
		solidity::frontend::Expression const& baseExpr,
		solidity::frontend::MemberAccess const& innerMA);

	/// Handle push/pop on box-backed dynamic arrays.
	std::shared_ptr<awst::Expression> handleBoxArray(
		std::string const& _memberName,
		solidity::frontend::Expression const& _baseExpr,
		solidity::frontend::VariableDeclaration const& _varDecl,
		std::shared_ptr<awst::Expression> _runtimeKey = nullptr);

	/// Length-only push/pop for arrays whose element type is a mapping
	/// (`mapping(K=>V)[] a`). The box only stores a 2-byte length header;
	/// each `a[i][k]` lives in its own derived box, persisting across
	/// `delete a` (matching EVM's "delete leaves data at hash" semantic).
	std::shared_ptr<awst::Expression> handleMappingElementArrayLengthOp(
		std::string const& _memberName,
		solidity::frontend::VariableDeclaration const& _varDecl,
		std::string const& _arrayVarName,
		std::shared_ptr<awst::Expression> _runtimeKey = nullptr);
};

} // namespace puyasol::builder::sol_ast
