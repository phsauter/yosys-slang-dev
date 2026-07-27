//
// Yosys slang frontend
//
// Copyright Martin Povišer <povik@cutebit.org>
// Distributed under the terms of the ISC license, see LICENSE
//
// Implementation of the experimental SystemVerilog architectural-variants
// system: modules carrying `(* arch_variant = "..." *) parameter string`
// selectors (and optionally an `(* arch_variant_configs = "..." *)` matrix)
// are expanded into one module copy per valid selector configuration.
//
#include "arch_variants.h"

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/symbols/AttributeSymbol.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"

#include <algorithm>
#include <cctype>

#include "diag.h"

namespace slang_frontend {

static std::string trim(std::string_view text)
{
	size_t begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string_view::npos)
		return "";
	size_t end = text.find_last_not_of(" \t\r\n");
	return std::string(text.substr(begin, end - begin + 1));
}

static std::vector<std::string> split_trim(std::string_view text, char sep)
{
	std::vector<std::string> ret;
	size_t pos = 0;
	while (true) {
		size_t next = text.find(sep, pos);
		std::string item = trim(text.substr(pos, next == std::string_view::npos
												 ? std::string_view::npos : next - pos));
		if (!item.empty())
			ret.push_back(std::move(item));
		if (next == std::string_view::npos)
			break;
		pos = next + 1;
	}
	return ret;
}

// slang converts string-literal attribute values to integer constants per the
// LRM; convert back to text
std::optional<std::string> arch_attr_string_value(const ast::AttributeSymbol &attr)
{
	const slang::ConstantValue &value = attr.getValue();
	if (value.isString())
		return value.str();
	if (value.isInteger()) {
		slang::ConstantValue converted = value.convertToStr();
		if (converted.isString())
			return converted.str();
	}
	return std::nullopt;
}

static std::string param_value_string(const ast::ParameterSymbol &param)
{
	const slang::ConstantValue &value = param.getValue();
	if (value.isString())
		return value.str();
	slang::ConstantValue converted = value.convertToStr();
	if (converted.isString())
		return converted.str();
	return value.toString();
}

static std::string sanitize_name_part(std::string_view raw)
{
	std::string ret;
	for (unsigned char ch : raw)
		ret.push_back((std::isalnum(ch) || ch == '_') ? ch : '_');
	return ret;
}

static std::string tuple_text(const ArchVariantDefinitionInfo &info,
		const std::vector<std::string> &config)
{
	std::string ret = "{";
	for (size_t i = 0; i < info.selectors.size() && i < config.size(); i++) {
		if (i)
			ret += ", ";
		ret += info.selectors[i].name;
		ret += "=";
		ret += config[i];
	}
	ret += "}";
	return ret;
}

static std::string make_descr(const ArchVariantDefinitionInfo &info,
		const std::vector<std::string> &config)
{
	return info.strictness_text + tuple_text(info, config);
}

static std::string make_suffix(const ArchVariantDefinitionInfo &info,
		const std::vector<std::string> &config)
{
	std::string ret;
	for (size_t i = 0; i < info.selectors.size() && i < config.size(); i++) {
		if (i)
			ret += "__";
		ret += sanitize_name_part(info.selectors[i].name);
		ret += "_";
		ret += sanitize_name_part(config[i]);
	}
	return ret;
}

static const ast::ParameterSymbol *find_body_parameter(
		const ast::InstanceBodySymbol &body, std::string_view name)
{
	for (auto *param : body.getParameters()) {
		if (param->symbol.kind == ast::SymbolKind::Parameter && param->symbol.name == name)
			return &param->symbol.as<ast::ParameterSymbol>();
	}
	return nullptr;
}

void ArchVariantExpander::run()
{
	for (auto instance : compilation.getRoot().topInstances)
		discover(*instance);
	expand_groups();
}

