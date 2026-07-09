# Review: update-map always_comb lowering (2026-07-07)

Scope: the uncommitted working-tree change adding write-domain discovery
(`src/write_domains.{cc,h}`), symbolic-update collection + guarded update
map (diagnostic), and the process-free always_comb materializer
(`src/process_lowering.{cc,h}`, `--use-update-map-lowering`), plus the
aggregate masked-write generalization in `src/procedural.cc`.

## Architecture assessment

The staging is sound: discovery and the symbolic map are side-effect-free
(enforced by `check_discovery_did_not_emit`) and cross-checked against each
other and against the classic path's `all_driven` — good scaffolding for
trusting the new path incrementally. The materializer's core idea — evaluate
each RHS once, accumulate guarded masked update events, coalesce, and flush
as `Bwmux(fallback, value, mask)` into a single continuous driver — is the
right shape: it eliminates the RTLIL process entirely and shares RHS logic
across control branches instead of duplicating per case-tree leaf.

One honest observation: the materializer does NOT consume the guarded update
map; it re-walks the AST independently. The map layer is diagnostics only.
That is fine for bring-up, but longer-term either the materializer should be
driven by the map (single source of truth) or the map should stay thin;
today ~500 lines of walker logic exist in three near-parallel copies
(domain walker, symbolic walker, support checker + materializer).

## Correctness fixes applied (with regression tests, verified failing pre-fix)

1. **Coalescing order violation across overlapping bit sets**
   (`try_coalesce_event`): the backward scan merged a later static write
   into an earlier same-bits event PAST an intervening event with a
   different-but-overlapping bit set (e.g. `v = 0; v[3:0] = a; v[idx] = b;
   v[3:0] = c;` — the dynamic whole-`v` event flushes after the merged
   `v[3:0]` event, so `b` wrongly survives over `c`). Fixed: the scan stops
   at the first overlapping event; coalescing may only skip disjoint ones.
   Regression: `order_gold/order_gate` in tests/various/update_map_lowering.ys.

2. **LHS selector reads did not observe pending updates**
   (`apply_assignment`): `idx = base + 1; v[idx] = d;` evaluated the index
   against pre-update state because only the RHS was checked for
   pending-variable reads. Fixed with `lhs_selectors_read_pending` (checks
   select/index subtrees only — the write target itself is not a read).
   Regression: `lhsidx_gold/lhsidx_gate` in the same test file.

## Lean/clean changes applied

- Shape masks no longer emit cells: `sparse_guarded_mask` built a
  `LogicAnd` per active bit for the shape copy whose only consumers are
  activity checks (`mask_bit_active`). Replaced with a cell-free activity
  mask (S1/S0). Semantics identical (non-const bits already counted as
  active); pure netlist-garbage reduction.
- Pending-read queries went from O(events x bits) with a `text()` string
  allocation per bit to O(1) pools maintained at event insertion.
- The three copy-pasted coverage cross-checks share one
  `check_covered_bits_match` helper.
- `process_kind_name` is shared from write_domains; the update-map logs no
  longer label always/always_latch as "process".
- Materializer failures that emitted no cells now fall back to the classic
  lowering instead of `log_error` (non-constant loop bounds are only
  detectable during materialization; the support checker cannot pre-verify
  them). Failures after cells were emitted remain hard errors since there
  is no clean rollback — materializing into a scratch module would remove
  that limitation and is the right v2 hardening.
- `lower_comb_like_process` reads as policy again; the dump/check plumbing
  moved to `run_lowering_diagnostics`.
- Removed the hand-rolled `same_bits` (VariableBits has operator==).

All 53 tests pass (including the 5 new suites of this change and the 2 new
regressions).

## Opportunities for better elaboration results

Ranked by expected synthesis QoR effect:

