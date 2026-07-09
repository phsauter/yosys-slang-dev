//
// Yosys slang frontend
//
// Copyright Martin Povišer <povik@cutebit.org>
// Distributed under the terms of the ISC license, see LICENSE
//

#include "slang/ast/Statement.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/text/SourceManager.h"

#include "kernel/bitpattern.h"

#include "cases.h"
#include "diag.h"
#include "process_lowering.h"
#include "statements.h"
#include "variables.h"
#include "write_domains.h"

#include <algorithm>
#include <vector>

namespace slang_frontend {

extern const slang::SourceManager *global_sourcemgr;

static std::string format_bits(const VariableBits &bits)
{
	std::string text;

	for (auto chunk : bits.chunks()) {
		if (!text.empty())
			text += ",";
		text += chunk.text();
	}

	return text;
}

static Yosys::pool<VariableBit> bit_pool(const VariableBits &bits)
{
	Yosys::pool<VariableBit> pool;

	for (auto bit : bits)
		pool.insert(bit);

	return pool;
}

static std::string format_source_range(slang::SourceRange range)
{
	auto sm = global_sourcemgr;
	if (!sm || !sm->isFileLoc(range.start()))
		return "<unknown>";

	std::string filename{sm->getFileName(range.start())};
	return Yosys::stringf("%s:%d.%d", filename.c_str(),
			(int)sm->getLineNumber(range.start()),
			(int)sm->getColumnNumber(range.start()));
}

struct SymbolicUpdate {
	int order = 0;
	std::string control;
	WriteDomainRecord target;
};

struct SymbolicUpdateCollection {
	std::vector<SymbolicUpdate> updates;

	bool has_unsupported() const
	{
		for (auto &update : updates) {
			if (update.target.kind == WriteDomainRecord::Unsupported)
				return true;
		}
		return false;
	}

	VariableBits covered_bits() const
	{
		VariableBits bits;

		for (auto &update : updates) {
			if (update.target.kind != WriteDomainRecord::Unsupported)
				bits.append(update.target.bits);
		}

		bits.sort_and_unify();
		return bits;
	}

	std::vector<std::string> format_lines() const
	{
		std::vector<std::string> lines;

		for (auto &update : updates) {
			lines.push_back("slang-symbolic-update #" + std::to_string(update.order) +
					" control=" + update.control + " " +
					format_write_domain_record(update.target));
		}

		return lines;
	}
};

struct UpdateMapRoot {
	std::string root;
	std::vector<SymbolicUpdate> updates;
};

struct GuardedUpdateMap {
	std::vector<UpdateMapRoot> roots;
	std::vector<SymbolicUpdate> unsupported;

	bool has_unsupported() const
	{
		return !unsupported.empty();
	}

	VariableBits covered_bits() const
	{
		VariableBits bits;

		for (auto &root : roots) {
			for (auto &update : root.updates)
				bits.append(update.target.bits);
		}

		bits.sort_and_unify();
		return bits;
	}

	std::vector<std::string> format_lines() const
	{
		std::vector<std::string> lines;

		for (auto &root : roots) {
			lines.push_back("slang-update-map root " + root.root);
			for (auto &update : root.updates) {
				lines.push_back("slang-update-map update root=" + root.root +
						" order=" + std::to_string(update.order) +
						" guard=" + update.control + " " +
						format_write_domain_record(update.target));
			}
		}

		for (auto &update : unsupported) {
			lines.push_back("slang-update-map unsupported order=" +
					std::to_string(update.order) + " guard=" + update.control +
					" " + format_write_domain_record(update.target));
		}

		return lines;
	}
};

static std::string root_name(const WriteDomainRecord &record)
{
	for (auto bit : record.bits) {
		if (bit.variable.kind != Variable::Dummy)
			return bit.variable.text();
	}

	return "<unsupported>";
}

static UpdateMapRoot &get_or_add_root(GuardedUpdateMap &map, const std::string &root_name)
{
	for (auto &root : map.roots) {
		if (root.root == root_name)
			return root;
	}

	map.roots.push_back(UpdateMapRoot{root_name, {}});
	return map.roots.back();
}

static GuardedUpdateMap build_guarded_update_map(const SymbolicUpdateCollection &updates)
{
	GuardedUpdateMap map;

	for (auto &update : updates.updates) {
		if (update.target.kind == WriteDomainRecord::Unsupported) {
			map.unsupported.push_back(update);
			continue;
		}

		get_or_add_root(map, root_name(update.target)).updates.push_back(update);
	}

	std::sort(map.roots.begin(), map.roots.end(),
			[](const UpdateMapRoot &a, const UpdateMapRoot &b) {
				return a.root < b.root;
			});

	return map;
}

static void log_symbolic_updates(const SymbolicUpdateCollection &updates,
		RTLIL::IdString module_name, ast::ProceduralBlockKind process_kind)
{
	log("slang-symbolic-updates begin module=%s kind=%s\n", log_id(module_name),
			process_kind_name(process_kind));
	for (auto &line : updates.format_lines())
		log("%s\n", line.c_str());
	log("slang-symbolic-updates end module=%s kind=%s\n", log_id(module_name),
			process_kind_name(process_kind));
}

static void log_guarded_update_map(const GuardedUpdateMap &map,
		RTLIL::IdString module_name, ast::ProceduralBlockKind process_kind)
{
	log("slang-update-map begin module=%s kind=%s\n", log_id(module_name),
			process_kind_name(process_kind));
	for (auto &line : map.format_lines())
		log("%s\n", line.c_str());
	log("slang-update-map end module=%s kind=%s\n", log_id(module_name),
			process_kind_name(process_kind));
}

// Shared bit-coverage comparison behind the developer cross-checks: `want`
// is the reference set, `have` the derived one; any mismatch is a frontend
// consistency bug, not a design problem.
static void check_covered_bits_match(const VariableBits &want, const VariableBits &have,
		const char *log_prefix, const char *error_text)
{
	Yosys::pool<VariableBit> want_pool = bit_pool(want);
	Yosys::pool<VariableBit> have_pool = bit_pool(have);
	VariableBits missing, extra;

	for (auto bit : want) {
		if (!have_pool.count(bit))
			missing.append(bit);
	}

	for (auto bit : have) {
		if (!want_pool.count(bit))
			extra.append(bit);
	}

	if (!missing.empty() || !extra.empty()) {
		log("%s missing %s\n", log_prefix, format_bits(missing).c_str());
		log("%s extra %s\n", log_prefix, format_bits(extra).c_str());
		log_error("%s\n", error_text);
	}
}

static void check_symbolic_updates_against_domains(
		const SymbolicUpdateCollection &updates, const ProcessWriteDomains &domains)
{
	if (updates.has_unsupported() || domains.has_unsupported()) {
		log("slang-symbolic-update check skipped unsupported\n");
		return;
	}

	check_covered_bits_match(domains.covered_bits(), updates.covered_bits(),
			"slang-symbolic-update",
			"symbolic update collection does not match write-domain discovery");
	log("slang-symbolic-update check matched write_domains\n");
}

static void check_update_map_against_symbolic_updates(
		const GuardedUpdateMap &map, const SymbolicUpdateCollection &updates)
{
	if (map.has_unsupported() || updates.has_unsupported()) {
		log("slang-update-map check skipped unsupported\n");
		return;
	}

	check_covered_bits_match(updates.covered_bits(), map.covered_bits(),
			"slang-update-map",
			"guarded update map does not match symbolic updates");
	log("slang-update-map check matched symbolic_updates\n");
}

static void check_update_map_against_domains(
		const GuardedUpdateMap &map, const ProcessWriteDomains &domains)
{
	if (map.has_unsupported() || domains.has_unsupported()) {
		log("slang-update-map domain check skipped unsupported\n");
		return;
	}

	check_covered_bits_match(domains.covered_bits(), map.covered_bits(),
			"slang-update-map domain",
			"guarded update map does not match write-domain discovery");
	log("slang-update-map domain check matched write_domains\n");
}

static bool has_nested_assignment(const ast::Expression &expr)
{
	if (expr.kind == ast::ExpressionKind::Assignment)
		return true;

	bool found = false;
	expr.visit(ast::makeVisitor([&](auto&, const ast::AssignmentExpression&) {
		found = true;
	}));
	return found;
}

static bool has_hierarchical_value(const ast::Expression &expr)
{
	if (expr.kind == ast::ExpressionKind::HierarchicalValue)
		return true;

	bool found = false;
	expr.visit(ast::makeVisitor([&](auto&, const ast::HierarchicalValueExpression&) {
		found = true;
	}));
	return found;
}

static bool lhs_shape_supported(EvalContext &eval, const ast::Expression &lhs)
{
	if (!lhs.type->isFixedSize())
		return false;

	switch (lhs.kind) {
	case ast::ExpressionKind::HierarchicalValue:
	case ast::ExpressionKind::NamedValue: {
		const ast::ValueSymbol &symbol = lhs.as<ast::ValueExpressionBase>().symbol;
		return !eval.netlist.is_inferred_memory(symbol);
	}
	case ast::ExpressionKind::ElementSelect: {
		const auto &select = lhs.as<ast::ElementSelectExpression>();
		if (!select.value().type->isBitstreamType() ||
				!select.value().type->hasFixedRange() ||
				eval.netlist.is_inferred_memory(select.value()))
			return false;
		return lhs_shape_supported(eval, select.value());
	}
	case ast::ExpressionKind::RangeSelect: {
		const auto &select = lhs.as<ast::RangeSelectExpression>();
		if (!select.value().type->isBitstreamType() ||
				!select.value().type->hasFixedRange())
			return false;
		return lhs_shape_supported(eval, lhs.as<ast::RangeSelectExpression>().value());
	}
	case ast::ExpressionKind::Concatenation:
		for (auto operand : lhs.as<ast::ConcatenationExpression>().operands()) {
			if (!lhs_shape_supported(eval, *operand))
				return false;
		}
		return true;
	case ast::ExpressionKind::MemberAccess: {
		const auto &access = lhs.as<ast::MemberAccessExpression>();
		return access.member.kind == ast::SymbolKind::Field &&
				lhs_shape_supported(eval, access.value());
	}
	case ast::ExpressionKind::Conversion: {
		const auto &conversion = lhs.as<ast::ConversionExpression>();
		if (conversion.operand().kind == ast::ExpressionKind::Streaming)
			return false;

		const ast::Type &from = conversion.operand().type->getCanonicalType();
		const ast::Type &to = conversion.type->getCanonicalType();
		return to.isBitstreamType() && from.isBitstreamType() &&
				from.getBitstreamWidth() == to.getBitstreamWidth() &&
				lhs_shape_supported(eval, conversion.operand());
	}
	default:
		return false;
	}
}

// The update plan is the single collected representation of a comb-like
// process: one side-effect-free AST walk gathers every assignment (with the
// conditions it executes under), the write-domain classification of each
// target, and whether the process fits the materializer's subset. The
// diagnostic views (write domains, symbolic updates, guarded update map) and
// the materializer all consume this plan; nothing walks the AST twice.
struct UpdatePlanNode {
	enum Kind {
		Assign,     // assignment statement (LHS/RHS via `assign`)
		If,         // conditional; guard = AND over `conds`
		Case,       // case statement (`case_stmt` for kind/dispatch)
		For,        // for loop, unrolled at materialization time
		LocalDecl,  // automatic variable declaration
		Scope       // statement block introducing an automatic scope
	} kind;

	const ast::AssignmentExpression *assign = nullptr;
	std::vector<const ast::Expression *> conds;
	const ast::CaseStatement *case_stmt = nullptr;
	const ast::ForLoopStatement *for_stmt = nullptr;
	const ast::VariableSymbol *local = nullptr;
	const ast::StatementBlockSymbol *scope = nullptr;

	struct CaseItem {
		std::vector<const ast::Expression *> exprs;
		std::vector<UpdatePlanNode> body;
	};

	std::vector<UpdatePlanNode> body;      // If: true branch; For/Scope: body
	std::vector<UpdatePlanNode> else_body; // If: false branch; Case: default
	std::vector<CaseItem> case_items;
};

struct UpdatePlan {
	std::vector<UpdatePlanNode> body;
	bool supported = true;
	std::string unsupported_reason;

	// Diagnostic views, built during the same walk.
	ProcessWriteDomains domains;
	SymbolicUpdateCollection symbolic;
};

class ProcessPlanBuilder
	: public ast::ASTVisitor<ProcessPlanBuilder, ast::VisitFlags::Statements> {
public:
	ProcessPlanBuilder(EvalContext &eval, UpdatePlan &plan)
		: eval(eval), plan(plan)
	{
		body_stack.push_back(&plan.body);
	}

	void handle(const ast::ExpressionStatement &stmt)
	{
		if (stmt.expr.kind != ast::ExpressionKind::Assignment) {
			reject("non-assignment expression statement");
			return;
		}

		const auto &assign = stmt.expr.as<ast::AssignmentExpression>();
		record_expr_assignments(stmt.expr);

		if (assign.isCompound() ||
				has_nested_assignment(assign.right()) ||
				has_hierarchical_value(assign.right()) ||
				has_hierarchical_value(assign.left()) ||
				!lhs_shape_supported(eval, assign.left()))
			reject("unsupported assignment shape");

		UpdatePlanNode node{UpdatePlanNode::Assign};
		node.assign = &assign;
		emit(std::move(node));
	}

	void handle(const ast::BlockStatement &stmt)
	{
		EnterAutomaticScopeGuard guard(eval, stmt.blockSymbol);
		UpdatePlanNode node{UpdatePlanNode::Scope};
		node.scope = stmt.blockSymbol;
		visit_into(node.body, stmt.body);
		emit(std::move(node));
	}

	void handle(const ast::StatementList &list)
	{
		for (auto stmt : list.list)
			stmt->visit(*this);
	}

	void handle(const ast::ConditionalStatement &stmt)
	{
		UpdatePlanNode node{UpdatePlanNode::If};

		for (auto &condition : stmt.conditions) {
			if (condition.expr) {
				record_expr_assignments(*condition.expr);
				if (condition.pattern || has_nested_assignment(*condition.expr) ||
						has_hierarchical_value(*condition.expr))
					reject("unsupported conditional shape");
				node.conds.push_back(condition.expr);
			} else {
				reject("unsupported conditional shape");
			}
		}

		push_control("if-true");
		visit_into(node.body, stmt.ifTrue);
		pop_control();

		if (stmt.ifFalse) {
			push_control("if-false");
			visit_into(node.else_body, *stmt.ifFalse);
			pop_control();
		}

		emit(std::move(node));
	}

	void handle(const ast::CaseStatement &stmt)
	{
		UpdatePlanNode node{UpdatePlanNode::Case};
		node.case_stmt = &stmt;

		record_expr_assignments(stmt.expr);
		if (has_nested_assignment(stmt.expr) || has_hierarchical_value(stmt.expr))
			reject("unsupported case dispatch");

		for (auto item : stmt.items) {
			UpdatePlanNode::CaseItem plan_item;
			for (auto expr : item.expressions) {
				if (!expr) {
					reject("case item missing expression");
					continue;
				}
				record_expr_assignments(*expr);
				if (has_nested_assignment(*expr) || has_hierarchical_value(*expr))
					reject("unsupported case item expression");
				plan_item.exprs.push_back(expr);
			}
			push_control("case");
			visit_into(plan_item.body, *item.stmt);
			pop_control();
			node.case_items.push_back(std::move(plan_item));
		}

		if (stmt.defaultCase) {
			push_control("case-default");
			visit_into(node.else_body, *stmt.defaultCase);
			pop_control();
		}

		emit(std::move(node));
	}

	void handle(const ast::ForLoopStatement &stmt)
	{
		UpdatePlanNode node{UpdatePlanNode::For};
		node.for_stmt = &stmt;

		for (auto init : stmt.initializers) {
			record_expr_assignments(*init);
			if (init->kind == ast::ExpressionKind::Assignment &&
					!lhs_shape_supported(eval,
							init->as<ast::AssignmentExpression>().left()))
				reject("unsupported for-loop initializer");
		}

		if (!stmt.stopExpr || has_hierarchical_value(*stmt.stopExpr))
			reject("unsupported for-loop stop expression");
		else
			record_expr_assignments(*stmt.stopExpr);

		push_control("for");
		visit_into(node.body, stmt.body);
		pop_control();

		for (auto step : stmt.steps) {
			record_expr_assignments(*step);
			if (step->kind == ast::ExpressionKind::Assignment &&
					!lhs_shape_supported(eval,
							step->as<ast::AssignmentExpression>().left()))
				reject("unsupported for-loop step");
		}

		emit(std::move(node));
	}

	// Constructs outside the materializer's subset: still walked so the
	// diagnostic views stay complete, but the plan is marked unsupported.
	void handle(const ast::WhileLoopStatement &stmt)
	{
		reject("while loop");
		record_expr_assignments(stmt.cond);
		push_control("while");
		stmt.body.visit(*this);
		pop_control();
	}

	void handle(const ast::ForeachLoopStatement &stmt)
	{
		reject("foreach loop");
		push_control("foreach");
		stmt.body.visit(*this);
		pop_control();
	}

	void handle(const ast::TimedStatement &stmt)
	{
		reject("timed statement");
		stmt.stmt.visit(*this);
	}

	void handle(const ast::ReturnStatement &stmt)
	{
		reject("return statement");
		if (stmt.expr)
			record_expr_assignments(*stmt.expr);
	}

	void handle(const ast::EmptyStatement &) {}

	void handle(const ast::VariableDeclStatement &stmt)
	{
		UpdatePlanNode node{UpdatePlanNode::LocalDecl};
		node.local = &stmt.symbol;
		emit(std::move(node));
	}

	void handle(const ast::BreakStatement &) { reject("break statement"); }
	void handle(const ast::ContinueStatement &) { reject("continue statement"); }
	void handle(const ast::WaitStatement &) { reject("wait statement"); }
	void handle(const ast::Statement &) { reject("unsupported statement kind"); }
	void handle(const ast::Expression &) {}

private:
	EvalContext &eval;
	UpdatePlan &plan;
	std::vector<std::vector<UpdatePlanNode> *> body_stack;
	std::vector<std::string> control_stack;

	void emit(UpdatePlanNode node)
	{
		body_stack.back()->push_back(std::move(node));
	}

	void visit_into(std::vector<UpdatePlanNode> &body, const ast::Statement &stmt)
	{
		body_stack.push_back(&body);
		stmt.visit(*this);
		body_stack.pop_back();
	}

	void reject(const char *reason)
	{
		if (plan.supported) {
			plan.supported = false;
			plan.unsupported_reason = reason;
		}
	}

	void push_control(std::string name)
	{
		control_stack.push_back(std::move(name));
	}

	void pop_control()
	{
		log_assert(!control_stack.empty());
		control_stack.pop_back();
	}

	std::string control_path() const
	{
		if (control_stack.empty())
			return "root";

		std::string path;
		for (auto &item : control_stack) {
			if (!path.empty())
				path += "/";
			path += item;
		}
		return path;
	}

	// Record the write targets of every assignment nested in `expr`, in
	// execution order (RHS updates before the enclosing assignment's own).
	void record_expr_assignments(const ast::Expression &expr)
	{
		if (expr.kind != ast::ExpressionKind::Assignment)
			return;

		const auto &assign = expr.as<ast::AssignmentExpression>();
		record_expr_assignments(assign.right());

		for (auto &target : classify_lhs_write_domains(eval, assign.left())) {
			plan.domains.records.push_back(target);

			SymbolicUpdate update;
			update.order = plan.symbolic.updates.size();
			update.control = control_path();
			update.target = target;
			plan.symbolic.updates.push_back(update);
		}
	}
};

static UpdatePlan build_update_plan(EvalContext &eval, const ast::Statement &body)
{
	UpdatePlan plan;
	ProcessPlanBuilder builder(eval, plan);
	body.visit(builder);
	return plan;
}

// Materializes a supported update plan into a single continuous driver.
// This consumes only the collected plan; the AST is reached exclusively
// through the expression references the plan carries.
class PlanMaterializer {
public:
	PlanMaterializer(NetlistContext &netlist)
		: netlist(netlist), procedure(netlist, ProcessTiming::implicit),
		  eval(procedure.eval), unroll_limit(procedure.unroll_limit)
	{}

