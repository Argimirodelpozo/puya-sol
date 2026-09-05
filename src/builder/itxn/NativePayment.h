#pragma once

#include "awst/Node.h"
#include "builder/TargetProfile.h"

namespace puyasol::builder
{

/// Shared native-payment boundary: resolve the receiver, enforce the EVM
/// identity policy, and check the amount before narrowing to AVM uint64.
/// An application-typed receiver is a known app identity; account/numeric
/// receivers use the address convention and, when configured, xchain mapping.
std::shared_ptr<awst::CreateInnerTransaction> buildNativePayment(
	TargetProfile const& _profile,
	std::vector<std::shared_ptr<awst::Statement>>& _preEffects,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount,
	awst::SourceLocation const& _loc);

/// Solidity transfer/send: invoke receive/fallback for application identities
/// in the same inner group as the payment; ordinary accounts receive only pay.
std::shared_ptr<awst::Statement> buildNativeTransfer(
	TargetProfile const& _profile,
	std::vector<std::shared_ptr<awst::Statement>>& _preEffects,
	std::shared_ptr<awst::Expression> _receiver,
	std::shared_ptr<awst::Expression> _amount,
	awst::SourceLocation const& _loc);

/// Same receiver/policy boundary for selfdestruct's CloseRemainderTo field.
std::shared_ptr<awst::CreateInnerTransaction> buildNativeClose(
	TargetProfile const& _profile,
	std::vector<std::shared_ptr<awst::Statement>>& _preEffects,
	std::shared_ptr<awst::Expression> _beneficiary,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder
