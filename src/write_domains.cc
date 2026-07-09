//
// Yosys slang frontend
//
// Copyright Martin Povišer <povik@cutebit.org>
// Distributed under the terms of the ISC license, see LICENSE
//

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Statement.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/Type.h"

#include "write_domains.h"

#include <algorithm>
#include <sstream>

namespace slang_frontend {

static std::string bits_text(const VariableBits &bits)
{
	std::ostringstream out;
	bool first = true;

	for (auto chunk : bits.chunks()) {
		if (!first)
			out << ",";
		first = false;
		out << chunk.text();
	}

	return out.str();
}

static bool same_bits(const VariableBits &a, const VariableBits &b)
{
	return a == b;
}

static bool single_real_root(const VariableBits &bits, Variable &root)
{
	bool have_root = false;

	for (auto bit : bits) {
		if (bit.variable.kind == Variable::Dummy)
			return false;
		if (!have_root) {
			root = bit.variable;
			have_root = true;
		} else if (root != bit.variable) {
			return false;
		}
	}

	return have_root;
}

void ProcessWriteDomains::add_static(VariableBits bits, slang::SourceRange range)
{
	if (bits.empty())
		return;

	WriteDomainRecord record;
	record.kind = WriteDomainRecord::StaticBits;
	record.bits = bits;
	record.source_range = range;
	records.push_back(record);
}

void ProcessWriteDomains::add_dynamic(
		VariableBits container_bits, int selected_width, slang::SourceRange range)
{
	if (container_bits.empty())
		return;

	WriteDomainRecord record;
	record.kind = WriteDomainRecord::DynamicFamily;
	record.bits = container_bits;
	record.selected_width = selected_width;
	record.source_range = range;
	records.push_back(record);
}

void ProcessWriteDomains::add_unsupported(std::string reason, slang::SourceRange range)
{
	WriteDomainRecord record;
	record.kind = WriteDomainRecord::Unsupported;
	record.reason = std::move(reason);
	record.source_range = range;
	records.push_back(record);
}

void ProcessWriteDomains::normalize()
{
	Yosys::dict<Variable, VariableBits> static_by_root;
	std::vector<WriteDomainRecord> dynamic_records;
	std::vector<WriteDomainRecord> unsupported_records;

	for (auto &record : records) {
		if (record.kind == WriteDomainRecord::StaticBits) {
			Variable root;
			if (!single_real_root(record.bits, root)) {
				unsupported_records.push_back(record);
				unsupported_records.back().kind = WriteDomainRecord::Unsupported;
				unsupported_records.back().reason = "static write spans multiple roots";
				continue;
			}
			static_by_root[root].append(record.bits);
			continue;
		}

		if (record.kind == WriteDomainRecord::DynamicFamily) {
			dynamic_records.push_back(record);
			continue;
		}

		unsupported_records.push_back(record);
	}

	records.clear();

	for (auto &entry : static_by_root) {
		VariableBits bits = entry.second;
		bits.sort_and_unify();

		WriteDomainRecord record;
		record.kind = WriteDomainRecord::StaticBits;
		record.bits = bits;
		records.push_back(record);
	}

	for (auto &record : dynamic_records) {
		bool duplicate = false;
		for (auto &existing : records) {
			if (existing.kind == WriteDomainRecord::DynamicFamily &&
					existing.selected_width == record.selected_width &&
					same_bits(existing.bits, record.bits)) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			records.push_back(record);
	}

	records.insert(records.end(), unsupported_records.begin(), unsupported_records.end());

	std::sort(records.begin(), records.end(), [](const WriteDomainRecord &a,
													 const WriteDomainRecord &b) {
		if (a.kind != b.kind)
			return a.kind < b.kind;
		if (bits_text(a.bits) != bits_text(b.bits))
			return bits_text(a.bits) < bits_text(b.bits);
		if (a.selected_width != b.selected_width)
			return a.selected_width < b.selected_width;
		return a.reason < b.reason;
	});
}

std::string format_write_domain_record(const WriteDomainRecord &record)
{
	std::ostringstream line;

	if (record.kind == WriteDomainRecord::StaticBits) {
		line << "static " << bits_text(record.bits);
	} else if (record.kind == WriteDomainRecord::DynamicFamily) {
		line << "dynamic " << bits_text(record.bits)
			 << " selected_width=" << record.selected_width;
	} else {
		line << "unsupported " << record.reason;
	}

	return line.str();
}

std::vector<std::string> ProcessWriteDomains::format_lines() const
{
	std::vector<std::string> lines;

	for (auto &record : records) {
		lines.push_back("slang-write-domain " + format_write_domain_record(record));
	}

	return lines;
}

bool ProcessWriteDomains::has_unsupported() const
{
	for (auto &record : records) {
		if (record.kind == WriteDomainRecord::Unsupported)
			return true;
	}
	return false;
}

VariableBits ProcessWriteDomains::covered_bits() const
{
	VariableBits bits;

	for (auto &record : records) {
		if (record.kind != WriteDomainRecord::Unsupported)
			bits.append(record.bits);
	}

	bits.sort_and_unify();
	return bits;
}

static bool const_int(EvalContext &eval, const ast::Expression &expr, int64_t &value)
{
	auto constant = expr.eval(eval.const_);
	if (!constant || !constant.isInteger())
		return false;

	auto converted = constant.integer().as<int64_t>();
	if (!converted)
		return false;

	value = converted.value();
	return true;
}

static int select_stride(const ast::Expression &container)
{
	const ast::Type &type = *container.type;
	if (type.isArray() && !type.isSimpleBitVector())
		return type.getArrayElementType()->getBitstreamWidth();
	return 1;
}

static bool extract_static_select(VariableBits container, const ast::Expression &container_expr,
		int64_t first_index, int64_t last_index, uint64_t width, VariableBits &selected)
{
	if (!container_expr.type->hasFixedRange())
		return false;

	auto range = container_expr.type->getFixedRange();
	int64_t first = range.translateIndex((int)first_index);
	int64_t last = range.translateIndex((int)last_index);
	int stride = select_stride(container_expr);
	int64_t base = std::min(first, last) * stride;

	if (base < 0 || base + (int64_t)width > (int64_t)container.bitwidth())
		return false;

	selected = container.extract(base, width);
	return true;
}

class LhsDomainClassifier {
public:
	LhsDomainClassifier(EvalContext &eval, ProcessWriteDomains &domains)
		: eval(eval), domains(domains)
	{}

	void classify(const ast::Expression &expr)
	{
		if (expr.kind == ast::ExpressionKind::Concatenation) {
			classify_concatenation(expr.as<ast::ConcatenationExpression>());
			return;
		}

		VariableBits bits;
		if (static_bits(expr, bits)) {
			domains.add_static(bits, expr.sourceRange);
			return;
		}

		classify_dynamic_or_unsupported(expr);
	}

private:
	EvalContext &eval;
	ProcessWriteDomains &domains;

	bool static_bits(const ast::Expression &expr, VariableBits &bits)
	{
		if (!expr.type->isFixedSize())
			return false;

		switch (expr.kind) {
		case ast::ExpressionKind::HierarchicalValue:
		case ast::ExpressionKind::NamedValue:
			return static_named_value(expr, bits);
		case ast::ExpressionKind::RangeSelect:
			return static_range_select(expr.as<ast::RangeSelectExpression>(), bits);
		case ast::ExpressionKind::ElementSelect:
			return static_element_select(expr.as<ast::ElementSelectExpression>(), bits);
		case ast::ExpressionKind::Concatenation:
			return static_concatenation(expr.as<ast::ConcatenationExpression>(), bits);
		case ast::ExpressionKind::MemberAccess:
			return static_member_access(expr.as<ast::MemberAccessExpression>(), bits);
		case ast::ExpressionKind::Conversion:
			return static_conversion(expr.as<ast::ConversionExpression>(), bits);
		default:
			return false;
		}
	}

	bool static_named_value(const ast::Expression &expr, VariableBits &bits)
	{
		const ast::ValueSymbol &symbol = expr.as<ast::ValueExpressionBase>().symbol;
		if (!ast::ValueSymbol::isKind(symbol.kind))
			return false;

		bits = Variable::from_symbol(&symbol);
		return true;
	}

	bool static_range_select(const ast::RangeSelectExpression &expr, VariableBits &bits)
	{
		VariableBits container;
		if (!static_bits(expr.value(), container))
			return false;

		int64_t left = 0, right = 0;
		switch (expr.getSelectionKind()) {
		case ast::RangeSelectionKind::Simple:
			if (!const_int(eval, expr.left(), left) || !const_int(eval, expr.right(), right))
				return false;
			break;
		case ast::RangeSelectionKind::IndexedUp: {
			int64_t count = 0;
			if (!const_int(eval, expr.left(), left) || !const_int(eval, expr.right(), count))
				return false;
			right = left + count - 1;
			break;
		}
		case ast::RangeSelectionKind::IndexedDown: {
			int64_t count = 0;
			if (!const_int(eval, expr.left(), left) || !const_int(eval, expr.right(), count))
				return false;
			right = left - count + 1;
			break;
		}
		}

		return extract_static_select(
				container, expr.value(), left, right, expr.type->getBitstreamWidth(), bits);
	}

	bool static_element_select(const ast::ElementSelectExpression &expr, VariableBits &bits)
	{
		VariableBits container;
		int64_t index = 0;
		if (!static_bits(expr.value(), container) || !const_int(eval, expr.selector(), index))
			return false;

		return extract_static_select(
				container, expr.value(), index, index, expr.type->getBitstreamWidth(), bits);
	}

	bool static_concatenation(const ast::ConcatenationExpression &expr, VariableBits &bits)
	{
		VariableBits result;

		for (auto it = expr.operands().rbegin(); it != expr.operands().rend(); it++) {
			VariableBits operand_bits;
			if (!static_bits(**it, operand_bits))
				return false;
			result.append(operand_bits);
		}

		bits = result;
		return true;
	}

	bool static_member_access(const ast::MemberAccessExpression &expr, VariableBits &bits)
	{
		VariableBits container;
		if (!static_bits(expr.value(), container))
			return false;

		if (expr.member.kind != ast::SymbolKind::Field)
			return false;

		const auto &field = expr.member.as<ast::FieldSymbol>();
		uint64_t offset = bitstream_member_offset(field);
		bits = container.extract(offset, expr.type->getBitstreamWidth());
		return true;
	}

	bool static_conversion(const ast::ConversionExpression &expr, VariableBits &bits)
	{
		if (expr.operand().kind == ast::ExpressionKind::Streaming)
			return false;

		const ast::Type &from = expr.operand().type->getCanonicalType();
		const ast::Type &to = expr.type->getCanonicalType();
		if (!to.isBitstreamType() || !from.isBitstreamType())
			return false;
		if (from.getBitstreamWidth() != to.getBitstreamWidth())
			return false;

		return static_bits(expr.operand(), bits);
	}

	void classify_dynamic_or_unsupported(const ast::Expression &expr)
	{
		if (!expr.type->isFixedSize()) {
			domains.add_unsupported("non-fixed-size lhs", expr.sourceRange);
			return;
		}

		switch (expr.kind) {
		case ast::ExpressionKind::ElementSelect:
			classify_dynamic_select(expr.as<ast::ElementSelectExpression>());
			return;
		case ast::ExpressionKind::RangeSelect:
			classify_dynamic_range(expr.as<ast::RangeSelectExpression>());
			return;
		case ast::ExpressionKind::Concatenation:
			classify_concatenation(expr.as<ast::ConcatenationExpression>());
			return;
		case ast::ExpressionKind::MemberAccess:
			classify(expr.as<ast::MemberAccessExpression>().value());
			return;
		case ast::ExpressionKind::Conversion:
			classify(expr.as<ast::ConversionExpression>().operand());
			return;
		default:
			domains.add_unsupported("unsupported lhs", expr.sourceRange);
			return;
		}
	}

	void classify_dynamic_select(const ast::ElementSelectExpression &expr)
	{
		if (expr.value().type->getCanonicalType().isUnpackedArray() ||
				eval.netlist.is_inferred_memory(expr.value())) {
			domains.add_unsupported("memory write", expr.sourceRange);
			return;
		}

		VariableBits container;
		if (static_bits(expr.value(), container) && add_dynamic_container(expr, container))
			return;

		// Nested dynamic writes need symbolic execution support before optimization.
		domains.add_unsupported("nested dynamic lhs", expr.sourceRange);
		classify(expr.value());
	}

	void classify_dynamic_range(const ast::RangeSelectExpression &expr)
	{
		VariableBits container;
		if (static_bits(expr.value(), container) && add_dynamic_container(expr, container))
			return;

		// Nested dynamic writes need symbolic execution support before optimization.
		domains.add_unsupported("nested dynamic lhs", expr.sourceRange);
		classify(expr.value());
	}

	bool add_dynamic_container(const ast::Expression &expr, VariableBits container)
	{
		Variable root;
		if (!single_real_root(container, root)) {
			domains.add_unsupported("dynamic write spans multiple roots", expr.sourceRange);
			return false;
		}

		domains.add_dynamic(container, expr.type->getBitstreamWidth(), expr.sourceRange);
		return true;
	}

	void classify_concatenation(const ast::ConcatenationExpression &expr)
	{
		for (auto operand : expr.operands())
			classify(*operand);
	}
};

std::vector<WriteDomainRecord> classify_lhs_write_domains(
		EvalContext &eval, const ast::Expression &expr)
{
	ProcessWriteDomains domains;
	LhsDomainClassifier classifier(eval, domains);
	classifier.classify(expr);
	domains.normalize();
	return domains.records;
}

const char *process_kind_name(ast::ProceduralBlockKind kind)
{
	switch (kind) {
	case ast::ProceduralBlockKind::Always:      return "always";
	case ast::ProceduralBlockKind::AlwaysComb:  return "always_comb";
	case ast::ProceduralBlockKind::AlwaysLatch: return "always_latch";
	case ast::ProceduralBlockKind::AlwaysFF:    return "always_ff";
	case ast::ProceduralBlockKind::Initial:     return "initial";
	case ast::ProceduralBlockKind::Final:       return "final";
	}
	return "unknown";
}

void log_write_domains(const ProcessWriteDomains &domains, RTLIL::IdString module_name,
		ast::ProceduralBlockKind process_kind)
{
	log("slang-write-domains begin module=%s kind=%s\n", log_id(module_name),
			process_kind_name(process_kind));
	for (auto &line : domains.format_lines())
		log("%s\n", line.c_str());
	log("slang-write-domains end module=%s kind=%s\n", log_id(module_name),
			process_kind_name(process_kind));
}

} // namespace slang_frontend