	bool run(const UpdatePlan &plan, VariableBits &driven, RTLIL::SigSpec &value)
	{
		materialize_body(plan.body);
		if (!ok) {
			update_events.clear();
			return false;
		}

		flush_update_events();
		driven = static_only(driven_bits);
		value = procedure.vstate.evaluate(netlist, driven);

		// Bits that stayed x-filled elaborate as don't-cares (deliberate:
		// gives the optimizer freedom where the classic path would keep
		// process semantics). Surface it for developers chasing
		// simulation/synthesis mismatches on incompletely assigned comb
		// processes.
		int x_bits = 0;
		for (int i = 0; i < value.size(); i++) {
			if (value[i] == RTLIL::Sx)
				x_bits++;
		}
		if (x_bits > 0)
			log_debug("slang-update-map module=%s driven_bits=%d dont_care_bits=%d\n",
					log_id(netlist.canvas->name), value.size(), x_bits);
		return true;
	}

private:
	void materialize_body(const std::vector<UpdatePlanNode> &body)
	{
		for (auto &node : body) {
			if (!ok)
				return;
			materialize_node(node);
		}
	}

	void materialize_node(const UpdatePlanNode &node)
	{
		switch (node.kind) {
		case UpdatePlanNode::Assign:
			apply_assignment(*node.assign);
			return;
		case UpdatePlanNode::Scope: {
			EnterAutomaticScopeGuard guard(eval, node.scope);
			materialize_body(node.body);
			return;
		}
		case UpdatePlanNode::If:
			materialize_if(node);
			return;
		case UpdatePlanNode::Case:
			materialize_case(node);
			return;
		case UpdatePlanNode::For:
			materialize_for(node);
			return;
		case UpdatePlanNode::LocalDecl:
			materialize_local(*node.local);
			return;
		}
	}

	void materialize_if(const UpdatePlanNode &node)
	{
		// Multiple conditions are `&&&` chains; without patterns (which the
		// plan builder rejects) they conjoin like plain AND.
		RTLIL::SigSpec cond = RTLIL::S1;
		for (auto expr : node.conds) {
			if (expression_reads_pending(*expr))
				flush_update_events();
			cond = and_guard(cond, netlist.ReduceBool(eval(*expr)));
		}

		visit_guarded(cond, node.body);
		if (!node.else_body.empty())
			visit_guarded(invert_guard(cond), node.else_body);
	}

