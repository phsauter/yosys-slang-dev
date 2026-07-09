//
// Yosys slang frontend
//
// Copyright Martin Povišer <povik@cutebit.org>
// Distributed under the terms of the ISC license, see LICENSE
//
#pragma once

#include "slang_frontend.h"
#include "variables.h"

#include <string>
#include <vector>

namespace slang_frontend {

struct WriteDomainRecord {
	enum Kind {
		StaticBits,
		DynamicFamily,
		Unsupported
	};

	Kind kind = Unsupported;
	VariableBits bits;
	int selected_width = 0;
	std::string reason;
	slang::SourceRange source_range;
};

struct ProcessWriteDomains {
	std::vector<WriteDomainRecord> records;

	void add_static(VariableBits bits, slang::SourceRange range);
	void add_dynamic(VariableBits container_bits, int selected_width, slang::SourceRange range);
	void add_unsupported(std::string reason, slang::SourceRange range);
	void normalize();
	bool has_unsupported() const;
	VariableBits covered_bits() const;
	std::vector<std::string> format_lines() const;
};

std::vector<WriteDomainRecord> classify_lhs_write_domains(
		EvalContext &eval, const ast::Expression &expr);
std::string format_write_domain_record(const WriteDomainRecord &record);
const char *process_kind_name(ast::ProceduralBlockKind kind);
void log_write_domains(const ProcessWriteDomains &domains, RTLIL::IdString module_name,
		ast::ProceduralBlockKind process_kind);

} // namespace slang_frontend