void ArchVariantExpander::discover(const ast::InstanceSymbol &instance)
{
	if (instance.body.flags.has(ast::InstanceFlags::Uninstantiated))
		return;

	const ast::DefinitionSymbol &definition = instance.body.getDefinition();

	if (instance.isModule() && settings.is_blackbox(definition))
		return;

	if (instance.isModule() && definition.definitionKind == ast::DefinitionKind::Module) {
		ArchVariantDefinitionInfo &info = get_definition_info(instance.body);
		if (info.is_variant && info.valid)
			note_group_member(instance, info);
	}

	instance.body.visit(ast::makeVisitor(
		[&](auto &, const ast::InstanceSymbol &child) {
			discover(child);
		},
		[&](auto &visitor, const ast::GenerateBlockSymbol &sym) {
			if (!sym.isUninstantiated)
				visitor.visitDefault(sym);
		}));
}

ArchVariantDefinitionInfo &ArchVariantExpander::get_definition_info(
		const ast::InstanceBodySymbol &body)
{
	const ast::DefinitionSymbol &definition = body.getDefinition();
	auto it = def_info.find(&definition);
	if (it != def_info.end())
		return it->second;

	ArchVariantDefinitionInfo &info = def_info[&definition];
	parse_definition(body, info);
	if (info.is_variant)
		settings.arch_variant_definitions.insert(&definition);
	return info;
}