	void materialize_case(const UpdatePlanNode &node)
	{
		const ast::CaseStatement &stmt = *node.case_stmt;
		using Condition = ast::CaseStatementCondition;

		if (expression_reads_pending(stmt.expr))
			flush_update_events();

		RTLIL::SigSpec dispatch = eval(stmt.expr);
		bool match_x = stmt.condition == Condition::WildcardXOrZ;
		bool match_z = stmt.condition == Condition::WildcardJustZ || match_x;

		// Evaluate every item compare once up front. Constant compares both
		// feed the parallelism check and the match cells below.
		std::vector<std::vector<RTLIL::SigSpec>> compares(node.case_items.size());
		std::vector<RTLIL::SigSpec> matches(node.case_items.size(), RTLIL::S0);
		bool comparable = stmt.condition != Condition::Inside;

		for (size_t i = 0; i < node.case_items.size(); i++) {
			for (auto expr : node.case_items[i].exprs) {
				if (expression_reads_pending(*expr))
					flush_update_events();

				if (!comparable) {
					require(stmt, stmt.expr.type->isIntegral());
					matches[i] = or_guard(matches[i],
							netlist.ReduceBool(inside_comparison(eval, dispatch, *expr)));
					continue;
				}

				RTLIL::SigSpec compare = eval(*expr);
				if (compare.size() != dispatch.size()) {
					fail("case item width mismatch");
					return;
				}
				compares[i].push_back(compare);
			}
			if (!ok)
				return;
		}

		// When every compare is constant and the items are pairwise
		// disjoint, at most one item can match: the priority ladder (each
		// guard carrying the negation of all earlier matches) is redundant
		// and only deepens the control cones. Guard by the raw matches.
		bool parallel = comparable && case_items_parallel(compares, match_x, match_z);

		if (comparable) {
			for (size_t i = 0; i < node.case_items.size(); i++) {
				for (auto &compare : compares[i]) {
					RTLIL::SigSpec match = (match_x || match_z)
							? wildcard_case_match(dispatch, compare, match_x, match_z)
							: exact_eq(dispatch, compare);
					matches[i] = or_guard(matches[i], match);
					if (!ok)
						return;
				}
			}
		}

		// Writes from distinct items (or the default) of a parallel case are
		// tagged so the flush can recognize their guards as pairwise disjoint
		// and fold them into one flat $pmux. A non-parallel case clears the
		// tags: its item guards overlap, so any enclosing parallel item that
		// writes through it produces duplicate slots and demotes to a chain.
		RTLIL::SigSpec prior_match = RTLIL::S0;
		int par_case_id = parallel ? parallel_case_counter++ : -1;
		int saved_par_case = active_par_case;
		int saved_par_slot = active_par_slot;
		for (size_t i = 0; i < node.case_items.size(); i++) {
			RTLIL::SigSpec guard = parallel ? matches[i]
					: and_guard(matches[i], invert_guard(prior_match));
			active_par_case = par_case_id;
			active_par_slot = parallel ? (int)i : -1;
			visit_guarded(guard, node.case_items[i].body);
			prior_match = or_guard(prior_match, matches[i]);
		}

		if (!node.else_body.empty()) {
			active_par_case = par_case_id;
			active_par_slot = parallel ? (int)node.case_items.size() : -1;
			visit_guarded(invert_guard(prior_match), node.else_body);
		}
		active_par_case = saved_par_case;
		active_par_slot = saved_par_slot;
	}

	// True when all compares are constants and pairwise provably disjoint
	// under the case kind's wildcard rules (some position where both are
	// definite 0/1 and differ). Conservative: any non-constant compare or a
	// non-wildcard x/z bit disqualifies.
	bool case_items_parallel(const std::vector<std::vector<RTLIL::SigSpec>> &compares,
			bool match_x, bool match_z)
	{
		std::vector<const RTLIL::SigSpec *> consts;
		for (auto &item : compares) {
			for (auto &compare : item) {
				if (!compare.is_fully_const())
					return false;
				for (int i = 0; i < compare.size(); i++) {
					RTLIL::SigBit bit = compare[i];
					if (bit != RTLIL::S0 && bit != RTLIL::S1 &&
							!is_case_wildcard(bit, match_x, match_z))
						return false;
				}
				consts.push_back(&compare);
			}
		}

		for (size_t a = 0; a < consts.size(); a++) {
			for (size_t b = a + 1; b < consts.size(); b++) {
				bool disjoint = false;
				for (int i = 0; i < consts[a]->size() && !disjoint; i++) {
					RTLIL::SigBit ba = (*consts[a])[i];
					RTLIL::SigBit bb = (*consts[b])[i];
					if (is_case_wildcard(ba, match_x, match_z) ||
							is_case_wildcard(bb, match_x, match_z))
						continue;
					disjoint = ba != bb;
				}
				if (!disjoint)
					return false;
			}
		}

		return true;
	}

	void materialize_for(const UpdatePlanNode &node)
	{
		const ast::ForLoopStatement &stmt = *node.for_stmt;

		for (auto init : stmt.initializers) {
			if (expression_reads_pending(*init))
				flush_update_events();
			eval(*init);
		}

		if (!stmt.stopExpr) {
			fail("for-loop missing stop expression");
			return;
		}

		unroll_limit.enter_unrolling();
		while (ok) {
			if (expression_reads_pending(*stmt.stopExpr))
				flush_update_events();

			RTLIL::SigSpec cond = netlist.ReduceBool(eval(*stmt.stopExpr));
			if (!cond.is_fully_const()) {
				fail("for-loop stop expression is not constant during unroll");
				break;
			}

			if (!cond.as_bool())
				break;

			materialize_body(node.body);

			for (auto step : stmt.steps) {
				if (expression_reads_pending(*step))
					flush_update_events();
				eval(*step);
			}

			if (!unroll_limit.unroll_tick(&stmt))
				break;
		}
		unroll_limit.exit_unrolling();
	}

	void materialize_local(const ast::VariableSymbol &symbol)
	{
		if (symbol.lifetime == ast::VariableLifetime::Static)
			return;

		Variable target = eval.variable(symbol);
		if (!target.bitwidth())
			return;

		RTLIL::SigSpec initval;
		if (symbol.getInitializer()) {
			if (expression_reads_pending(*symbol.getInitializer()))
				flush_update_events();
			initval = eval(*symbol.getInitializer());
		} else {
			auto converted =
					netlist.convert_const(symbol.getType().getDefaultValue(), symbol.location);
			initval = converted ? *converted : RTLIL::SigSpec(RTLIL::Sx, (int)target.bitwidth());
		}

		procedure.do_simple_assign(symbol.location, target, initval, true);
	}

	NetlistContext &netlist;
	ProceduralContext procedure;
	EvalContext &eval;
	UnrollLimitTracking &unroll_limit;
	RTLIL::SigSpec current_guard = RTLIL::S1;
	VariableBits driven_bits;
	bool ok = true;
	int parallel_case_counter = 0;
	int active_par_case = -1;
	int active_par_slot = -1;

	struct UpdateEvent {
		int order = 0;
		VariableBits bits;
		RTLIL::SigSpec value;
		RTLIL::SigSpec mask;
		RTLIL::SigSpec shape_mask;
		// Provenance of a whole-word guarded write: writes from distinct
		// items of one parallel case carry the same par_case with distinct
		// par_slot values and are pairwise disjoint by construction.
		int par_case = -1;
		int par_slot = -1;
		// Whole-word merges stack here (coalescing time) instead of folding
		// into `value` immediately; the flush picks between a priority chain
		// and a single flat $pmux. While the stack is non-empty, `value` and
		// `mask` still describe only the first write.
		struct WordWrite {
			RTLIL::SigBit guard;
			RTLIL::SigSpec value;
			int par_case;
			int par_slot;
		};
		std::vector<WordWrite> word_writes;
	};

	std::vector<UpdateEvent> update_events;
	// Index of variables with pending updates: Variable identity plus its
	// text form, because automatic variables of reentrant scopes can appear
	// as distinct Variable instances for the same underlying storage.
	Yosys::pool<Variable> pending_variables;
	Yosys::pool<std::string> pending_variable_texts;
	int next_order = 0;

	void fail(const char *reason)
	{
		if (ok)
			log("slang-update-map materialize fail module=%s reason=%s\n",
					log_id(netlist.canvas->name), reason);
		ok = false;
	}

	void fail_lhs(const char *reason, const ast::Expression &lhs)
	{
		if (ok)
			log("slang-update-map materialize fail module=%s reason=%s lhs_kind=%d lhs_width=%d loc=%s\n",
					log_id(netlist.canvas->name), reason, (int)lhs.kind,
					(int)lhs.type->getBitstreamWidth(),
					format_source_range(lhs.sourceRange).c_str());
		ok = false;
	}

	VariableBits static_only(VariableBits bits)
	{
		bits.sort_and_unify();

		VariableBits filtered;
		for (auto chunk : bits.chunks()) {
			if (is_persistent_static(chunk.variable))
				filtered.append(VariableBits(chunk));
		}
		return filtered;
	}

	bool is_persistent_static(const Variable &variable)
	{
		if (variable.kind != Variable::Static)
			return false;

		const ast::Symbol *symbol = variable.get_symbol();
		return symbol && netlist.wire_cache.count(symbol);
	}

	bool bits_overlap(const VariableBits &lhs, const VariableBits &rhs)
	{
		Yosys::pool<VariableBit> pool = bit_pool(lhs);
		for (auto bit : rhs) {
			if (pool.count(bit))
				return true;
		}
		return false;
	}

