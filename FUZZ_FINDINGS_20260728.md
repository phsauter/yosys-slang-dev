# Elaboration correctness QA findings — 2026-07-28

Repository: `/scratch/phsauter/synthesis/yosys-slang-dev`

Branch: `testing`

Starting revision: `5b7430c`

Scope: classic procedural lowering versus `--use-update-map-lowering` for
`always_comb`, with focused language-semantic checks where a same-frontend
miter is not sufficient.

The worktree already contained the recent automatic-local, dynamic packed-range,
and multi-root concatenation fixes when this campaign began. Those owner-provided
changes are treated as the starting state and are not counted as findings below.

## Finding summary

| ID | Severity | Symptom | Status |
| --- | --- | --- | --- |
| F1 | Critical | Silent wrong logic / combinational loop after a nested read of a pending assignment | Fixed |
| F2 | High | Silent loss of module-scope writes made by a called function | Fixed |

Severity totals: **1 critical, 1 high, 0 medium, 0 low**.

## F1 — Nested pending reads used the pre-process wire

- Testcases:
  - `tests/various/update_map_pending_rhs_read.ys`
  - `tests/various/update_map_single_bit_struct_bits.ys`
- Minimal trigger:

  ```systemverilog
  always_comb begin
    temporary = data_i;
    temporary = temporary ^ invert_i;
    data_o = temporary;
  end
  ```

- Symptom: the classic/update-map SAT miter failed. The update-map netlist
  implemented the second assignment as `temporary = temporary ^ invert_i`,
  creating a combinational self-loop instead of using the value from the first
  blocking assignment. A counterexample exists even at
  `data_i=0, invert_i=0`.
- Root cause:
  1. `expression_reads_pending` installed an AST visitor handler for the base
     `Expression` type. In slang's visitor API, a matching handler suppresses
     the default recursive walk, so named-value reads nested below an operator,
     cast, select, or call argument were never seen.
  2. Flushing an update event wrote directly to `procedure.vstate` without
     updating `ProceduralContext`'s blocking-assignment bookkeeping.
     `substitute_rvalue` therefore bypassed the flushed value for a static
     variable and read the underlying RTLIL wire.
- Fix:
  - Visit `NamedValueExpression` leaves so operators and other containers are
    recursively traversed.
  - Flush materialized updates through `ProceduralContext::do_simple_assign`,
    keeping variable state and blocking-assignment tracking consistent.
- Regression: both testcases above now prove equivalent with
  `miter -equiv -flatten` plus `sat -verify`.
- Status: **fixed**.

This is the most dangerous finding in the campaign because it affects ordinary
blocking-assignment sequencing, does not require an unusual datatype, and
silently changes a feed-forward expression into a combinational loop.

## F2 — Function upward-reference writes disappeared

- Testcases:
  - `tests/various/update_map_function_upward_ref.ys`
  - `tests/various/update_map_function_upward_ref_local_init.ys`
  - `tests/various/update_map_function_upward_ref_for_step.ys`
- Trigger forms:
  - a function called on an assignment RHS writes a module output;
  - the same function is called from an automatic-local initializer;
  - the same function is called from a procedural `for` step.
- Symptom: the function return value matched, but the module-scope output
  written inside the function was not driven by the update-map result. All
  three classic/update-map SAT miters failed before the fix.
- Root cause: plan construction inspected the syntax of the call expression
  but did not traverse the called subroutine body. The function execution
  updated `ProceduralContext`, while the update-map final driver set was derived
  only from roots discovered in the enclosing process plan. The upward-written
  root was therefore omitted.
- Fix:
  - Recursively inspect called, recursion-free subroutine bodies with a visited
    set.
  - Detect value references outside the active subroutine's local scope.
  - Reject affected RHS calls, local initializers, loop initializers,
    conditions, and steps from update-map materialization so they safely use
    classic process lowering.
  - Pure nested functions and struct-returning functions remain on the
    update-map path.
- Regression: all three failing forms now log a conservative fallback and
  prove equivalent. Additional passing coverage verifies function output args,
  multiple output args, tasks with output args, deep pure calls, struct returns,
  and an automatic function with a static local.
- Status: **fixed**.

## Targeted differential coverage

Twenty-five new focused `.ys` cases were added in addition to the
owner-provided automatic-local regression. Each case elaborates classic and
update-map variants and proves observable-output equivalence with a Yosys
miter/SAT check.

- Interfaces and hierarchy:
  - generic interfaces without virtual interfaces;
  - modports;
  - interface arrays under generate loops;
  - arrays of module instances.
- Aggregates and types:
  - packed and unpacked structs;
  - packed and unpacked unions;
  - enum fields and typedef chains;
  - signed/unsigned conversions;
  - single-bit structs and `$bits`-derived widths;
  - whole-struct case assignments.
