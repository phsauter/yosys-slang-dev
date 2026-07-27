//
// Yosys slang frontend
//
// Copyright Martin Povišer <povik@cutebit.org>
// Distributed under the terms of the ISC license, see LICENSE
//
// Support for the experimental SystemVerilog architectural-variants system
// (`arch_variant` / `arch_variant_configs` attributes), gated behind the
// `read_slang --arch-variants` option.
//
#pragma once
#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "slang_frontend.h"

namespace slang_frontend {

// slang converts string-literal attribute values to integer constants per the
// LRM; this converts back to text
std::optional<std::string> arch_attr_string_value(const ast::AttributeSymbol &attr);

// One selector (`(* arch_variant = "..." *) parameter string ...`) on a module
struct ArchVariantSelector {
	const ast::ParameterSymbol *rep_param = nullptr;
	std::string name;
	std::vector<std::string> valid_values;
	slang::SourceLocation location;
};

// Parsed per-definition variant description
struct ArchVariantDefinitionInfo {
	bool is_variant = false;
	// false when attribute parsing/validation failed; errors have been issued
	// and no expansion is attempted
	bool valid = true;
	// selectors in `arch_variant_configs` order (or declaration order for the
	// implicit single-selector matrix)
	std::vector<ArchVariantSelector> selectors;
	// strictness modifier exactly as written by the user ("" if absent);
	// absent means CHECK semantics
	std::string strictness_text;
	// valid selector-value combinations, each in `selectors` order
	std::vector<std::vector<std::string>> configs;
	bool checked_default = false;
};

// One (definition, non-selector parameterization) expansion group
struct ArchVariantGroup {
	const ast::DefinitionSymbol *definition = nullptr;
	const ast::InstanceSymbol *rep_instance = nullptr;
	// resolved in resolve_bodies() (canonical body of rep_instance)
	const ast::InstanceBodySymbol *origin_body = nullptr;
	std::string nonselector_signature;
	bool skip_expansion = false;
};

// One module to be emitted with arch-variant attributes: either an existing
// instantiated body (synthetic=false) or a detached instance created for a
// configuration no instance uses (synthetic=true)
struct ArchVariantEmission {
	const ast::InstanceSymbol *instance = nullptr;
	// resolved in resolve_bodies()
	const ast::InstanceBodySymbol *body = nullptr;
	size_t group_index = 0;
	bool synthetic = false;
	// `Sel1_VAL1__Sel2_VAL2` payload for `__av__` naming (synthetic only)
	std::string suffix;
	// `arch_variant_descr` attribute value
	std::string descr;
};

class ArchVariantExpander : public DiagnosticIssuer {
public:
	ArchVariantExpander(SynthesisSettings &settings, ast::Compilation &compilation)
		: settings(settings), compilation(compilation) {}

	// Phase A: must run after Driver::createCompilation() and *before* the
	// first Compilation::getAllDiagnostics() call (Driver::reportCompilation),
	// so that forced elaboration of all variant configurations lands in the
	// compilation's diagnostics. Detects selector-bearing modules, parses and
	// validates the attributes, creates one detached instance per not-yet-
	// instantiated configuration and force-elaborates it.
	void run();

	// Phase B: must run after AST elaboration finished (canonical bodies are
	// assigned). Resolves the bodies backing groups and emissions.
	void resolve_bodies();

	std::vector<ArchVariantGroup> groups;
	std::vector<ArchVariantEmission> emissions;

private:
	struct GroupData {
		std::map<std::vector<std::string>, const ast::InstanceSymbol *> existing;
	};

	SynthesisSettings &settings;
	ast::Compilation &compilation;

	std::map<const ast::DefinitionSymbol *, ArchVariantDefinitionInfo> def_info;
	std::map<std::pair<const ast::DefinitionSymbol *, std::string>, size_t> group_by_key;
	std::vector<GroupData> group_data;
	// stable storage: bodies keep pointers into this for the whole compilation
	std::deque<ast::HierarchyOverrideNode> override_storage;
	std::set<const ast::InstanceBodySymbol *> forced_bodies;

	void discover(const ast::InstanceSymbol &instance);
	ArchVariantDefinitionInfo &get_definition_info(const ast::InstanceBodySymbol &body);
	void parse_definition(const ast::InstanceBodySymbol &body, ArchVariantDefinitionInfo &info);
	void check_default_values(const ast::InstanceBodySymbol &body, ArchVariantDefinitionInfo &info);
	void note_group_member(const ast::InstanceSymbol &instance, ArchVariantDefinitionInfo &info);
	void expand_groups();
	const ast::InstanceSymbol *create_variant(const ArchVariantGroup &group,
			const ArchVariantDefinitionInfo &info, const std::vector<std::string> &config);
	void force_elaborate(const ast::InstanceBodySymbol &body);
};

}; // namespace slang_frontend