	bool mask_bit_active(RTLIL::SigBit bit)
	{
		return bit != RTLIL::S0;
	}

	bool masks_are_disjoint(const RTLIL::SigSpec &old_mask, const RTLIL::SigSpec &new_mask)
	{
		log_assert(old_mask.size() == new_mask.size());

		// Syntactic zero positions are the disjointness certificate.
		for (int i = 0; i < old_mask.size(); i++)
			if (mask_bit_active(old_mask[i]) && mask_bit_active(new_mask[i]))
				return false;

		return true;
	}

	RTLIL::SigBit or_mask_bit(RTLIL::SigBit lhs, RTLIL::SigBit rhs)
	{
		if (lhs == RTLIL::S1 || rhs == RTLIL::S1)
			return RTLIL::S1;
		if (lhs == RTLIL::S0)
			return rhs;
		if (rhs == RTLIL::S0)
			return lhs;
		return netlist.LogicOr(RTLIL::SigSpec(lhs), RTLIL::SigSpec(rhs))[0];
	}

	// True when every bit of the mask is the same signal or constant.
	bool uniform_mask(const RTLIL::SigSpec &mask)
	{
		for (int i = 1; i < mask.size(); i++) {
			if (mask[i] != mask[0])
				return false;
		}
		return mask.size() > 0;
	}

	// Fold a deferred whole-word stack back into the first-write invariant:
	// later writes have priority, so each becomes one word-level mux, and the
	// mask accumulates the guards. This reproduces exactly what an eager
	// merge would have built.
	void materialize_word_writes(UpdateEvent &event)
	{
		for (auto &ww : event.word_writes) {
			event.value = netlist.Mux(event.value, ww.value, RTLIL::SigSpec(ww.guard));
			event.mask = RTLIL::SigSpec(or_mask_bit(event.mask[0], ww.guard),
					event.mask.size());
		}
		event.word_writes.clear();
	}

	void merge_priority_update(UpdateEvent &event, const UpdateEvent &incoming)
	{
		const RTLIL::SigSpec &value = incoming.value;
		const RTLIL::SigSpec &mask = incoming.mask;
		const RTLIL::SigSpec &shape_mask = incoming.shape_mask;
		log_assert(incoming.word_writes.empty());
		log_assert(event.value.size() == value.size());
		log_assert(event.mask.size() == mask.size());
		log_assert(event.shape_mask.size() == shape_mask.size());

		// Whole-word writes under uniform guards (the case/if pattern) stay
		// word-level; dissolving them into per-bit muxes would destroy the
		// bus structure for downstream passes. The merge itself is deferred:
		// the flush folds the stack into a priority chain, or into a single
		// flat $pmux when the guards are provably disjoint.
		if (shape_mask.is_fully_ones() && event.shape_mask.is_fully_ones() &&
				uniform_mask(mask) && uniform_mask(event.mask)) {
			event.word_writes.push_back({mask[0], value,
					incoming.par_case, incoming.par_slot});
			return;
		}

		materialize_word_writes(event);
		for (int i = 0; i < shape_mask.size(); i++) {
			if (!mask_bit_active(shape_mask[i]))
				continue;

			if (!mask_bit_active(event.shape_mask[i])) {
				event.value[i] = value[i];
				event.mask[i] = mask[i];
			} else if (mask[i] == RTLIL::S1) {
				event.value[i] = value[i];
				event.mask[i] = RTLIL::S1;
			} else if (event.mask[i] == RTLIL::S0) {
				event.value[i] = value[i];
				event.mask[i] = mask[i];
			} else {
				// Later assignments have priority when their write mask is active.
				event.value[i] = netlist.Mux(
						RTLIL::SigSpec(event.value[i]),
						RTLIL::SigSpec(value[i]),
						RTLIL::SigSpec(mask[i]))[0];
				event.mask[i] = or_mask_bit(event.mask[i], mask[i]);
			}

			event.shape_mask[i] = RTLIL::S1;
		}
	}

	bool is_unconditional_cover(const UpdateEvent &event)
	{
		return event.mask.is_fully_ones() && event.shape_mask.is_fully_ones();
	}

	void overlay_disjoint_update(UpdateEvent &event,
			RTLIL::SigSpec value, RTLIL::SigSpec mask, RTLIL::SigSpec shape_mask)
	{
		log_assert(event.value.size() == value.size());
		log_assert(event.mask.size() == mask.size());
		log_assert(event.shape_mask.size() == shape_mask.size());

		for (int i = 0; i < shape_mask.size(); i++) {
			if (!mask_bit_active(shape_mask[i]))
				continue;

			event.value[i] = value[i];
			event.mask[i] = mask[i];
			event.shape_mask[i] = shape_mask[i];
		}
	}

	bool try_coalesce_event(std::vector<UpdateEvent> &events, const UpdateEvent &incoming)
	{
		for (int i = (int)events.size() - 1; i >= 0; i--) {
			if (events[i].bits != incoming.bits) {
				// A later event over a different but overlapping bit set
				// flushes after events[i]; merging the incoming update into
				// events[i] would let that event clobber it. Order must be
				// preserved, so coalescing can only skip past disjoint events.
				if (bits_overlap(events[i].bits, incoming.bits))
					return false;
				continue;
			}

			if (masks_are_disjoint(events[i].shape_mask, incoming.shape_mask)) {
				// Disjoint writes commute, so the later order can extend the
				// group. (A deferred whole-word stack implies a fully-ones
				// shape, which can never be disjoint from an active incoming
				// mask, but keep the first-write invariant regardless.)
				materialize_word_writes(events[i]);
				overlay_disjoint_update(events[i],
						incoming.value, incoming.mask, incoming.shape_mask);
			} else {
				if (is_unconditional_cover(events[i]) && !incoming.mask.is_fully_ones())
					return false;

				merge_priority_update(events[i], incoming);
			}
			events[i].order = incoming.order;
			return true;
		}

		return false;
	}

	std::vector<UpdateEvent> coalesced_events()
	{
		std::vector<UpdateEvent> events;
		for (auto &event : update_events) {
			if (!try_coalesce_event(events, event))
				events.push_back(event);
		}
		return events;
	}

	RTLIL::SigSpec fallback_value(VariableBits bits)
	{
		RTLIL::SigSpec fallback = procedure.vstate.evaluate(netlist, bits);
		int offset = 0;
		for (auto bit : bits) {
			if (!procedure.vstate.visible_assignments.count(bit))
				fallback[offset] = RTLIL::Sx;
			offset++;
		}
		return fallback;
	}

	// Whole-word writes from distinct items (or the default) of one parallel
	// case have pairwise-disjoint guards by construction: each guard is a
	// conjunction containing its item's match, and the matches are disjoint
	// constants. At most one can be active, so the stack folds into a single
	// flat $pmux over the fallback instead of a priority chain.
	RTLIL::SigSpec try_parallel_pmux(const UpdateEvent &event)
	{
		// A/B kill-switch: fall back to the priority chain unconditionally.
		static const bool disabled = getenv("SLANG_UPDATE_MAP_NO_PMUX") != nullptr;
		if (disabled || event.par_case < 0 || event.mask[0] == RTLIL::S1)
			return {};

		Yosys::pool<int> slots;
		slots.insert(event.par_slot);
		for (auto &ww : event.word_writes) {
			// A repeated slot means two writes under the same item guard:
			// order matters there, so demote to the chain.
			if (ww.par_case != event.par_case || ww.guard == RTLIL::S1 ||
					!slots.insert(ww.par_slot).second)
				return {};
		}

		RTLIL::SigSpec values = event.value;
		RTLIL::SigSpec guards = RTLIL::SigSpec(event.mask[0]);
		for (auto &ww : event.word_writes) {
			values.append(ww.value);
			guards.append(ww.guard);
		}
		return netlist.Pmux(fallback_value(event.bits), values, guards);
	}

