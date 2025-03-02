/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#pragma once

#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/vasm.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-visit.h"

#include <cstdint>
#include <utility>

namespace HPHP::jit {

///////////////////////////////////////////////////////////////////////////////

struct Env {
  Env(Vunit& unit, const jit::vector<Vlabel>& labels)
      : unit(unit)
      , preds(computePreds(unit)) {
    init(labels);
  }

  void init(const jit::vector<Vlabel>& labels);

  Vunit& unit;

  // Number of uses of each Vreg.
  jit::vector<uint32_t> use_counts;

  // Instruction which def'd each Vreg.  Probably only useful when the def
  // instruction only has a single dst, but that's all we need right now.
  jit::vector<Vinstr::Opcode> def_insts;

  // Vregs which are constants, or copies of constants.
  jit::vector<Optional<Vconst>> consts;

  // Predecessors for each block.
  const PredVector preds;
};

template<Vinstr::Opcode op>
using op_type = typename Vinstr::op_matcher<op>::type;

/*
 * Check if the instruction at block `b', index `i' of `env.unit' is an `op'.
 * If so, call `f' on the specific instruction, and return the result.  If not,
 * return a default-constructed instance of f's return type.
 */
template<Vinstr::Opcode op, typename F>
auto if_inst(const Env& env, Vlabel b, size_t i, F f)
  -> decltype(f(std::declval<const op_type<op>&>()))
{
  auto const& code = env.unit.blocks[b].code;
  if (i >= code.size() || code[i].op != op) {
    return decltype(f(code[i].get<op>())){};
  }
  return f(code[i].get<op>());
}

/*
 * Helper for vasm-simplification routines.
 *
 * This just wraps vmodify() with some accounting logic for `env'.
 */
template<typename Simplify>
auto simplify_impl(Env& env, Vlabel b, size_t i, Simplify simplify)
  -> decltype(simplify(std::declval<Vout&>()), false)
{
  auto& unit = env.unit;

  return vmodify(unit, b, i, [&] (Vout& v) {
    auto& blocks = unit.blocks;
    auto const nremove = simplify(v);

    // Update use counts for to-be-removed instructions.
    for (auto j = i; j < i + nremove; ++j) {
      visitUses(unit, blocks[b].code[j], [&] (Vreg r) {
        --env.use_counts[r];
      });
    }

    // Update use counts and def instructions for to-be-added instructions.
    for (auto const& inst : blocks[Vlabel(v)].code) {
      visitUses(unit, inst, [&] (Vreg r) {
        if (r >= env.use_counts.size()) {
          env.use_counts.resize(size_t{r} + 1);
        }
        ++env.use_counts[r];
        if (r >= env.consts.size()) {
          env.consts.resize(size_t{r} + 1);
        }
        auto const it = env.unit.regToConst.find(r);
        if (it != env.unit.regToConst.end()) env.consts[r] = it->second;
      });
      visitDefs(unit, inst, [&] (Vreg r) {
        if (r >= env.def_insts.size()) {
          env.def_insts.resize(size_t{r} + 1, Vinstr::nop);
        }
        env.def_insts[r] = inst.op;
        if (r >= env.consts.size()) {
          env.consts.resize(size_t{r} + 1);
        }
      });
      if (inst.op == Vinstr::copy && !inst.copy_.d.isPhys()) {
        env.consts[inst.copy_.d] = env.consts[inst.copy_.s];
      }
    }

    return nremove;
  });
}

inline bool simplify_impl(Env& env, Vlabel b, size_t i, const Vinstr& instr) {
  return simplify_impl(env, b, i, [&] (Vout& v) {
    v << instr;
    return 1;
  });
}

namespace x64 {
bool simplify(Env& env, Vlabel b, size_t i);
bool psimplify(Env& env, Vlabel b, size_t i);
}

namespace arm {
bool simplify(Env& env, Vlabel b, size_t i);
bool psimplify(Env& env, Vlabel b, size_t i);
}

///////////////////////////////////////////////////////////////////////////////

}
