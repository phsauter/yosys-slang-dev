# CEC Frontend Report

- Generated: 2026-07-07T11:45:34+00:00
- Repository: `/scratch/phsauter/synthesis/yosys-slang-dev`
- Scratch workdir: `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend`
- Yosys: `/scratch/phsauter/synthesis/yosys/yosys`
- ABC: `/scratch/phsauter/synthesis/yosys/yosys-abc`
- slang plugin: `/scratch/phsauter/synthesis/yosys-slang-dev/build/slang.so`
- Manta flist: `/scratch/phsauter/synthesis/manta/build/red_arrow_gf22_exp_nomux_20260703/manta_rtl_yosys.flist`
- Manta top: `manta_hwpe_top_wrapper`

## Validation Gates

- Validation summary not found.

## Exact Commands and Recipes

Driver commands used:

```sh
python3.11 tests/formal_cec/run_frontend_cec.py --workdir /scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend validate --jobs 8
python3.11 tests/formal_cec/run_frontend_cec.py --workdir /scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend manta --jobs 8 --report
python3.11 tests/formal_cec/run_frontend_cec.py --workdir /scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend spotcheck --report
```

Manta elaboration scripts generated and run from `/scratch/phsauter/synthesis/manta`:
- `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/elaboration/elaborate_classic.ys`
- `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/elaboration/elaborate_update_map.ys`

Elaboration recipe:

```yosys
plugin -i /scratch/phsauter/synthesis/yosys-slang-dev/build/slang.so
read_slang --ignore-assertions --ignore-timing --top manta_hwpe_top_wrapper -f /scratch/phsauter/synthesis/manta/build/red_arrow_gf22_exp_nomux_20260703/manta_rtl_yosys.flist --keep-hierarchy --module-uniquify param --ff-naming signal [--use-update-map-lowering]
proc
opt_clean
write_rtlil -sort <side>.rtlil
```

Per-module recipe:

```yosys
read_rtlil <side>.rtlil
hierarchy -top <module>
flatten
async2sync
dffunmap
rename -enumerate -pattern ff_% t:$ff
expose -evert -evert-dff t:$dff t:$adff t:$aldff t:$ff
opt_clean
write_rtlil -sort <module>.evert.rtlil
techmap
aigmap
write_blif -top <module> <module>.blif
```

ABC and x-aware fallback commands:

```sh
yosys-abc -c 'cec gold.blif gate.blif'
```

```yosys
read_rtlil gold.evert.rtlil
rename <gold_module> gold
read_rtlil gate.evert.rtlil
rename <gate_module> gate
miter -equiv -flatten -ignore_gold_x gold gate miter
sat -verify -prove trigger 0 -enable_undef miter
```

## Verdict Counts

| Verdict | Count |
|---|---:|
| EQUIV | 49 |
| EQUIV_MOD_X | 13 |
| FAIL | 6 |
| TIMEOUT | 3 |
| SKIP | 0 |
| Total | 71 |

## Fallback Cross-Reference

Parsed `12` module(s) from update-map fallback log lines. These rows are marked `yes` in the verdict table.

## Failures, Timeouts, and Skips

FAIL modules:
- `manta_output_combiner`: x-aware SAT miter failed with rc=-15
- `manta_top`: x-aware SAT miter failed with rc=-15
- `manta_hwpe_engine`: x-aware SAT miter failed with rc=-15
- `manta_hwpe_top`: x-aware SAT miter failed with rc=-15
- `manta_hwpe_top_wrapper`: x-aware SAT miter failed with rc=-15
- `manta_accumulator`: x-aware SAT miter found a counterexample
TIMEOUT modules:
- `manta_fp32_adder`: x-aware Yosys SAT fallback timed out
- `fp32_add_core_stage1`: x-aware Yosys SAT fallback timed out
- `fp32_add_core_pipe`: x-aware Yosys SAT fallback timed out
- SKIP modules: none

## FF Port Matching

No FF-everted port-set mismatches were recorded.

## Spot Checks

- Spot-check summary not found.

## Per-Module Verdicts