	void flush_update_events()
	{
		for (auto &event : coalesced_events()) {
			RTLIL::SigSpec merged;
			if (!event.word_writes.empty())
				merged = try_parallel_pmux(event);
			if (merged.empty()) {
				materialize_word_writes(event);
				if (event.mask.is_fully_ones()) {
					// Unconditional full write: no fallback needed at all.
					merged = event.value;
				} else if (uniform_mask(event.mask) && event.mask[0].wire != nullptr) {
					// Single-condition write: keep the word-level mux structure
					// instead of dissolving the bus into bitwise masks.
					merged = netlist.Mux(fallback_value(event.bits), event.value,
							RTLIL::SigSpec(event.mask[0]));
				} else {
					merged = netlist.Bwmux(fallback_value(event.bits), event.value,
							event.mask);
				}
			}
			procedure.vstate.set(event.bits, merged);
			driven_bits.append(event.bits);
		}

		update_events.clear();
		pending_variables.clear();
		pending_variable_texts.clear();
	}

	bool pending_reads_value(const ast::ValueSymbol &symbol)
	{
		Variable variable = eval.variable(symbol);
		if (!variable)
			return false;

		return pending_variables.count(variable) ||
				pending_variable_texts.count(variable.text());
	}

	bool expression_reads_pending(const ast::Expression &expr)
	{
		if (update_events.empty())
			return false;

		bool found = false;
		expr.visit(ast::makeVisitor([&](auto&, const ast::Expression &node) {
			if (node.kind != ast::ExpressionKind::NamedValue)
				return;

			const auto &value = node.as<ast::ValueExpressionBase>();
			if (ast::ValueSymbol::isKind(value.symbol.kind) &&
					pending_reads_value(value.symbol.as<ast::ValueSymbol>()))
				found = true;
		}));
		return found;
	}

	RTLIL::SigSpec and_guard(RTLIL::SigSpec lhs, RTLIL::SigSpec rhs)
	{
		if (lhs.is_fully_zero() || rhs.is_fully_zero())
			return RTLIL::S0;
		if (lhs.is_fully_ones())
			return rhs;
		if (rhs.is_fully_ones())
			return lhs;
		return netlist.LogicAnd(lhs, rhs);
	}

	RTLIL::SigSpec or_guard(RTLIL::SigSpec lhs, RTLIL::SigSpec rhs)
	{
		if (lhs.is_fully_ones() || rhs.is_fully_ones())
			return RTLIL::S1;
		if (lhs.is_fully_zero())
			return rhs;
		if (rhs.is_fully_zero())
			return lhs;
		return netlist.LogicOr(lhs, rhs);
	}

	RTLIL::SigSpec invert_guard(RTLIL::SigSpec guard)
	{
		if (guard.is_fully_zero())
			return RTLIL::S1;
		if (guard.is_fully_ones())
			return RTLIL::S0;
		return netlist.LogicNot(guard);
	}

	void visit_guarded(RTLIL::SigSpec guard, const std::vector<UpdatePlanNode> &body)
	{
		RTLIL::SigSpec saved = current_guard;
		current_guard = and_guard(current_guard, guard);
		materialize_body(body);
		current_guard = saved;
	}

	RTLIL::SigSpec exact_eq(RTLIL::SigSpec a, RTLIL::SigSpec b)
	{
		// A fully-defined constant compare behaves identically under $eq,
		// which downstream passes handle better than $eqx.
		bool defined = b.is_fully_const() && b.is_fully_def();
		return netlist.Biop(defined ? ID($eq) : ID($eqx), a, b, false, false, 1);
	}

	bool is_case_wildcard(RTLIL::SigBit bit, bool match_x, bool match_z)
	{
		if (bit == RTLIL::Sa)
			return true;
		if (match_z && bit == RTLIL::Sz)
			return true;
		if (match_x && bit == RTLIL::Sx)
			return true;
		return false;
	}

	RTLIL::SigSpec wildcard_case_match(RTLIL::SigSpec dispatch,
			RTLIL::SigSpec compare, bool match_x, bool match_z)
	{
		if (!compare.is_fully_const()) {
			fail("wildcard case item is not constant");
			return RTLIL::S0;
		}

		RTLIL::SigSpec checked_dispatch;
		RTLIL::SigSpec checked_compare;
		for (int i = 0; i < compare.size(); i++) {
			if (is_case_wildcard(compare[i], match_x, match_z))
				continue;

			checked_dispatch.append(dispatch[i]);
			checked_compare.append(compare[i]);
		}

		if (checked_compare.empty())
			return RTLIL::S1;

		return exact_eq(checked_dispatch, checked_compare);
	}

	// Selector expressions on the LHS are reads: `vec[idx] = x` must see a
	// pending update to `idx`. The write target itself is not a read, so only
	// the select/index subtrees are checked.
	bool lhs_selectors_read_pending(const ast::Expression &lhs)
	{
		switch (lhs.kind) {
		case ast::ExpressionKind::ElementSelect: {
			const auto &select = lhs.as<ast::ElementSelectExpression>();
			return expression_reads_pending(select.selector()) ||
					lhs_selectors_read_pending(select.value());
		}
		case ast::ExpressionKind::RangeSelect: {
			const auto &select = lhs.as<ast::RangeSelectExpression>();
			return expression_reads_pending(select.left()) ||
					expression_reads_pending(select.right()) ||
					lhs_selectors_read_pending(select.value());
		}
		case ast::ExpressionKind::Concatenation:
			for (auto operand : lhs.as<ast::ConcatenationExpression>().operands()) {
				if (lhs_selectors_read_pending(*operand))
					return true;
			}
			return false;
		case ast::ExpressionKind::MemberAccess:
			return lhs_selectors_read_pending(lhs.as<ast::MemberAccessExpression>().value());
		case ast::ExpressionKind::Conversion:
			return lhs_selectors_read_pending(lhs.as<ast::ConversionExpression>().operand());
		default:
			return false;
		}
	}

	void apply_assignment(const ast::AssignmentExpression &assign)
	{
		if (assign.isCompound() || expression_reads_pending(assign.right()) ||
				lhs_selectors_read_pending(assign.left()))
			flush_update_events();

		const ast::Expression *old_lvalue = eval.lvalue;
		eval.lvalue = &assign.left();
		RTLIL::SigSpec rhs = eval(assign.right());
		eval.lvalue = old_lvalue;
		apply_lhs(assign.sourceRange.start(), assign.left(), rhs);
	}

	bool static_target(const ast::Expression &lhs, VariableBits &bits)
	{
		auto targets = classify_lhs_write_domains(eval, lhs);
		if (targets.size() != 1 || targets[0].kind != WriteDomainRecord::StaticBits)
			return false;

		bits = targets[0].bits;
		return bits.bitwidth() == lhs.type->getBitstreamWidth();
	}

	RTLIL::SigSpec guarded_mask(RTLIL::SigSpec mask)
	{
		if (current_guard.is_fully_const()) {
			return current_guard.as_bool() ? mask : RTLIL::SigSpec(RTLIL::S0, mask.size());
		}

		return netlist.Mux(RTLIL::SigSpec(RTLIL::S0, mask.size()), mask, current_guard);
	}

	RTLIL::SigSpec sparse_guarded_mask(RTLIL::SigSpec mask)
	{
		if (current_guard.is_fully_const())
			return current_guard.as_bool() ? mask : RTLIL::SigSpec(RTLIL::S0, mask.size());

		// One guard AND per DISTINCT mask bit, not per position: a uniform
		// mask (whole-word write under a condition, the common case on wide
		// buses) costs a single cell instead of one per bit. Zero positions
		// stay syntactic zeros so later disjointness checks stay cheap.
		Yosys::dict<RTLIL::SigBit, RTLIL::SigBit> guarded_bits;
		RTLIL::SigSpec guarded(RTLIL::S0, mask.size());
		for (int i = 0; i < mask.size(); i++) {
			if (!mask_bit_active(mask[i]))
				continue;

			RTLIL::SigBit bit = mask[i];
			auto it = guarded_bits.find(bit);
			if (it == guarded_bits.end()) {
				RTLIL::SigBit g = bit == RTLIL::S1
						? current_guard[0]
						: netlist.LogicAnd(current_guard, RTLIL::SigSpec(bit))[0];
				it = guarded_bits.insert(std::make_pair(bit, g)).first;
			}
			guarded[i] = it->second;
		}
		return guarded;
	}

