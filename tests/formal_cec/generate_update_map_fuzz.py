#!/usr/bin/env python3
"""Generate deterministic classic-vs-update-map SAT differential cases."""

import argparse
import random
from pathlib import Path


MODULE = """\
module fuzz_{case}_{side} (
  input  logic [15:0] data_i,
  input  logic [15:0] mask_i,
  input  logic [2:0]  index_i,
  input  logic [1:0]  select_i,
  input  logic        en_i,
  output logic [31:0] data_o
);
  typedef struct packed {{
    logic [7:0] high;
    logic [7:0] low;
  }} pair_t;
  typedef enum logic [2:0] {{
    E0, E1, E2, E3, E4, E5, E6, E7
  }} state_e;

  function automatic logic [15:0] twist(input logic [15:0] value);
    return {{value[12:0], value[15:13]}} ^ 16'h{twist:04x};
  endfunction

  logic [15:0] x;
  logic [15:0] z;
  logic [3:0][7:0] words;
  logic [1:0] idx;
  pair_t pair;
  state_e state;

  always_comb begin
    x = data_i;
    z = mask_i;
    words = {{data_i, mask_i}};
    idx = index_i[1:0];
    pair = pair_t'(data_i);
    state = state_e'({{select_i, en_i}});
{body}
    data_o = {{x, z}};
  end
endmodule
"""


def family_body(rng: random.Random, family: int) -> str:
    c0 = rng.randrange(1 << 16)
    c1 = rng.randrange(1 << 16)
    bit = rng.randrange(8)
    lo = rng.randrange(7)

    families = [
        f"""\
    x[{lo} +: 2] = mask_i[{bit} +: 1] ^ mask_i[{(bit + 1) % 8} +: 1];
    z = x;
    x = twist(x);
    x[7:4] = z[3:0] ^ 4'h{c0 & 0xF:x};
    z = x ^ 16'h{c1:04x};""",
        f"""\
    pair.high[{lo} +: 2] = mask_i[{bit} +: 1] ^ 2'b{c0 & 3:02b};
    pair.low = pair.high ^ mask_i[7:0];
    x = pair;
    pair.high = pair.low + 8'h{c1 & 0xFF:02x};
    z = pair;""",
        f"""\
    words[idx] = data_i[7:0] ^ 8'h{c0 & 0xFF:02x};
    x = {{words[idx], words[(idx + 2'd1) & 2'd3]}};
    words[index_i[1:0]][{lo} +: 2] = x[{bit} +: 1] ^ 2'b{c1 & 3:02b};
    z = words[0] ^ words[1] ^ words[2] ^ words[3];""",
        f"""\
    if (en_i)
      x[{lo} +: 2] = mask_i[{lo} +: 2];
    z = x;
    casez (select_i ^ x[1:0])
      2'b?1: x[11:4] = mask_i[15:8];
      2'b1?: x = x ^ 16'h{c0:04x};
      default: x[15:12] = 4'h{c1 & 0xF:x};
    endcase
    z = z + x;""",
        f"""\
    for (int k = 0; k < 4; k++) begin
      if (en_i ^ k[0])
        x[k +: 2] = x[15-k -: 2] ^ mask_i[k +: 2];
      x = twist(x) + 16'h{c0:04x};
    end
    z = x ^ 16'h{c1:04x};""",
        f"""\
    state = state_e'((state + index_i + 3'd{1 + (c0 % 7)}) & 3'h7);
    x = twist(x);
    x = twist(x ^ 16'h{c1:04x});
    x[2:0] = state;
    z = {{pair.low, pair.high}} ^ x;""",
    ]
    return families[family]


def emit_case(rng: random.Random, case: int) -> str:
    family = case % 6
    twist = rng.randrange(1 << 16)
    body = family_body(rng, family)
    gold = MODULE.format(case=case, side="gold", twist=twist, body=body)
    gate = MODULE.format(case=case, side="gate", twist=twist, body=body)
    return f"""\
read_slang <<SV
{gold}SV
proc; opt
read_slang --use-update-map-lowering <<SV
{gate}SV
proc; opt
miter -equiv -flatten fuzz_{case}_gold fuzz_{case}_gate fuzz_{case}_miter
sat -verify -prove trigger 0 fuzz_{case}_miter
design -reset

"""


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", type=int, default=300)
    parser.add_argument("--seed", type=int, default=20260728)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.cases < 1:
        parser.error("--cases must be positive")

    rng = random.Random(args.seed)
    script = "".join(emit_case(rng, case) for case in range(args.cases))
    args.output.write_text(script)


if __name__ == "__main__":
    main()