1. **Skip the priority ladder for provably parallel cases.** The case
   handler guards item k with `match_k AND NOT(match_0 OR ... OR
   match_{k-1})` — a serial chain of depth O(items) even when all item
   expressions are distinct constants (provably disjoint, the common
   decoder shape). Detect constant-disjoint item sets (the BitPatternPool
   machinery already exists) and emit the raw matches as guards. This
   directly shortens decoder-driven control cones, which neither ABC nor
   opt reliably re-flatten once the priority structure is in the netlist.

2. **Dead-write elimination at coalesce time.** A later unconditional
   full-width event should drop earlier events for the same bits entirely
   (and their masks' cells); today they still flush and stack Bwmux layers
   that downstream opt must chew through. Cheap: in `try_coalesce_event`,
   when the incoming event is an unconditional cover, erase prior same-bits
   events instead of merging into them.

3. **Word-level flush for uniform events.** When an event's mask is the
   same single guard bit replicated (the whole-signal `if (en) x = ...;`
   shape), flush as a word-level `$mux(fallback, value, guard)` instead of
   `Bwmux` with a replicated mask. Word-level muxes preserve bus structure
   for techmap/ABC grouping; bitwise masks dissolve it.

4. **Incomplete-coverage policy is silently different from the classic
   path — make it deliberate.** `fallback_value` fills never-assigned bits
   with Sx, so an incomplete always_comb elaborates to don't-cares (real
   optimization freedom, likely QoR upside) where the classic path leaves
   process semantics (and latch diagnostics for always_latch-shaped code).
   Keep the x-fill, but emit a diagnostic when coverage is incomplete so
   simulation/synthesis mismatch is a visible choice, not an accident.

5. **Guard reuse.** `visit_guarded` rebuilds `AND(current_guard, cond)`
   chains per nesting level and `invert_guard` per else; reconvergent
   control (same condition tested in several statements) duplicates these
   cells. A small (parent_guard, cond) -> guard cache would deduplicate.
   Downstream opt usually merges them, but not across module-level
   boundaries of later passes; cheaper to not create them.

6. **Support growth, in order of payoff:** multi-condition `if` (fold with
   and_guard — trivial), hierarchical reads on the RHS (reads are safe; the
   current blanket rejection exists only because reads-pending tracking is
   NamedValue-only — extend `expression_reads_pending` to
   HierarchicalValue and allow them), pattern conditions, `unique`/
   `priority` case (use them to justify the parallel-case optimization in
   (1) even for non-constant items), constant-trip `while`/`repeat`.
   Inferred-memory writes should stay excluded (different machinery).

7. **Update-map layer hardening if it becomes load-bearing:** the root
   grouping keys on `Variable::text()`, which can collide for shadowed
   names in nested scopes; key on `Variable` itself. Also
   `SymbolicUpdate.control` strings drop the branch index for case items
   ("case" for every item), which limits their diagnostic value — encode
   the item ordinal.

## Files touched by this review

- src/process_lowering.cc (fixes + leaning)
- src/write_domains.cc / .h (process_kind_name export)
- tests/various/update_map_lowering.ys (two regressions)

## Restructure: single-walk plan pipeline (2026-07-07, per owner direction)

The three parallel walkers are gone. `ProcessPlanBuilder` performs ONE
side-effect-free AST walk producing an `UpdatePlan`: the control-flow tree
with AST expression references (conditions, RHS, loop controls), the
write-domain records and the ordered symbolic-update view (both built from
the same per-assignment classification, so the old cross-checks now guard
mere derivation drift), and the supported/unsupported verdict (subsuming
MaterializeSupportChecker). `PlanMaterializer` consumes only the plan;
nothing walks the AST twice. `discover_write_domains` and its walker were
deleted from write_domains.cc. Unsupported constructs (while/foreach/timed/
return/break/...) are still traversed for diagnostic completeness and mark
the plan for classic fallback.

Lean-representation passes folded into the materializer:
- Dead-write erasure: an unconditional full-width write drops every pending
  event it fully covers (test: deadwrite_gate has zero mux cells AND the
  dead RHS logic is gone end-to-end).