	void apply_static(slang::SourceLocation loc, VariableBits bits, RTLIL::SigSpec rhs)
	{
		apply_masked(loc, bits, rhs, RTLIL::SigSpec(RTLIL::S1, rhs.size()));
	}

	void apply_masked(slang::SourceLocation loc, VariableBits bits,
			RTLIL::SigSpec rhs, RTLIL::SigSpec mask)
	{
		apply_masked(loc, bits, rhs, mask, mask);
	}

	// The shape mask only ever feeds activity bookkeeping (disjointness and
	// coalescing decisions via mask_bit_active), never the netlist, so the
	// guard doesn't need real cells here: a possibly-active bit under any
	// guard is simply "active" (S1). Zero positions stay syntactic zeros.
	RTLIL::SigSpec shape_activity(const RTLIL::SigSpec &shape_mask)
	{
		if (current_guard.is_fully_const() && !current_guard.as_bool())
			return RTLIL::SigSpec(RTLIL::S0, shape_mask.size());

		RTLIL::SigSpec activity(RTLIL::S0, shape_mask.size());
		for (int i = 0; i < shape_mask.size(); i++) {
			if (mask_bit_active(shape_mask[i]))
				activity[i] = RTLIL::S1;
		}
		return activity;
	}

	void apply_masked(slang::SourceLocation loc, VariableBits bits,
			RTLIL::SigSpec rhs, RTLIL::SigSpec mask, RTLIL::SigSpec shape_mask,
			bool pre_guarded = false)
	{
		(void)loc;
		RTLIL::SigSpec guarded = pre_guarded ? mask : sparse_guarded_mask(mask);
		RTLIL::SigSpec guarded_shape = shape_activity(shape_mask);
		if (guarded.is_fully_zero())
			return;

		// An unconditional full-width write makes every earlier pending
		// event whose bits it fully covers dead; drop them instead of
		// stacking mux layers for downstream opt to chew through.
		if (guarded.is_fully_ones() && guarded_shape.is_fully_ones()) {
			Yosys::pool<VariableBit> covered = bit_pool(bits);
			update_events.erase(
					std::remove_if(update_events.begin(), update_events.end(),
							[&](const UpdateEvent &event) {
								for (auto bit : event.bits) {
									if (!covered.count(bit))
										return false;
								}
								return true;
							}),
					update_events.end());
		}

		UpdateEvent event{next_order++, bits,
				RTLIL::SigSpec(RTLIL::Sx, bits.bitwidth()),
				RTLIL::SigSpec(RTLIL::S0, bits.bitwidth()),
				RTLIL::SigSpec(RTLIL::S0, bits.bitwidth())};
		event.par_case = active_par_case;
		event.par_slot = active_par_slot;
		overlay_disjoint_update(event, rhs, guarded, guarded_shape);
		update_events.push_back(event);

		for (auto chunk : bits.chunks()) {
			if (pending_variables.count(chunk.variable))
				continue;
			pending_variables.insert(chunk.variable);
			pending_variable_texts.insert(chunk.variable.text());
		}
	}

	void apply_dynamic(slang::SourceLocation loc, VariableBits container,
			AddressingResolver &resolver, RTLIL::SigSpec rhs)
	{
		RTLIL::SigSpec data = resolver.shift_up(rhs, true, container.bitwidth());
		// Distribute the guard through the decoder instead of ANDing it
		// onto every decoded mask bit afterwards.
		RTLIL::SigSpec mask = resolver.demux(
				RTLIL::SigSpec(current_guard[0], (int)rhs.size()), container.bitwidth());
		RTLIL::SigSpec shape = resolver.demux(
				RTLIL::SigSpec(RTLIL::S1, rhs.size()), container.bitwidth());

		apply_masked(loc, container, data, mask, shape, /* pre_guarded= */ true);
	}

	bool apply_aggregate_dynamic(slang::SourceLocation loc,
			const ast::Expression &lhs, RTLIL::SigSpec rhs)
	{
		std::optional<LValue> lvalue = LValue::analyze(eval, lhs, /* silent= */ true);
		if (!lvalue)
			return false;

		VariableBits base_lvalue;
		RTLIL::SigSpec base_rvalue;
		RTLIL::SigSpec base_mask;
		RTLIL::SigSpec base_shape_mask;
		bool has_dynamic_select = false;

		// Reuse the existing nested-select write-mask construction, seeding
		// it with the guard so decoders distribute it for free.
		RTLIL::SigSpec seed_mask(current_guard[0], (int)rhs.size());
		if (!expand_aggregate_write(*lvalue, rhs, seed_mask,
					base_lvalue, base_rvalue, base_mask, has_dynamic_select,
					&base_shape_mask))
			return false;

		(void)has_dynamic_select;
		if (base_lvalue.bitwidth() < rhs.size())
			return false;

		apply_masked(loc, base_lvalue, base_rvalue, base_mask, base_shape_mask,
				/* pre_guarded= */ true);
		return true;
	}

	void apply_lhs(slang::SourceLocation loc, const ast::Expression &lhs, RTLIL::SigSpec rhs)
	{
		VariableBits bits;
		if (static_target(lhs, bits)) {
			apply_static(loc, bits, rhs);
			return;
		}

		if (apply_aggregate_dynamic(loc, lhs, rhs))
			return;

		switch (lhs.kind) {
		case ast::ExpressionKind::ElementSelect:
			apply_element_select(loc, lhs.as<ast::ElementSelectExpression>(), rhs);
			return;
		case ast::ExpressionKind::RangeSelect:
			apply_range_select(loc, lhs.as<ast::RangeSelectExpression>(), rhs);
			return;
		default:
			fail_lhs("unsupported lhs after dynamic aggregate fallback", lhs);
			return;
		}
	}

	void apply_element_select(slang::SourceLocation loc,
			const ast::ElementSelectExpression &lhs, RTLIL::SigSpec rhs)
	{
		VariableBits container;
		if (!static_target(lhs.value(), container)) {
			fail_lhs("dynamic element base is not static", lhs.value());
			return;
		}

		AddressingResolver resolver(eval, lhs);
		apply_dynamic(loc, container, resolver, rhs);
	}