- Arrays and selects:
  - packed arrays of structs;
  - one- and two-dimensional unpacked arrays of packed structs;
  - nested member, element, and indexed-range LHS forms;
  - dynamic packed part-selects.
- Subroutines and locals:
  - direct and deeply nested pure functions;
  - struct-returning functions;
  - function output args and multiple LHS roots;
  - tasks with output args;
  - static function locals with an explicit semantic oracle;
  - nested automatic locals;
  - upward-reference calls from RHS, local initialization, and loop control.

The static-local test calls the same function twice in one `always_comb`
activation and separately proves that the second call observes the value stored
by the first call, in both lowering modes.

## Randomized differential sweep

A deterministic 200-case generated sweep (seed `20260728`) exercised fully
assigned 8-bit `always_comb` programs containing:

- nested read-after-write arithmetic;
- dynamic bit and indexed part-select writes;
- assigned indices reused by later LHS selectors;
- conditional partial writes;
- case dispatch dependent on pending values;
- mixed whole-word and partial updates.

All 200 classic/update-map SAT miters passed after F1 and F2 were fixed. No case
fell back from update-map materialization. Generated artifacts were kept outside
the repository at `/tmp/update_map_fuzz_20260728.ys` and
`/tmp/update_map_fuzz_20260728.log`.

## Validation status

- Build: `cmake --build build -j8` — passed.
- Focused update-map CTest run: **29/29 passed**.
- Full CTest suite: **81/81 passed**.
- `git diff --check`: passed.
- Standalone frontend CEC driver: not run because its hard-coded Manta filelist
  asset is absent; this does not affect the CTest or direct SAT-miter results.
- CVA6 classic-path baseline and post-fix histogram: **not run**. An unrelated
  Yosys process (PID 4100497) remained active at approximately 22.3 GB RSS and
  99.8% CPU throughout the available benchmark window.
- Snitch cluster frontend-only smoke: **not run** for the same reason.

The big-design runs were deliberately not started concurrently with an
unrelated Yosys synthesis already using approximately 22 GB RSS, per the
single-big-Yosys conduct requirement.

---

# Round 2 — current-value, control-flow, and datatype expansion

Round-2 scope was classic procedural lowering versus
`--use-update-map-lowering`, with emphasis on values read after earlier
blocking writes and on subroutine side effects that were not exercised in
round 1.

## Round-2 finding summary

| ID | Severity | Symptom | Status |
| --- | --- | --- | --- |
| F3 | High | A function return under control flow could make the update-map result silently become zero | Fixed |
| F4 | High | A function with a static local called twice in one expression was not equivalent to classic lowering | Fixed |

Round-2 severity totals: **0 critical, 2 high, 0 medium, 0 low**.

## F3 — Controlled function return state was not representable

- Regression: `tests/various/update_map_control_flow_round2.ys`
- Trigger:

  ```systemverilog
  function automatic logic [15:0] early_patch(...);
    early_patch = value;
    early_patch[3:0] = patch[3:0];
    if (stop)
      return early_patch;
    early_patch[15:8] = patch[15:8];
    return early_patch ^ 16'h1021;
  endfunction
  ```

- Symptom: the classic/update-map SAT miter failed. For
  `seed_i=16'hca9a`, `patch_i=16'hee19`, `op_i=2`, and `select_i=0`, the
  classic function result was `16'hfef8` while the update-map result was
  `16'h0000`. The other 48 observed output bits matched.
- Root cause: executing a controlled `return` through `StatementExecutor`
  creates escape-flag and procedural case actions. The update-map
  materializer consumes the resulting variable state but does not represent
  that nested procedural case tree as update-map output roots, so the
  function return value could be left with an invalid driver.
- Fix: recursively inspect called subroutines and conservatively keep a call
  on classic process lowering when a `return` occurs under an `if`, `case`,
  or loop. Ordinary tail-return functions and pure call chains remain on the
  update-map path.
- Status: **fixed**.

This was the most significant round-2 finding because a common early-return
idiom could silently replace an entire function result with zero while the
rest of the combinational block remained plausible.

## F4 — Persistent function-local state was omitted

- Regression:
  `tests/various/update_map_function_static_expression_round2.ys`
- Trigger:

  ```systemverilog
  always_comb
    observed_o = {remember(first_i), remember(second_i)};
  ```

  `remember` reads and updates a static function local.
- Symptom: the miter failed for `first_i=0`, `second_i=1`: classic lowering
  admitted `observed_o=16'h0100` while update-map lowering produced
  `16'h0000`. The low byte is separately proven to observe the update made by
  the first call.