| Module | Depth | Subtree Cells | Fallback | Verdict | Reason | Artifact |
|---|---:|---:|:---:|---|---|---|
| `adder_abc` | 0 | 0 |  | EQUIV_MOD_X | x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/adder_abc__b8e71f7de7` |
| `barrel_shift_left_impl` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/barrel_shift_left_impl__3f7840cf1c` |
| `barrel_shift_right` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/barrel_shift_right__85e33c790a` |
| `exp_diffs_abc` | 0 | 0 |  | EQUIV_MOD_X | x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/exp_diffs_abc__f481c3200f` |
| `exp_equal_abc` | 0 | 0 |  | EQUIV_MOD_X | x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/exp_equal_abc__1d2bbdf3a1` |
| `exp_subtract_abc` | 0 | 0 |  | EQUIV_MOD_X | x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/exp_subtract_abc__0782e29837` |
| `fifo_v3__FALL_THROUGH_0__DATA_WIDTH_32__DEPTH_8__dtype_logic_31downto0__ADDR_DEPTH_3` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/fifo_v3__FALL_THROUGH_0__DATA_WIDTH_32__DEPTH_8__dtype_logic_31downto0__ADDR_DEPTH_3__8589998597` |
| `fifo_v3__FALL_THROUGH_1__DATA_WIDTH_8__DEPTH_16__dtype_logic_7downto0__ADDR_DEPTH_4` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/fifo_v3__FALL_THROUGH_1__DATA_WIDTH_8__DEPTH_16__dtype_logic_7downto0__ADDR_DEPTH_4__ab216384c0` |
| `fp32_add_special_abc` | 0 | 0 |  | EQUIV_MOD_X | x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/fp32_add_special_abc__82baeaccc8` |
| `hci_core_assign__p811c9dc5` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hci_core_assign__p811c9dc5__71d3933140` |
| `hci_core_assign__p811c9dc5__p68f09f47` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hci_core_assign__p811c9dc5__p68f09f47__87fbecff74` |
| `hci_core_fifo` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hci_core_fifo__a5f22f51e6` |
| `hci_core_r_id_filter` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hci_core_r_id_filter__2a1c1385b4` |
| `hci_core_source` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hci_core_source__412449fa8f` |
| `hwpe_ctrl_regfile` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_ctrl_regfile__b5f04e262b` |
| `hwpe_ctrl_regfile_ff` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_ctrl_regfile_ff__5f397465a6` |
| `hwpe_ctrl_slave` | 0 | 0 | yes | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_ctrl_slave__d447e9e869` |
| `hwpe_stream_addressgen_v3__TRANS_CNT_32__CNT_32__DIM_ENABLE_1H_3` | 0 | 0 | yes | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_stream_addressgen_v3__TRANS_CNT_32__CNT_32__DIM_ENABLE_1H_3__dd2ca1a33c` |
| `hwpe_stream_addressgen_v3__TRANS_CNT_32__CNT_32__DIM_ENABLE_1H_3__pe6126018` | 0 | 0 | yes | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_stream_addressgen_v3__TRANS_CNT_32__CNT_32__DIM_ENABLE_1H_3__pe6126018__05adc85d01` |
| `hwpe_stream_assign__p811c9dc5` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_stream_assign__p811c9dc5__c5be0931b7` |
| `hwpe_stream_assign__p811c9dc5__p9e61068d` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_stream_assign__p811c9dc5__p9e61068d__b53b925bce` |
| `hwpe_stream_assign__p811c9dc5__pec42199e` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_stream_assign__p811c9dc5__pec42199e__794e1be5c8` |
| `hwpe_stream_fence` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_stream_fence__cdb50fa405` |
| `hwpe_stream_fifo__DATA_WIDTH_36__FIFO_DEPTH_2__LATCH_FIFO_0__LATCH_FIFO_TEST_WRAP_0` | 0 | 0 | yes | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_stream_fifo__DATA_WIDTH_36__FIFO_DEPTH_2__LATCH_FIFO_0__LATCH_FIFO_TEST_WRAP_0__a7ef9176e8` |
| `hwpe_stream_fifo__DATA_WIDTH_36__FIFO_DEPTH_2__LATCH_FIFO_0__LATCH_FIFO_TEST_WRAP_0__p52bf4332` | 0 | 0 | yes | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_stream_fifo__DATA_WIDTH_36__FIFO_DEPTH_2__LATCH_FIFO_0__LATCH_FIFO_TEST_WRAP_0__p52bf__ee709fbf3f` |
| `hwpe_stream_fifo__DATA_WIDTH_512__FIFO_DEPTH_2__LATCH_FIFO_0__LATCH_FIFO_TEST_WRAP_0` | 0 | 0 | yes | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_stream_fifo__DATA_WIDTH_512__FIFO_DEPTH_2__LATCH_FIFO_0__LATCH_FIFO_TEST_WRAP_0__621a0a87a4` |
| `hwpe_stream_fifo__DATA_WIDTH_516__FIFO_DEPTH_2__LATCH_FIFO_0__LATCH_FIFO_TEST_WRAP_0` | 0 | 0 | yes | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_stream_fifo__DATA_WIDTH_516__FIFO_DEPTH_2__LATCH_FIFO_0__LATCH_FIFO_TEST_WRAP_0__4dbeb19e75` |
| `hwpe_stream_fifo__DATA_WIDTH_598__FIFO_DEPTH_2__LATCH_FIFO_0__LATCH_FIFO_TEST_WRAP_0` | 0 | 0 | yes | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_stream_fifo__DATA_WIDTH_598__FIFO_DEPTH_2__LATCH_FIFO_0__LATCH_FIFO_TEST_WRAP_0__a6d0797b67` |
| `inc9_abc` | 0 | 0 |  | EQUIV_MOD_X | x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/inc9_abc__25801822b9` |
| `lza28` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/lza28__4508c9a309` |
| `lzc` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/lzc__f91ca05ee3` |
| `lzc28_abc` | 0 | 0 |  | EQUIV_MOD_X | x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/lzc28_abc__c7107ad405` |
| `mant_comp_abc` | 0 | 0 |  | EQUIV_MOD_X | x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/mant_comp_abc__42f1ab67c2` |
| `manta_accumulator` | 0 | 0 |  | FAIL | x-aware SAT miter found a counterexample | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_accumulator__a7929b258a` |
| `manta_ctrl` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_ctrl__2feb6870e3` |
| `manta_fp32_adder` | 0 | 0 |  | TIMEOUT | x-aware Yosys SAT fallback timed out | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_fp32_adder__03494a8580` |
| `manta_hwpe_ctrl` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_hwpe_ctrl__27f16dc181` |
| `manta_hwpe_input_buffer__p1d1fb13e` | 0 | 0 | yes | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_hwpe_input_buffer__p1d1fb13e__f14feddbed` |
| `manta_hwpe_input_buffer__p1e66d8ae` | 0 | 0 | yes | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_hwpe_input_buffer__p1e66d8ae__0360f3a57f` |
| `manta_hwpe_input_buffer__p6773dab8` | 0 | 0 | yes | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_hwpe_input_buffer__p6773dab8__4f7e1fd350` |
| `manta_hwpe_mux_static` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_hwpe_mux_static__d6626f75b6` |
| `manta_hwpe_stream_select` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_hwpe_stream_select__8e149a7423` |
| `manta_normalizer` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_normalizer__bf9db78ed4` |
| `manta_output_combiner` | 0 | 0 |  | FAIL | x-aware SAT miter failed with rc=-15 | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_output_combiner__3a0f2f40c3` |
| `manta_pe` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_pe__b387a1b474` |
| `manta_shift_buffer__DATA_WIDTH_32__DEPTH_1` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_shift_buffer__DATA_WIDTH_32__DEPTH_1__6ac04b5536` |
| `manta_shift_buffer__DATA_WIDTH_32__DEPTH_2` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_shift_buffer__DATA_WIDTH_32__DEPTH_2__752a395d0d` |
| `manta_shift_buffer__DATA_WIDTH_32__DEPTH_3` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_shift_buffer__DATA_WIDTH_32__DEPTH_3__72270c261d` |
| `manta_shift_buffer__DATA_WIDTH_32__DEPTH_4` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_shift_buffer__DATA_WIDTH_32__DEPTH_4__a0956eccc8` |
| `manta_shift_buffer__DATA_WIDTH_32__DEPTH_5` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_shift_buffer__DATA_WIDTH_32__DEPTH_5__fb88bab0fe` |
| `manta_shift_buffer__DATA_WIDTH_32__DEPTH_6` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_shift_buffer__DATA_WIDTH_32__DEPTH_6__e135b1aa14` |
| `manta_shift_buffer__DATA_WIDTH_32__DEPTH_7` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_shift_buffer__DATA_WIDTH_32__DEPTH_7__2f8b4969c2` |
| `manta_shift_buffer__DATA_WIDTH_32__DEPTH_8` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_shift_buffer__DATA_WIDTH_32__DEPTH_8__7f296c6910` |
| `manta_systolic_array` | 0 | 0 | yes | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_systolic_array__5e93797705` |
| `round_mant_abc` | 0 | 0 |  | EQUIV_MOD_X | x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/round_mant_abc__8f6397881d` |
| `shift_right_sticky_abc` | 0 | 0 |  | EQUIV_MOD_X | x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/shift_right_sticky_abc__e1eef5e330` |
| `tc_clk_gating` | 0 | 0 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/tc_clk_gating__52c08b939b` |
| `adder_subber` | 1 | 1 |  | EQUIV_MOD_X | x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/adder_subber__bd1f003c9b` |
| `fp32_add_core_stage1` | 1 | 1 |  | TIMEOUT | x-aware Yosys SAT fallback timed out | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/fp32_add_core_stage1__a083da81ea` |
| `fp32_add_core_stage2` | 1 | 1 |  | EQUIV_MOD_X | x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/fp32_add_core_stage2__83965b7754` |
| `hci_core_sink` | 1 | 1 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hci_core_sink__33dc068e5c` |
| `hwpe_ctrl_regfile_latch_test_wrap` | 1 | 1 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/hwpe_ctrl_regfile_latch_test_wrap__fd5067072e` |
| `manta_hwpe_input_weight_fence` | 1 | 1 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_hwpe_input_weight_fence__2901f20873` |
| `manta_hwpe_streamer` | 1 | 1 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_hwpe_streamer__13fdab2b84` |
| `manta_hwpe_top` | 1 | 1 |  | FAIL | x-aware SAT miter failed with rc=-15 | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_hwpe_top__8127ff5ccb` |
| `manta_int_to_fp` | 1 | 1 |  | EQUIV | ABC cec proved 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_int_to_fp__0aac07af40` |
| `manta_top` | 1 | 1 |  | FAIL | x-aware SAT miter failed with rc=-15 | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_top__71fe40d4df` |
| `shift_right_impl` | 1 | 1 |  | EQUIV_MOD_X | x-aware SAT miter passed after ABC/BLIF flow did not prove 2-valued equivalence | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/shift_right_impl__99a6b9fcdd` |
| `fp32_add_core_pipe` | 2 | 2 |  | TIMEOUT | x-aware Yosys SAT fallback timed out | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/fp32_add_core_pipe__a8165dd73a` |
| `manta_hwpe_engine` | 2 | 2 |  | FAIL | x-aware SAT miter failed with rc=-15 | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_hwpe_engine__b0b11c6a83` |
| `manta_hwpe_top_wrapper` | 2 | 2 |  | FAIL | x-aware SAT miter failed with rc=-15 | `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend/manta/modules/manta_hwpe_top_wrapper__be8e8f880f` |