void ArchVariantExpander::parse_definition(const ast::InstanceBodySymbol &body,
		ArchVariantDefinitionInfo &info)
{
	const ast::DefinitionSymbol &definition = body.getDefinition();

	const ast::AttributeSymbol *configs_attr = nullptr;
	for (auto attr : compilation.getAttributes(definition)) {
		if (attr->name == "arch_variant_configs")
			configs_attr = attr;
	}

	for (auto *param_base : body.getParameters()) {
		const ast::AttributeSymbol *selector_attr = nullptr;
		for (auto attr : compilation.getAttributes(param_base->symbol)) {
			if (attr->name == "arch_variant")
				selector_attr = attr;
		}
		if (!selector_attr)
			continue;

		info.is_variant = true;

		if (param_base->isLocalParam() ||
				param_base->symbol.kind != ast::SymbolKind::Parameter ||
				!param_base->symbol.as<ast::ParameterSymbol>().getType().isString()) {
			add_diag(diag::ArchVariantBadSelector, param_base->symbol.location);
			info.valid = false;
			continue;
		}

		const ast::ParameterSymbol &param = param_base->symbol.as<ast::ParameterSymbol>();
		std::optional<std::string> text = arch_attr_string_value(*selector_attr);
		std::vector<std::string> values;
		if (text)
			values = split_trim(*text, ',');
		if (values.empty()) {
			auto &diag = add_diag(diag::ArchVariantEmptyValues, selector_attr->location);
			diag << param.name;
			info.valid = false;
			continue;
		}

		info.selectors.push_back(ArchVariantSelector{
				&param, std::string(param.name), std::move(values), param.location});
	}

	if (!info.is_variant) {
		if (configs_attr) {
			auto &diag = add_diag(diag::ArchVariantBadConfigs, configs_attr->location);
			diag << std::string("no variant selectors in module");
		}
		return;
	}

	if (!info.valid)
		return;

	if (!configs_attr) {
		if (info.selectors.size() > 1) {
			auto &diag = add_diag(diag::ArchVariantMissingConfigs, definition.location);
			diag << definition.name;
			info.valid = false;
			return;
		}
		// implicit matrix: every valid value of the single selector
		for (auto &value : info.selectors[0].valid_values)
			info.configs.push_back({value});
		return;
	}

	auto fail = [&](const std::string &why) {
		auto &diag = add_diag(diag::ArchVariantBadConfigs, configs_attr->location);
		diag << why;
		info.valid = false;
	};

	std::optional<std::string> text = arch_attr_string_value(*configs_attr);
	if (!text) {
		fail("attribute value is not a string");
		return;
	}

	const std::string &s = *text;
	size_t lbrace = s.find('{');
	size_t rbrace = s.find('}');
	if (lbrace == std::string::npos || rbrace == std::string::npos || rbrace < lbrace) {
		fail("expected '{' parameter list '}'");
		return;
	}

	std::string modifier = trim(std::string_view(s).substr(0, lbrace));
	if (!modifier.empty() && modifier != "STRICT" && modifier != "CHECK" && modifier != "PREFER") {
		fail("unknown strictness modifier '" + modifier + "'");
		return;
	}
	info.strictness_text = modifier;

	std::vector<std::string> names = split_trim(
			std::string_view(s).substr(lbrace + 1, rbrace - lbrace - 1), ',');
	if (names.empty()) {
		fail("empty parameter list");
		return;
	}

	size_t equals = s.find('=', rbrace);
	if (equals == std::string::npos) {
		fail("expected '=' after parameter list");
		return;
	}

	// reorder selectors into the listed order
	std::vector<ArchVariantSelector> ordered;
	for (auto &name : names) {
		auto it = std::find_if(info.selectors.begin(), info.selectors.end(),
				[&](const ArchVariantSelector &sel) { return sel.name == name; });
		if (it == info.selectors.end()) {
			auto &diag = add_diag(diag::ArchVariantUnknownParam, configs_attr->location);
			diag << name << definition.name;
			info.valid = false;
			return;
		}
		if (std::find_if(ordered.begin(), ordered.end(),
				[&](const ArchVariantSelector &sel) { return sel.name == name; }) != ordered.end()) {
			fail("selector '" + name + "' is listed twice");
			return;
		}
		ordered.push_back(*it);
	}
	for (auto &sel : info.selectors) {
		if (std::find(names.begin(), names.end(), sel.name) == names.end()) {
			fail("selector '" + sel.name + "' is not listed");
			return;
		}
	}
	info.selectors = std::move(ordered);

	// parse the configuration items
	std::set<std::vector<std::string>> seen_items;
	size_t pos = equals + 1;
	while (true) {
		while (pos < s.size() && std::isspace((unsigned char)s[pos]))
			pos++;
		if (pos >= s.size())
			break;
		if (s[pos] != '[') {
			fail("expected '[' to start a configuration item");
			return;
		}
		size_t rbracket = s.find(']', pos);
		if (rbracket == std::string::npos) {
			fail("unterminated configuration item");
			return;
		}
		std::vector<std::string> item = split_trim(
				std::string_view(s).substr(pos + 1, rbracket - pos - 1), ',');
		if (item.size() != info.selectors.size()) {
			fail("configuration item has " + std::to_string(item.size()) +
					" values, expected " + std::to_string(info.selectors.size()));
			return;
		}
		for (size_t i = 0; i < item.size(); i++) {
			auto &valid = info.selectors[i].valid_values;
			if (std::find(valid.begin(), valid.end(), item[i]) == valid.end()) {
				auto &diag = add_diag(diag::ArchVariantConfigValue, configs_attr->location);
				diag << item[i] << info.selectors[i].name;
				info.valid = false;
			}
		}
		if (!seen_items.insert(item).second)
			add_diag(diag::ArchVariantDuplicateConfig, configs_attr->location);
		else
			info.configs.push_back(std::move(item));

		pos = rbracket + 1;
		while (pos < s.size() && std::isspace((unsigned char)s[pos]))
			pos++;
		if (pos >= s.size())
			break;
		if (s[pos] != ',') {
			fail("unexpected text after configuration item");
			return;
		}
		pos++;
	}

	if (info.configs.empty() && info.valid)
		fail("no configuration items");
}

