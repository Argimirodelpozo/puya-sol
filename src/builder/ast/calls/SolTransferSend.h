#pragma once

#include "builder/ast/calls/SolBareCall.h"

namespace puyasol::builder::sol_ast
{

/// address.transfer(amount) and address.send(amount): same lowering as the
/// bare-call forms — InnerCallHandlers::tryHandleAddressCall on the member
/// name emits the inner payment transaction.
class SolTransferSend: public SolBareCall
{
public:
	using SolBareCall::SolBareCall;
};

} // namespace puyasol::builder::sol_ast