- Flush fast paths: full-mask events emit the value directly (no Bwmux);
  uniform-guard events emit one word-level $mux.
- Word-level priority merge: conflicting whole-word writes under uniform
  guards merge as Mux(old, new, guard) with an OR-combined mask instead of
  dissolving the bus into per-bit muxes. Probe (mixed static/if/dynamic/
  case process, post-`proc; opt`): was 37 cells (27 bit muxes), now 16 vs
  classic 12. Bitwise masks (dynamic demux writes) keep the per-bit merge
  the coalescing test battery encodes; two structural test assertions were
  re-baselined to the strictly leaner outcomes (coalesce_priority /
  coalesce_static_overlap now need no $bwmux at all).
- Parallel case: provably disjoint constant items skip the priority ladder
  (asserted: zero $logic_and on the parcase test); `==0` compares
  canonicalize to $logic_not via Biop as before.
- Fully-defined compares use $eq instead of $eqx.
- Multi-condition (`&&&`) ifs fold with and_guard instead of falling back.

Remaining known gap vs classic on the probe: proc builds one 4-way $pmux
where the plan flush builds a 3-deep word-mux chain (+2 $logic_or). At gate
level these lower identically; forming $pmux directly at flush (grouping
one-hot-guarded events per root) is the next opportunity if pmux-consuming
passes are to benefit before techmap.

All 53 tests pass; the two re-baselined assertions are commented in the
test file with the reasoning.

## Full-Manta validation (2026-07-07 autonomous check)

First real-design run of the restructured path: full manta_hwpe_top_wrapper
elaboration with --use-update-map-lowering (nomux flist, dev slang.so).

- Stability: clean exit, zero errors; 53 processes fell back gracefully
  (27 unsupported assignment shape, 20 case dispatch, 6 conditional shape) —
  the graceful-fallback path works at scale.
- Runtime: elaborate + proc + opt_expr + opt_clean is 557 s classic vs
  **80 s** with update-map lowering (7x) — materialized processes skip
  proc's case-tree machinery entirely.
- Representation size: intermediate (pre-proc) blowup to 1.91M cells
  (classic: 228k + processes) — the per-bit guard AND cells of
  sparse_guarded_mask on wide buses; after opt_expr/opt_clean the gap is
  258k vs 180k cells (+44%).

Follow-ups this suggests, in order:
1. Word-level guarded masks: sparse_guarded_mask emits one $logic_and per
   active mask bit; for uniform masks a single guard AND + replication (or
   deferring the guard to the event level) removes most of the intermediate
   blowup and likely much of the +44%.
2. Full-`opt` (and post-ABC) comparison to see how much of the residual is
   structural vs foldable; the unit probe suggests ~+33% residual from
   mux-chains-vs-pmux, which gate-level lowering largely equalizes.
3. Incomplete-coverage x-fill means the two modes are not logically
   identical on incompletely assigned processes — cell deltas partly
   reflect legitimate don't-care freedom.

Also added the deliberate-x-fill debug diagnostic (log_debug per
materialized process with remaining don't-care bits). Suite still 53/53.

## Guard-through-decoder (2026-07-07, follow-up executed)

The 1.68M intermediate $logic_and cells were NOT the uniform static masks
(LogicAnd already dedupes those internally) but the dynamic-write demux
masks: one-hot decoders over wide containers, guard ANDed per decoded bit.
Fix: distribute the guard THROUGH the decoder - apply_dynamic demuxes the
guard bit itself (shape stays a separate S1 demux for activity bookkeeping)
and apply_aggregate_dynamic seeds expand_aggregate_write with the
guard-replicated mask; apply_masked gained a pre_guarded flag. Also
sparse_guarded_mask now emits one AND per DISTINCT mask bit (uniform masks
= one cell).

Full-Manta effect: intermediate 1.91M -> 237k cells ($logic_and 1.68M ->
2,010); post proc+opt_expr+opt_clean 258k -> 189k cells = +5.3% vs classic
179.5k (was +44%); elaborate+proc+opt_clean 53 s vs classic 557 s. The
update-map intermediate is now roughly the SIZE OF classic's elaboration
while already being process-free, at ~10x lower front-end runtime. The
residual +5% is mux-chain-vs-pmux structure plus legitimate x-fill
don't-care differences. Suite 53/53 (SAT miters cover the changed dynamic
paths).