- Root cause: a static local is persistent state even though it is lexically
  local to the function. The round-1 upward-reference test treated
  subroutine-local symbols as safe, so the persistent local was absent from
  the update-map root set. Calling the function twice in one expression made
  that missing state observable.
- Fix: references to static subroutine locals now conservatively select
  classic process lowering. This supersedes the round-1 note that the static
  local test remained on the update-map path.
- Status: **fixed**.

## Focused round-2 coverage

Six new CTest scripts add differential or structural coverage:

- `update_map_read_after_write_round2.ys`: constant and variable packed-array
  elements, part-to-whole and whole-to-part reads, packed struct fields,
  chained pure functions, and loop-carried updates;
- `update_map_control_flow_round2.ys`: one-sided branch writes followed by
  reads, overlapping `casez` items plus default, controlled function returns,
  nested conditional loops, and latch-versus-complete-path structure;
- `update_map_multi_block_round2.ys`: producer/consumer `always_comb` blocks,
  self-output reads after a default, and a two-block fixpoint with matching
  SCC structure;
- `update_map_subroutines_round2.ys`: function output arguments, a task
  writing multiple roots, and a three-deep packed-struct-by-value call chain;
- `update_map_datatypes_round2.ys`: packed union aliases, enum arithmetic with
  explicit casts, signed dynamic part-selects, `$bits`/`$size`-dependent
  function widths, streaming LHS, and overlapping concatenation LHS;
- `update_map_function_static_expression_round2.ys`: two stateful function
  calls in one expression plus an explicit same-activation semantic oracle.

Streaming and concatenation LHS forms, tasks, controlled returns, and
persistent static-local functions currently prove equivalence through an
intentional classic-lowering fallback. Function/task `inout` actuals remain
an upstream frontend limitation: both lowering modes diagnose the form as an
unsupported language feature, so round 2 covers supported output arguments
instead.

## Reproducible randomized differential sweep

`tests/formal_cec/generate_update_map_fuzz.py` is a new deterministic
generator. With seed `20260728`, it emitted 300 cases across six rotating
families:

- part/whole read-after-write and chained function updates;
- packed struct member writes followed by whole-struct reads;
- constant and dynamic packed-array element writes and later aliased reads;
- one-sided branches plus overlapping/default `casez` writes;
- fixed for-loops with loop-carried and conditional writes;
- enum casts/arithmetic and repeated pure-function calls.

Command:

```text
python3 tests/formal_cec/generate_update_map_fuzz.py \
  --cases 300 --seed 20260728 \
  --output /tmp/update_map_fuzz_round2_20260728.ys
yosys -m ./build/slang.so \
  -ql /tmp/update_map_fuzz_round2_20260728.log \
  -s /tmp/update_map_fuzz_round2_20260728.ys
```

Result: **300/300 SAT equivalence proofs passed**, with **0 update-map
fallbacks** and no errors. Runtime was 7.05 seconds wall clock with a 35.2 MiB
peak RSS.

## Round-2 validation

- Build: `cmake --build build -j8` — passed.
- Focused update-map CTest run: **35/35 passed**.
- Full CTest suite: **87/87 passed**.
- `git diff --check`: passed.
- No commit or push was made.

### Big-design checks

The classic-path CVA6 baseline was recorded using the current Yosys 0.65+11
and plugin build, top `cva6`, under a 30 GiB virtual-memory cap:

```text
59339 cells
  288 $add          2979 $aldff        3329 $and
  306 $bmux        15709 $buf           172 $bwmux
  414 $demux           6 $dff             1 $dffe
 5349 $eq            414 $ge             61 $gt
  157 $le           2255 $logic_and     1286 $logic_not
  302 $logic_or      552 $lt             14 $meminit
   14 $memrd_v2       53 $mul         13130 $mux
   88 $ne             25 $neg          1904 $not
 2563 $or            368 $pmux          143 $reduce_and
   85 $reduce_bool   596 $reduce_or        9 $shift
   69 $shiftx         44 $shl             20 $shr
    1 $sshl            2 $sshr            78 $sub
  264 $xnor         6289 $xor
```

The run used 498.34 MiB peak RSS. The round-2 source fixes only affect plan
construction when update-map lowering is enabled; classic mode does not build
or consume this plan, so the post-fix classic histogram is unchanged by
construction. A second benchmark invocation was not started concurrently
with the externally launched synthesis Yosys processes present at the final
validation window.

The requested `snitch_cluster_wrapper` frontend-through-hierarchy check
passed under the same 30 GiB cap: exit status 0, top preserved as
`snitch_cluster_wrapper`, 11m31s wall clock, and 2.44 GiB peak RSS. Most of the
runtime (95%) was Yosys `proc_rmdead` over the flattened process set; no pass
after hierarchy was requested.