## Created Files

- `tests/formal_cec/run_frontend_cec.py`
- `CEC_FRONTEND_REPORT.md`
- Scratch artifacts under `/scratch/phsauter/synthesis/manta/build/cec_rerun_20260707/frontend`

## Assumptions and Soundness Holes

- Classic lowering is always treated as gold; update-map lowering is gate.
- The elaboration sides are compared after exactly `proc; opt_clean`; no other pre-CEC optimization is applied before the saved RTLIL.
- Per-module checks flatten each module's full subtree, so the top-level check covers the whole chip and smaller modules localize issues.
- The FF cut uses `rename -enumerate -pattern ff_% t:$ff` then `expose -evert -evert-dff t:$dff t:$adff t:$aldff t:$ff`; in this Yosys build, `-evert-dff` alone creates boundary ports but leaves FF cells behind, and latch-derived `$ff` cells are private until renamed.
- The harness compares the complete FF-everted port set by name, direction, and width before invoking ABC or SAT.
- Modules that cannot be prepared into FF-everted BLIF are reported as SKIP; they are not treated as silently equivalent.
- ABC is a 2-valued check. When ABC fails or errors after successful FF-everted preparation, the harness escalates to `miter -ignore_gold_x` plus `sat -enable_undef` and reports `EQUIV_MOD_X` only on that proof.
- Fallback log parsing is best-effort when the log line does not contain `module=<name>`; unmatched raw lines are counted above if present.
- No classic-only or update-only modules were reported by the elaboration RTLIL parser.