void ArchVariantExpander::check_default_values(const ast::InstanceBodySymbol &body,
		ArchVariantDefinitionInfo &info)
{
	if (info.checked_default)
		return;

	bool all_checked = true;
	for (auto &sel : info.selectors) {
		const ast::ParameterSymbol *param = find_body_parameter(body, sel.name);
		if (!param || param->isOverridden()) {
			all_checked = false;
			continue;
		}
		std::string value = param_value_string(*param);
		if (std::find(sel.valid_values.begin(), sel.valid_values.end(), value) ==
				sel.valid_values.end()) {
			auto &diag = add_diag(diag::ArchVariantBadDefault, param->location);
			diag << value << sel.name;
			info.valid = false;
		}
	}
	if (all_checked)
		info.checked_default = true;
}

void ArchVariantExpander::note_group_member(const ast::InstanceSymbol &instance,
		ArchVariantDefinitionInfo &info)
{
	const ast::InstanceBodySymbol &body = instance.body;
	const ast::DefinitionSymbol &definition = body.getDefinition();

	check_default_values(body, info);
	if (!info.valid)
		return;

	auto is_selector = [&](std::string_view name) {
		return std::find_if(info.selectors.begin(), info.selectors.end(),
				[&](const ArchVariantSelector &sel) { return sel.name == name; }) !=
				info.selectors.end();
	};

	// non-selector parameterization signature; also detect stage-1 limitations
	std::string signature;
	std::string unsupported_reason;
	for (auto *param_base : body.getParameters()) {
		if (param_base->isLocalParam())
			continue;
		if (param_base->symbol.kind == ast::SymbolKind::Parameter) {
			const auto &param = param_base->symbol.as<ast::ParameterSymbol>();
			if (is_selector(param.name))
				continue;
			signature += "param ";
			signature += param.name;
			signature += "=";
			signature += param.getValue().toString(slang::SVInt::MAX_BITS, true);
			signature += "\n";
		} else {
			const auto &param = param_base->symbol.as<ast::TypeParameterSymbol>();
			if (param.isOverridden())
				unsupported_reason = "overridden type parameters are unsupported";
			signature += "type ";
			signature += param.name;
			signature += "=";
			signature += param.targetType.getType().toString();
			signature += "\n";
		}
	}
	for (auto port : body.getPortList()) {
		if (port->kind == ast::SymbolKind::InterfacePort)
			unsupported_reason = "interface ports are unsupported";
	}

	auto key = std::make_pair(&definition, signature);
	size_t group_index;
	auto it = group_by_key.find(key);
	if (it != group_by_key.end()) {
		group_index = it->second;
	} else {
		group_index = groups.size();
		group_by_key[key] = group_index;
		groups.push_back(ArchVariantGroup{&definition, &instance, nullptr, signature, false});
		group_data.emplace_back();
	}

	if (!unsupported_reason.empty() && !groups[group_index].skip_expansion) {
		groups[group_index].skip_expansion = true;
		auto &diag = add_diag(diag::ArchVariantUnsupported, instance.location);
		diag << definition.name << unsupported_reason;
	}

	std::vector<std::string> tuple;
	for (auto &sel : info.selectors) {
		const ast::ParameterSymbol *param = find_body_parameter(body, sel.name);
		if (!param)
			return;
		tuple.push_back(param_value_string(*param));
	}
	group_data[group_index].existing.emplace(std::move(tuple), &instance);
}

