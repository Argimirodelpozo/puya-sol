#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/ASTForward.h>

namespace puyasol::builder::sol_ast
{

/// Unary operations: !, ~, -, ++, --, delete.
/// Delegates to sol-eb builders for Not/Sub/BitNot; handles inc/dec/delete inline.
class SolUnaryOperation: public SolExpression
{
public:
	SolUnaryOperation(eb::ContractContext& _ctx, solidity::frontend::UnaryOperation const& _node);
	std::shared_ptr<awst::Expression> toAwst() override;

private:
	solidity::frontend::UnaryOperation const& m_unaryOp;

	bool isBigUInt(awst::WType const* _type) const;
	std::shared_ptr<awst::Expression> handleNot(std::shared_ptr<awst::Expression> _operand);
	std::shared_ptr<awst::Expression> handleNegate(std::shared_ptr<awst::Expression> _operand);
	std::shared_ptr<awst::Expression> handleBitNot(std::shared_ptr<awst::Expression> _operand);
	std::shared_ptr<awst::Expression> handleIncDec(std::shared_ptr<awst::Expression> _operand);
	std::shared_ptr<awst::Expression> handleDelete(std::shared_ptr<awst::Expression> _operand);
	/// `delete arr[i]` where `arr` is multi-box paged: zero the element's slice
	/// via box_replace at its page/offset. False when the element's width is
	/// unknown, leaving the caller's generic path in charge.
	bool clearMultiBoxElement(
		solidity::frontend::VariableDeclaration const& _var,
		awst::WType const* _arrWtype,
		std::shared_ptr<awst::Expression> const& _index);
	/// --evm-storage-layout: ++/--/delete on a storage state ref via slot RMW.
	std::shared_ptr<awst::Expression> handleEvmStorageIncDecDelete();
};

} // namespace puyasol::builder::sol_ast