## Full-opt convergence (final validation datapoint)

Full Manta through `proc; opt -full`: classic 159,721 cells in 737 s;
update-map 162,443 cells in 140 s. The netlist-size delta converges to
+1.7% (from +44% pre-fixes / +5.3% light-opt), within the noise of the
legitimate x-fill don't-care differences; front-end runtime advantage
holds at ~5x. Validation story complete: the update-map path elaborates
process-free at netlist-size parity and substantially lower runtime, with
word-level structure preserved. Remaining optional items: direct $pmux
formation at flush, post-ABC A/B (gate-level QoR), enabling by default.

## Post-ABC gate-level A/B (2026-07-07)

Both modes elaborated fresh (flow flags: --keep-hierarchy --module-uniquify param
--ff-naming signal), identical backend: proc; opt -full; techmap; opt -fast;
abc -genlib red_arrow.genlib -D 1000; flattened stat over manta_hwpe_top_wrapper.

| | classic | update-map | delta |
|---|---|---|---|
| comb cells | 896,773 | 880,905 | -1.8% |
| genlib area | 223,788 | 219,306 | -2.0% |
| FFs | 87,990 | 87,959 | ~equal |
| elab+opt wall | 270 s | 31 s | 8.7x faster |
| abc wall | 306 s | 86 s | 3.6x faster |

The +1.7% word-level residual inverts after technology mapping: update-map is
the smaller netlist at the gate level. The mux-chain shape maps fine.

## Direct $pmux formation at flush (2026-07-07, owner-approved investigation)

Implemented: the word-level fast path in merge_priority_update no longer folds
eagerly into a mux chain; whole-word guarded writes stack on the event
(UpdateEvent::word_writes) and the flush decides. Writes from distinct items
(or the default) of one parallel case carry (par_case, par_slot) provenance
tags stamped in materialize_case; same case + distinct slots proves the guards
pairwise disjoint (each guard is a conjunction containing its item's disjoint
constant match), so the stack folds into a single flat $pmux over the fallback
(try_parallel_pmux). Everything else (repeated slots, mixed provenance, S1
guards, partial-word writes) demotes to materialize_word_writes, which rebuilds
exactly the old chain. Non-parallel cases clear the tags for their item bodies.
New helper RTLILBuilder::Pmux (builder.cc, degenerates to Mux at one select).

Regression: parcase asserts extended (1 $pmux, 0 $mux); new parbase pair
(unconditional default write before a defaultless parallel case: default
flushes as plain value, items pmux over it) with structural asserts + SAT
miter. Full ctest 53/53, no re-baselining needed - the deferred chain is
cell-identical to the old eager fold wherever $pmux does not apply.

### $pmux A/B result (same backend, same day)

| | classic | update-map (chain) | update-map ($pmux) |
|---|---|---|---|
| comb cells | 896,773 | 880,905 | 879,964 |
| genlib area | 223,788 | 219,306 | 219,076 |
| FFs | 87,990 | 87,959 | 87,959 |

$pmux formation is a further -0.11% cells / -0.10% area over the chain form
(-1.9% / -2.1% vs classic). Word-level netlists are near-identical after
opt -full (6,004 vs 6,014 $mux per-definition; the handful of surviving
elaboration-time $pmux sit where opt cannot re-derive them). The win is small
because Manta's hot always_combs are mostly if-ladders and incomplete cases,
which correctly demote to chains; the change also makes the pre-opt IR leaner
and exposes $pmux to the passes keyed on it (pmux2shiftx, memory port
inference, muxcover) at zero measured cost. Verdict: keep it on
unconditionally within the update-map path.