void ArchVariantExpander::expand_groups()
{
	for (size_t group_index = 0; group_index < groups.size(); group_index++) {
		ArchVariantGroup &group = groups[group_index];
		GroupData &data = group_data[group_index];
		ArchVariantDefinitionInfo &info = def_info.at(group.definition);

		if (!info.valid || info.configs.empty() || group.skip_expansion)
			continue;

		std::set<std::vector<std::string>> listed;
		for (auto &config : info.configs) {
			listed.insert(config);
			auto it = data.existing.find(config);
			if (it != data.existing.end()) {
				emissions.push_back(ArchVariantEmission{it->second, nullptr, group_index,
						false, "", make_descr(info, config)});
			} else {
				const ast::InstanceSymbol *variant = create_variant(group, info, config);
				if (variant)
					emissions.push_back(ArchVariantEmission{variant, nullptr, group_index,
							true, make_suffix(info, config), make_descr(info, config)});
			}
		}

		// instantiations using selector combinations outside the matrix
		for (auto &[tuple, instance] : data.existing) {
			if (listed.count(tuple))
				continue;
			bool strict = info.strictness_text == "STRICT";
			auto &diag = add_diag(strict ? diag::ArchVariantComboIllegal
										 : diag::ArchVariantComboUnlisted,
					instance->location);
			diag << tuple_text(info, tuple) << group.definition->name;
			if (!strict) {
				// treated as an additional ad-hoc configuration
				emissions.push_back(ArchVariantEmission{instance, nullptr, group_index,
						false, "", make_descr(info, tuple)});
			}
		}
	}
}

const ast::InstanceSymbol *ArchVariantExpander::create_variant(const ArchVariantGroup &group,
		const ArchVariantDefinitionInfo &info, const std::vector<std::string> &config)
{
	ast::HierarchyOverrideNode &node = override_storage.emplace_back();
	const ast::InstanceBodySymbol &body = group.rep_instance->body;

	for (auto *param_base : body.getParameters()) {
		if (param_base->isLocalParam())
			continue;
		if (param_base->symbol.kind != ast::SymbolKind::Parameter)
			continue; // type parameters keep their defaults (overrides are rejected earlier)

		const auto &param = param_base->symbol.as<ast::ParameterSymbol>();
		const slang::syntax::SyntaxNode *syntax = param.getSyntax();
		if (!syntax)
			continue;

		slang::ConstantValue value;
		auto sel = std::find_if(info.selectors.begin(), info.selectors.end(),
				[&](const ArchVariantSelector &s) { return s.name == param.name; });
		if (sel != info.selectors.end())
			value = config[sel - info.selectors.begin()];
		else
			value = param.getValue();

		if (value.bad())
			continue;

		node.paramOverrides.emplace(syntax,
				ast::HierarchyOverrideNode::ParamOverride{std::move(value), nullptr, nullptr});
	}

	ast::InstanceSymbol &instance =
			ast::InstanceSymbol::createDefault(compilation, *group.definition, &node);
	// The instance is detached from the design tree; give it a parent scope
	// (like InstanceSymbol::createVirtual does) so port connection resolution
	// works. It is intentionally not added as a member anywhere.
	instance.setParent(*group.definition->getParentScope());
	force_elaborate(instance.body);
	return &instance;
}

void ArchVariantExpander::force_elaborate(const ast::InstanceBodySymbol &body)
{
	if (!forced_bodies.insert(&body).second)
		return;

	// Compilation::forceElaborate does not descend into child instance
	// bodies (visitInstances=false), so recurse over them explicitly to make
	// sure elaboration errors anywhere inside a variant are issued.
	compilation.forceElaborate(body);
	body.visit(ast::makeVisitor(
		[&](auto &, const ast::InstanceSymbol &child) {
			if (child.body.flags.has(ast::InstanceFlags::Uninstantiated))
				return;
			force_elaborate(child.body);
		},
		[&](auto &visitor, const ast::GenerateBlockSymbol &sym) {
			if (!sym.isUninstantiated)
				visitor.visitDefault(sym);
		}));
}

void ArchVariantExpander::resolve_bodies()
{
	for (auto &group : groups)
		group.origin_body = &get_instance_body(settings, *group.rep_instance);

	std::set<const ast::InstanceBodySymbol *> seen;
	std::vector<ArchVariantEmission> kept;
	for (auto &emission : emissions) {
		emission.body = emission.synthetic ? &emission.instance->body
										   : &get_instance_body(settings, *emission.instance);
		if (seen.insert(emission.body).second)
			kept.push_back(emission);
	}
	emissions.swap(kept);
}

}; // namespace slang_frontend
