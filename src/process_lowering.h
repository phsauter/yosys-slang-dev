//
// Yosys slang frontend
//
// Copyright Martin Povišer <povik@cutebit.org>
// Distributed under the terms of the ISC license, see LICENSE
//
#pragma once

#include "slang_frontend.h"

namespace slang_frontend {

void lower_comb_like_process(NetlistContext &netlist,
		const ast::ProceduralBlockSymbol &symbol, const ast::Statement &body);

} // namespace slang_frontend