	void apply_range_select(slang::SourceLocation loc,
			const ast::RangeSelectExpression &lhs, RTLIL::SigSpec rhs)
	{
		VariableBits container;
		if (!static_target(lhs.value(), container)) {
			fail_lhs("dynamic range base is not static", lhs.value());
			return;
		}

		AddressingResolver resolver(eval, lhs);
		apply_dynamic(loc, container, resolver, rhs);
	}
};

static bool try_lower_with_update_map(NetlistContext &netlist, const UpdatePlan &plan)
{
	if (!plan.supported) {
		log("slang-update-map falling back to process lowering module=%s reason=%s\n",
				log_id(netlist.canvas->name), plan.unsupported_reason.c_str());
		return false;
	}

	size_t cell_count = netlist.canvas->cells_.size();

	PlanMaterializer materializer(netlist);
	VariableBits driven;
	RTLIL::SigSpec value;
	if (!materializer.run(plan, driven, value)) {
		// Some conditions (e.g. non-constant loop bounds) are only detectable
		// during materialization. Fall back to the classic lowering when the
		// attempt left no cells behind; leaked wires are harmless and get
		// swept by opt_clean. Once cells were emitted there is no clean
		// rollback, so that stays a hard error.
		if (netlist.canvas->cells_.size() == cell_count) {
			log("slang-update-map falling back to process lowering module=%s\n",
					log_id(netlist.canvas->name));
			return false;
		}
		log_error("update-map materialization failed after emitting cells\n");
	}

	netlist.add_continuous_driver(driven, value);
	return true;
}

static void check_discovery_did_not_emit(NetlistContext &netlist,
		size_t wire_count, size_t cell_count)
{
	if (netlist.canvas->wires_.size() != wire_count ||
			netlist.canvas->cells_.size() != cell_count) {
		log_error("write-domain discovery emitted RTLIL objects\n");
	}
}

static void check_write_domains_against_all_driven(
		const ProcessWriteDomains &domains, const VariableBits &all_driven)
{
	if (domains.has_unsupported()) {
		log("slang-write-domain check skipped unsupported\n");
		return;
	}

	VariableBits discovered = domains.covered_bits();
	Yosys::pool<VariableBit> discovered_pool = bit_pool(discovered);
	Yosys::pool<VariableBit> driven_pool = bit_pool(all_driven);
	VariableBits missing, extra;

	for (auto bit : all_driven) {
		if (!discovered_pool.count(bit))
			missing.append(bit);
	}

	for (auto bit : discovered) {
		if (!driven_pool.count(bit))
			extra.append(bit);
	}

	if (!missing.empty() || !extra.empty()) {
		log("slang-write-domain missing %s\n", format_bits(missing).c_str());
		log("slang-write-domain extra %s\n", format_bits(extra).c_str());
		log_error("write-domain discovery does not match procedural all_driven set\n");
	}

	log("slang-write-domain check matched all_driven\n");
}

static Yosys::pool<VariableBit> detect_possibly_unassigned_subset(
		Yosys::pool<VariableBit> &signals, Case *rule, int level=0)
{
	Yosys::pool<VariableBit> remaining = signals;
	bool debug = false;

	for (auto &action : rule->actions) {
		if (debug) {
			log_debug("%saction %s<=%s (mask %s)\n", std::string(level, ' ').c_str(),
					  "FIXME" /* log_signal(action.lvalue) */, log_signal(action.unmasked_rvalue),
					  log_signal(action.mask));
		}

		if (action.mask.is_fully_ones()) {
			for (auto bit : action.lvalue)
				remaining.erase(bit);
		}
	}

	for (auto switch_ : rule->switches) {
		if (debug)
			log_debug("%sswitch %s\n", std::string(level, ' ').c_str(), log_signal(switch_->signal));

		if (remaining.empty())
			break;

		Yosys::pool<VariableBit> new_remaining;
		Yosys::BitPatternPool pool(switch_->signal);
		for (auto case_ : switch_->cases) {
			if (!switch_->signal.empty() && pool.empty())
				break;

			if (debug) {
				log_debug("%s case ", std::string(level, ' ').c_str());
				for (auto compare : case_->compare)
					log_debug("%s ", log_signal(compare));
				log_debug("\n");
			}

			bool selectable = false;
			if (case_->compare.empty()) {
				// Default cases cover the remaining compare space.
				selectable = pool.take_all() || switch_->signal.empty();
			} else {
				for (auto compare : case_->compare) {
					if (!compare.is_fully_const()) {
						if (!pool.empty())
							selectable = true;
					} else if (pool.take(compare)) {
						selectable = true;
					}
				}
			}

			if (selectable) {
				for (auto bit : detect_possibly_unassigned_subset(remaining, case_, level + 2))
					new_remaining.insert(bit);
			}
		}

		if (switch_->full_case || pool.empty())
			remaining.swap(new_remaining);
	}

	return remaining;
}

// Developer diagnostics: the write-domain, symbolic-update and guarded
// update-map views, all derived from the collected plan. The cross-checks
// guard the derivation code paths against drifting apart.
static void dump_plan_diagnostics(NetlistContext &netlist,
		const ast::ProceduralBlockSymbol &symbol, const UpdatePlan &plan)
{
	bool dump_domains = netlist.settings.dump_write_domains.value_or(false);
	bool dump_updates = netlist.settings.dump_symbolic_updates.value_or(false);
	bool dump_map = netlist.settings.dump_update_map.value_or(false);

	if (dump_domains)
		log_write_domains(plan.domains, netlist.canvas->name, symbol.procedureKind);

	if (dump_updates || dump_map) {
		if (dump_updates)
			log_symbolic_updates(plan.symbolic, netlist.canvas->name,
					symbol.procedureKind);
		check_symbolic_updates_against_domains(plan.symbolic, plan.domains);
	}

	if (dump_map) {
		auto map = build_guarded_update_map(plan.symbolic);
		log_guarded_update_map(map, netlist.canvas->name, symbol.procedureKind);
		check_update_map_against_symbolic_updates(map, plan.symbolic);
		check_update_map_against_domains(map, plan.domains);
	}
}

void lower_comb_like_process(NetlistContext &netlist,
		const ast::ProceduralBlockSymbol &symbol, const ast::Statement &body)
{
	bool dump_any = netlist.settings.dump_write_domains.value_or(false) ||
			netlist.settings.dump_symbolic_updates.value_or(false) ||
			netlist.settings.dump_update_map.value_or(false);
	bool lower_enabled = netlist.settings.use_update_map_lowering.value_or(false) &&
			symbol.procedureKind == ast::ProceduralBlockKind::AlwaysComb;
	bool have_plan = dump_any || lower_enabled;

	UpdatePlan plan;
	if (have_plan) {
		size_t wire_count = netlist.canvas->wires_.size();
		size_t cell_count = netlist.canvas->cells_.size();
		plan = build_update_plan(netlist.eval, body);
		check_discovery_did_not_emit(netlist, wire_count, cell_count);
		plan.domains.normalize();
		if (dump_any)
			dump_plan_diagnostics(netlist, symbol, plan);
	}

	if (lower_enabled && try_lower_with_update_map(netlist, plan))
		return;

	RTLIL::Process *proc = netlist.canvas->addProcess(netlist.new_id());
	transfer_attrs(netlist, body, proc);

	ProceduralContext procedure(netlist, ProcessTiming::implicit);
	body.visit(StatementExecutor(procedure));

	VariableBits all_driven = procedure.all_driven();
	if (dump_any)
		check_write_domains_against_all_driven(plan.domains, all_driven);

	Yosys::pool<VariableBit> dangling;
	if (symbol.procedureKind != ast::ProceduralBlockKind::AlwaysComb) {
		Yosys::pool<VariableBit> driven_pool = {all_driven.begin(), all_driven.end()};
		dangling = detect_possibly_unassigned_subset(driven_pool, procedure.root_case.get());
	}

	RTLIL::SigSpec cr;
	VariableBits cl, latch_driven;

	for (auto driven_bit : all_driven) {
		if (!dangling.count(driven_bit)) {
			// Non-dangling bits are driven directly by this process.
			cl.append(driven_bit);
			cr.append(procedure.vstate.visible_assignments.at(driven_bit));
		} else {
			latch_driven.append(driven_bit);
		}
	}

	if (symbol.procedureKind == ast::ProceduralBlockKind::AlwaysLatch && !cl.empty()) {
		for (auto chunk : cl.chunks()) {
			auto &diag = netlist.add_diag(diag::LatchNotInferred, symbol.location);
			diag << chunk.text();
		}
	}

	if (!latch_driven.empty()) {
		// Latches need explicit enable and staging wires per retained bit.
		Yosys::dict<VariableBit, RTLIL::SigSig> signaling;
		RTLIL::SigSpec enables, all_staging;

		latch_driven.sort_and_unify();
		for (auto chunk : latch_driven.chunks()) {
			RTLIL::SigSpec en = netlist.add_placeholder_signal(chunk.bitwidth());
			RTLIL::SigSpec staging = netlist.add_placeholder_signal(chunk.bitwidth());

			for (uint64_t i = 0; i < chunk.bitwidth(); i++) {
				const ast::Symbol &symbol_ref = symbol;
				RTLIL::Cell *cell = netlist.canvas->addDlatch(netlist.new_id(), en[i],
										staging[i], netlist.convert_static(chunk[i]), true);
				netlist.driven_variables.insert(chunk[i]);
				netlist.register_driven_variables.insert(chunk[i]);
				transfer_attrs(netlist, symbol_ref, cell);
				signaling[chunk[i]] = {en[i], staging[i]};
			}
			enables.append(en);
			all_staging.append(staging);
		}

		procedure.root_case->aux_actions.push_back(
					{enables, RTLIL::SigSpec(RTLIL::S0, enables.size())});
		procedure.root_case->aux_actions.push_back(
					{all_staging, RTLIL::SigSpec(RTLIL::Sx, all_staging.size())});
		procedure.root_case->insert_latch_signaling(netlist, signaling);
	}

	procedure.copy_case_tree_into(proc->root_case);
	netlist.add_continuous_driver(cl, cr);
}

} // namespace slang_frontend
