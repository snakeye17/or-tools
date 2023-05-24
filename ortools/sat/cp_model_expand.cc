// Copyright 2010-2022 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ortools/sat/cp_model_expand.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/inlined_vector.h"
#include "absl/log/check.h"
#include "absl/meta/type_traits.h"
#include "absl/strings/str_cat.h"
#include "ortools/base/integral_types.h"
#include "ortools/base/logging.h"
#include "ortools/base/stl_util.h"
#include "ortools/port/proto_utils.h"
#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_utils.h"
#include "ortools/sat/presolve_context.h"
#include "ortools/sat/sat_parameters.pb.h"
#include "ortools/sat/util.h"
#include "ortools/util/logging.h"
#include "ortools/util/saturated_arithmetic.h"
#include "ortools/util/sorted_interval_list.h"

namespace operations_research {
namespace sat {

// TODO(user): Note that if we have duplicate variables controlling different
// time point, this might not reach the fixed point. Fix? it is not that
// important as the expansion take care of this case anyway.
void PropagateAutomaton(const AutomatonConstraintProto& proto,
                        const PresolveContext& context,
                        std::vector<absl::flat_hash_set<int64_t>>* states,
                        std::vector<absl::flat_hash_set<int64_t>>* labels) {
  const int n = proto.vars_size();
  const absl::flat_hash_set<int64_t> final_states(
      {proto.final_states().begin(), proto.final_states().end()});

  labels->clear();
  labels->resize(n);
  states->clear();
  states->resize(n + 1);
  (*states)[0].insert(proto.starting_state());

  // Forward pass.
  for (int time = 0; time < n; ++time) {
    for (int t = 0; t < proto.transition_tail_size(); ++t) {
      const int64_t tail = proto.transition_tail(t);
      const int64_t label = proto.transition_label(t);
      const int64_t head = proto.transition_head(t);
      if (!(*states)[time].contains(tail)) continue;
      if (!context.DomainContains(proto.vars(time), label)) continue;
      if (time == n - 1 && !final_states.contains(head)) continue;
      (*labels)[time].insert(label);
      (*states)[time + 1].insert(head);
    }
  }

  // Backward pass.
  for (int time = n - 1; time >= 0; --time) {
    absl::flat_hash_set<int64_t> new_states;
    absl::flat_hash_set<int64_t> new_labels;
    for (int t = 0; t < proto.transition_tail_size(); ++t) {
      const int64_t tail = proto.transition_tail(t);
      const int64_t label = proto.transition_label(t);
      const int64_t head = proto.transition_head(t);

      if (!(*states)[time].contains(tail)) continue;
      if (!(*labels)[time].contains(label)) continue;
      if (!(*states)[time + 1].contains(head)) continue;
      new_labels.insert(label);
      new_states.insert(tail);
    }
    (*labels)[time].swap(new_labels);
    (*states)[time].swap(new_states);
  }
}

namespace {

void ExpandReservoir(ConstraintProto* ct, PresolveContext* context) {
  if (ct->reservoir().min_level() > ct->reservoir().max_level()) {
    VLOG(1) << "Empty level domain in reservoir constraint.";
    return (void)context->NotifyThatModelIsUnsat();
  }

  const ReservoirConstraintProto& reservoir = ct->reservoir();
  const int num_events = reservoir.time_exprs_size();
  const int true_literal = context->GetTrueLiteral();
  const auto is_active_literal = [&reservoir, true_literal](int index) {
    if (reservoir.active_literals_size() == 0) return true_literal;
    return reservoir.active_literals(index);
  };

  int num_positives = 0;
  int num_negatives = 0;
  for (const LinearExpressionProto& demand_expr : reservoir.level_changes()) {
    const int64_t demand = context->FixedValue(demand_expr);
    if (demand > 0) {
      num_positives++;
    } else if (demand < 0) {
      num_negatives++;
    }
  }

  absl::flat_hash_map<std::pair<int, int>, int> precedence_cache;

  if (num_positives > 0 && num_negatives > 0) {
    // Creates Boolean variables equivalent to (start[i] <= start[j]) i != j
    for (int i = 0; i < num_events - 1; ++i) {
      const int active_i = is_active_literal(i);
      if (context->LiteralIsFalse(active_i)) continue;
      const LinearExpressionProto& time_i = reservoir.time_exprs(i);

      for (int j = i + 1; j < num_events; ++j) {
        const int active_j = is_active_literal(j);
        if (context->LiteralIsFalse(active_j)) continue;
        const LinearExpressionProto& time_j = reservoir.time_exprs(j);

        const int i_lesseq_j = context->GetOrCreateReifiedPrecedenceLiteral(
            time_i, time_j, active_i, active_j);
        context->working_model->mutable_variables(i_lesseq_j)
            ->set_name(absl::StrCat(i, " before ", j));
        precedence_cache[{i, j}] = i_lesseq_j;
        const int j_lesseq_i = context->GetOrCreateReifiedPrecedenceLiteral(
            time_j, time_i, active_j, active_i);
        context->working_model->mutable_variables(j_lesseq_i)
            ->set_name(absl::StrCat(j, " before ", i));
        precedence_cache[{j, i}] = j_lesseq_i;
      }
    }

    // Constrains the running level to be consistent at all time_exprs.
    // For this we only add a constraint at the time a given demand
    // take place. We also have a constraint for time zero if needed
    // (added below).
    for (int i = 0; i < num_events; ++i) {
      const int active_i = is_active_literal(i);
      if (context->LiteralIsFalse(active_i)) continue;

      // Accumulates level_changes of all predecessors.
      ConstraintProto* const level = context->working_model->add_constraints();
      level->add_enforcement_literal(active_i);

      // Add contributions from previous events.
      int64_t offset = 0;
      for (int j = 0; j < num_events; ++j) {
        if (i == j) continue;
        const int active_j = is_active_literal(j);
        if (context->LiteralIsFalse(active_j)) continue;

        const auto prec_it = precedence_cache.find({j, i});
        CHECK(prec_it != precedence_cache.end());
        const int prec_lit = prec_it->second;
        const int64_t demand = context->FixedValue(reservoir.level_changes(j));
        if (RefIsPositive(prec_lit)) {
          level->mutable_linear()->add_vars(prec_lit);
          level->mutable_linear()->add_coeffs(demand);
        } else {
          level->mutable_linear()->add_vars(prec_lit);
          level->mutable_linear()->add_coeffs(-demand);
          offset -= demand;
        }
      }

      // Accounts for own demand in the domain of the sum.
      const int64_t demand_i = context->FixedValue(reservoir.level_changes(i));
      level->mutable_linear()->add_domain(
          CapAdd(CapSub(reservoir.min_level(), demand_i), offset));
      level->mutable_linear()->add_domain(
          CapAdd(CapSub(reservoir.max_level(), demand_i), offset));
    }
  } else {
    // If all level_changes have the same sign, we do not care about the order,
    // just the sum.
    auto* const sum =
        context->working_model->add_constraints()->mutable_linear();
    for (int i = 0; i < num_events; ++i) {
      sum->add_vars(is_active_literal(i));
      sum->add_coeffs(context->FixedValue(reservoir.level_changes(i)));
    }
    sum->add_domain(reservoir.min_level());
    sum->add_domain(reservoir.max_level());
  }

  ct->Clear();
  context->UpdateRuleStats("reservoir: expanded");
}

void ExpandIntMod(ConstraintProto* ct, PresolveContext* context) {
  const LinearArgumentProto& int_mod = ct->int_mod();
  const LinearExpressionProto& mod_expr = int_mod.exprs(1);
  if (context->IsFixed(mod_expr)) return;

  const LinearExpressionProto& expr = int_mod.exprs(0);
  const LinearExpressionProto& target_expr = int_mod.target();

  // We reduce the domain of target_expr to avoid later overflow.
  if (!context->IntersectDomainWith(
          target_expr, context->DomainSuperSetOf(expr).PositiveModuloBySuperset(
                           context->DomainSuperSetOf(mod_expr)))) {
    return;
  }

  // Create a new constraint with the same enforcement as ct.
  auto new_enforced_constraint = [&]() {
    ConstraintProto* new_ct = context->working_model->add_constraints();
    *new_ct->mutable_enforcement_literal() = ct->enforcement_literal();
    return new_ct;
  };

  // div_expr = expr / mod_expr.
  const int div_var = context->NewIntVar(
      context->DomainSuperSetOf(expr).PositiveDivisionBySuperset(
          context->DomainSuperSetOf(mod_expr)));
  LinearExpressionProto div_expr;
  div_expr.add_vars(div_var);
  div_expr.add_coeffs(1);

  LinearArgumentProto* const div_proto =
      new_enforced_constraint()->mutable_int_div();
  *div_proto->mutable_target() = div_expr;
  *div_proto->add_exprs() = expr;
  *div_proto->add_exprs() = mod_expr;

  // Create prod_expr = div_expr * mod_expr.
  const Domain prod_domain =
      context->DomainOf(div_var)
          .ContinuousMultiplicationBy(context->DomainSuperSetOf(mod_expr))
          .IntersectionWith(context->DomainSuperSetOf(expr).AdditionWith(
              context->DomainSuperSetOf(target_expr).Negation()));
  const int prod_var = context->NewIntVar(prod_domain);
  LinearExpressionProto prod_expr;
  prod_expr.add_vars(prod_var);
  prod_expr.add_coeffs(1);

  LinearArgumentProto* const int_prod =
      new_enforced_constraint()->mutable_int_prod();
  *int_prod->mutable_target() = prod_expr;
  *int_prod->add_exprs() = div_expr;
  *int_prod->add_exprs() = mod_expr;

  // expr - prod_expr = target_expr.
  LinearConstraintProto* const lin =
      new_enforced_constraint()->mutable_linear();
  lin->add_domain(0);
  lin->add_domain(0);
  AddLinearExpressionToLinearConstraint(expr, 1, lin);
  AddLinearExpressionToLinearConstraint(prod_expr, -1, lin);
  AddLinearExpressionToLinearConstraint(target_expr, -1, lin);

  ct->Clear();
  context->UpdateRuleStats("int_mod: expanded");
}

// TODO(user): Move this into the presolve instead?
void ExpandIntProdWithBoolean(int bool_ref,
                              const LinearExpressionProto& int_expr,
                              const LinearExpressionProto& product_expr,
                              PresolveContext* context) {
  ConstraintProto* const one = context->working_model->add_constraints();
  one->add_enforcement_literal(bool_ref);
  one->mutable_linear()->add_domain(0);
  one->mutable_linear()->add_domain(0);
  AddLinearExpressionToLinearConstraint(int_expr, 1, one->mutable_linear());
  AddLinearExpressionToLinearConstraint(product_expr, -1,
                                        one->mutable_linear());

  ConstraintProto* const zero = context->working_model->add_constraints();
  zero->add_enforcement_literal(NegatedRef(bool_ref));
  zero->mutable_linear()->add_domain(0);
  zero->mutable_linear()->add_domain(0);
  AddLinearExpressionToLinearConstraint(product_expr, 1,
                                        zero->mutable_linear());
}

void ExpandIntProd(ConstraintProto* ct, PresolveContext* context) {
  const LinearArgumentProto& int_prod = ct->int_prod();
  if (int_prod.exprs_size() != 2) return;
  const LinearExpressionProto& a = int_prod.exprs(0);
  const LinearExpressionProto& b = int_prod.exprs(1);
  const LinearExpressionProto& p = int_prod.target();
  int literal;
  const bool a_is_literal = context->ExpressionIsALiteral(a, &literal);
  const bool b_is_literal = context->ExpressionIsALiteral(b, &literal);

  // We expand if exactly one of {a, b} is a literal. If both are literals, it
  // will be presolved into a better version.
  if (a_is_literal && !b_is_literal) {
    ExpandIntProdWithBoolean(literal, b, p, context);
    ct->Clear();
    context->UpdateRuleStats("int_prod: expanded product with Boolean var");
  } else if (b_is_literal) {
    ExpandIntProdWithBoolean(literal, a, p, context);
    ct->Clear();
    context->UpdateRuleStats("int_prod: expanded product with Boolean var");
  }
}

void ExpandInverse(ConstraintProto* ct, PresolveContext* context) {
  const auto& f_direct = ct->inverse().f_direct();
  const auto& f_inverse = ct->inverse().f_inverse();
  const int n = f_direct.size();
  CHECK_EQ(n, f_inverse.size());

  // Make sure the domains are included in [0, n - 1).
  // Note that if a variable and its negation appear, the domains will be set to
  // zero here.
  //
  // TODO(user): Add support for UNSAT at expansion. This should create empty
  // domain if UNSAT, so it should still work correctly.
  absl::flat_hash_set<int> used_variables;
  for (const int ref : f_direct) {
    used_variables.insert(PositiveRef(ref));
    if (!context->IntersectDomainWith(ref, Domain(0, n - 1))) {
      VLOG(1) << "Empty domain for a variable in ExpandInverse()";
      return;
    }
  }
  for (const int ref : f_inverse) {
    used_variables.insert(PositiveRef(ref));
    if (!context->IntersectDomainWith(ref, Domain(0, n - 1))) {
      VLOG(1) << "Empty domain for a variable in ExpandInverse()";
      return;
    }
  }

  // If we have duplicate variables, we make sure the domain are reduced
  // as the loop below might not detect incompatibilities.
  if (used_variables.size() != 2 * n) {
    for (int i = 0; i < n; ++i) {
      for (int j = 0; j < n; ++j) {
        // Note that if we don't have the same sign, both domain are at zero.
        if (PositiveRef(f_direct[i]) != PositiveRef(f_inverse[j])) continue;

        // We can't have i or j as value if i != j.
        if (i == j) continue;
        if (!context->IntersectDomainWith(
                f_direct[i], Domain::FromValues({i, j}).Complement())) {
          return;
        }
      }
    }
  }

  // Reduce the domains of each variable by checking that the inverse value
  // exists.
  std::vector<int64_t> possible_values;

  // Propagate from one vector to its counterpart.
  const auto filter_inverse_domain =
      [context, n, &possible_values](const auto& direct, const auto& inverse) {
        // Propagate from the inverse vector to the direct vector.
        for (int i = 0; i < n; ++i) {
          possible_values.clear();
          const Domain domain = context->DomainOf(direct[i]);
          bool removed_value = false;
          for (const int64_t j : domain.Values()) {
            if (context->DomainOf(inverse[j]).Contains(i)) {
              possible_values.push_back(j);
            } else {
              removed_value = true;
            }
          }
          if (removed_value) {
            if (!context->IntersectDomainWith(
                    direct[i], Domain::FromValues(possible_values))) {
              VLOG(1) << "Empty domain for a variable in ExpandInverse()";
              return false;
            }
          }
        }
        return true;
      };

  // Note that this should reach the fixed point in one pass.
  // However, if we have duplicate variable, I am not sure.
  if (!filter_inverse_domain(f_direct, f_inverse)) return;
  if (!filter_inverse_domain(f_inverse, f_direct)) return;

  // Expand the inverse constraint by associating literal to var == value
  // and sharing them between the direct and inverse variables.
  //
  // Note that this is only correct because the domain are tight now.
  for (int i = 0; i < n; ++i) {
    const int f_i = f_direct[i];
    for (const int64_t j : context->DomainOf(f_i).Values()) {
      // We have f[i] == j <=> r[j] == i;
      const int r_j = f_inverse[j];
      int r_j_i;
      if (context->HasVarValueEncoding(r_j, i, &r_j_i)) {
        context->InsertVarValueEncoding(r_j_i, f_i, j);
      } else {
        const int f_i_j = context->GetOrCreateVarValueEncoding(f_i, j);
        context->InsertVarValueEncoding(f_i_j, r_j, i);
      }
    }
  }

  ct->Clear();
  context->UpdateRuleStats("inverse: expanded");
}

// A[V] == V means for all i, V == i => A_i == i
void ExpandElementWithTargetEqualIndex(ConstraintProto* ct,
                                       PresolveContext* context) {
  const ElementConstraintProto& element = ct->element();
  DCHECK_EQ(element.index(), element.target());

  const int index_ref = element.index();
  std::vector<int64_t> valid_indices;
  for (const int64_t v : context->DomainOf(index_ref).Values()) {
    if (!context->DomainContains(element.vars(v), v)) continue;
    valid_indices.push_back(v);
  }
  if (valid_indices.size() < context->DomainOf(index_ref).Size()) {
    if (!context->IntersectDomainWith(index_ref,
                                      Domain::FromValues(valid_indices))) {
      VLOG(1) << "No compatible variable domains in "
                 "ExpandElementWithTargetEqualIndex()";
      return;
    }
    context->UpdateRuleStats("element: reduced index domain");
  }

  for (const int64_t v : context->DomainOf(index_ref).Values()) {
    const int var = element.vars(v);
    if (context->MinOf(var) == v && context->MaxOf(var) == v) continue;
    context->AddImplyInDomain(
        context->GetOrCreateVarValueEncoding(index_ref, v), var, Domain(v));
  }
  context->UpdateRuleStats(
      "element: expanded with special case target = index");
  ct->Clear();
}

// Special case if the array of the element is filled with constant values.
void ExpandConstantArrayElement(ConstraintProto* ct, PresolveContext* context) {
  const ElementConstraintProto& element = ct->element();
  const int index_ref = element.index();
  const int target_ref = element.target();

  // Index and target domain have been reduced before calling this function.
  const Domain index_domain = context->DomainOf(index_ref);
  const Domain target_domain = context->DomainOf(target_ref);

  // This BoolOrs implements the deduction that if all index literals pointing
  // to the same value in the constant array are false, then this value is no
  // no longer valid for the target variable. They are created only for values
  // that have multiples literals supporting them.
  // Order is not important.
  absl::flat_hash_map<int64_t, BoolArgumentProto*> supports;
  {
    absl::flat_hash_map<int64_t, int> constant_var_values_usage;
    for (const int64_t v : index_domain.Values()) {
      DCHECK(context->IsFixed(element.vars(v)));
      const int64_t value = context->MinOf(element.vars(v));
      if (++constant_var_values_usage[value] == 2) {
        // First time we cross > 1.
        BoolArgumentProto* const support =
            context->working_model->add_constraints()->mutable_bool_or();
        const int target_literal =
            context->GetOrCreateVarValueEncoding(target_ref, value);
        support->add_literals(NegatedRef(target_literal));
        supports[value] = support;
      }
    }
  }

  {
    // While this is not stricly needed since all value in the index will be
    // covered, it allows to easily detect this fact in the presolve.
    auto* exactly_one =
        context->working_model->add_constraints()->mutable_exactly_one();
    for (const int64_t v : index_domain.Values()) {
      const int index_literal =
          context->GetOrCreateVarValueEncoding(index_ref, v);
      exactly_one->add_literals(index_literal);

      const int64_t value = context->MinOf(element.vars(v));
      const auto& it = supports.find(value);
      if (it != supports.end()) {
        // The encoding literal for 'value' of the target_ref has been
        // created before.
        const int target_literal =
            context->GetOrCreateVarValueEncoding(target_ref, value);
        context->AddImplication(index_literal, target_literal);
        it->second->add_literals(index_literal);
      } else {
        // Try to reuse the literal of the index.
        context->InsertVarValueEncoding(index_literal, target_ref, value);
      }
    }
  }

  context->UpdateRuleStats("element: expanded value element");
  ct->Clear();
}

// General element when the array contains non fixed variables.
void ExpandVariableElement(ConstraintProto* ct, PresolveContext* context) {
  const ElementConstraintProto& element = ct->element();
  const int index_ref = element.index();
  const int target_ref = element.target();
  const Domain index_domain = context->DomainOf(index_ref);

  BoolArgumentProto* exactly_one =
      context->working_model->add_constraints()->mutable_exactly_one();

  for (const int64_t v : index_domain.Values()) {
    const int var = element.vars(v);
    const Domain var_domain = context->DomainOf(var);
    const int index_lit = context->GetOrCreateVarValueEncoding(index_ref, v);
    exactly_one->add_literals(index_lit);

    if (var_domain.IsFixed()) {
      context->AddImplyInDomain(index_lit, target_ref, var_domain);
    } else {
      ConstraintProto* const ct = context->working_model->add_constraints();
      ct->add_enforcement_literal(index_lit);
      ct->mutable_linear()->add_vars(var);
      ct->mutable_linear()->add_coeffs(1);
      ct->mutable_linear()->add_vars(target_ref);
      ct->mutable_linear()->add_coeffs(-1);
      ct->mutable_linear()->add_domain(0);
      ct->mutable_linear()->add_domain(0);
    }
  }

  context->UpdateRuleStats("element: expanded");
  ct->Clear();
}

void ExpandElement(ConstraintProto* ct, PresolveContext* context) {
  const ElementConstraintProto& element = ct->element();

  const int index_ref = element.index();
  const int target_ref = element.target();
  const int size = element.vars_size();

  // Reduce the domain of the index to be compatible with the array of
  // variables. Note that the element constraint is 0 based.
  if (!context->IntersectDomainWith(index_ref, Domain(0, size - 1))) {
    VLOG(1) << "Empty domain for the index variable in ExpandElement()";
    return;
  }

  // Special case when index = target.
  if (index_ref == target_ref) {
    ExpandElementWithTargetEqualIndex(ct, context);
    return;
  }

  // Reduces the domain of the index and the target.
  bool all_constants = true;
  std::vector<int64_t> valid_indices;
  const Domain index_domain = context->DomainOf(index_ref);
  const Domain target_domain = context->DomainOf(target_ref);
  Domain reached_domain;
  for (const int64_t v : index_domain.Values()) {
    const Domain var_domain = context->DomainOf(element.vars(v));
    if (var_domain.IntersectionWith(target_domain).IsEmpty()) continue;

    valid_indices.push_back(v);
    reached_domain = reached_domain.UnionWith(var_domain);
    if (var_domain.Min() != var_domain.Max()) {
      all_constants = false;
    }
  }

  if (valid_indices.size() < index_domain.Size()) {
    if (!context->IntersectDomainWith(index_ref,
                                      Domain::FromValues(valid_indices))) {
      VLOG(1) << "No compatible variable domains in ExpandElement()";
      return;
    }

    context->UpdateRuleStats("element: reduced index domain");
  }

  // We know the target_domain is not empty as this would have triggered the
  // above check.
  bool target_domain_changed = false;
  if (!context->IntersectDomainWith(target_ref, reached_domain,
                                    &target_domain_changed)) {
    return;
  }

  if (target_domain_changed) {
    context->UpdateRuleStats("element: reduced target domain");
  }

  if (all_constants) {
    ExpandConstantArrayElement(ct, context);
    return;
  }

  ExpandVariableElement(ct, context);
}

// Adds clauses so that literals[i] true <=> encoding[values[i]] true.
// This also implicitly use the fact that exactly one alternative is true.
void LinkLiteralsAndValues(const std::vector<int>& literals,
                           const std::vector<int64_t>& values,
                           const absl::flat_hash_map<int64_t, int>& encoding,
                           PresolveContext* context) {
  CHECK_EQ(literals.size(), values.size());

  // We use a map to make this method deterministic.
  //
  // TODO(user): Make sure this does not appear in the profile.
  absl::btree_map<int, std::vector<int>> encoding_lit_to_support;

  // If a value is false (i.e not possible), then the tuple with this
  // value is false too (i.e not possible). Conversely, if the tuple is
  // selected, the value must be selected.
  for (int i = 0; i < values.size(); ++i) {
    encoding_lit_to_support[encoding.at(values[i])].push_back(literals[i]);
  }

  // If all tuples supporting a value are false, then this value must be
  // false.
  for (const auto& [encoding_lit, support] : encoding_lit_to_support) {
    CHECK(!support.empty());
    if (support.size() == 1) {
      context->StoreBooleanEqualityRelation(encoding_lit, support[0]);
    } else {
      BoolArgumentProto* bool_or =
          context->working_model->add_constraints()->mutable_bool_or();
      bool_or->add_literals(NegatedRef(encoding_lit));
      for (const int lit : support) {
        bool_or->add_literals(lit);
        context->AddImplication(lit, encoding_lit);
      }
    }
  }
}

// Add the constraint literal => one_of(encoding[v]), for v in reachable_values.
// Note that all possible values are the ones appearing in encoding.
void AddImplyInReachableValues(int literal,
                               std::vector<int64_t>& reachable_values,
                               const absl::flat_hash_map<int64_t, int> encoding,
                               PresolveContext* context) {
  gtl::STLSortAndRemoveDuplicates(&reachable_values);
  if (reachable_values.size() == encoding.size()) return;  // No constraint.
  if (reachable_values.size() <= encoding.size() / 2) {
    // Bool or encoding.
    ConstraintProto* ct = context->working_model->add_constraints();
    ct->add_enforcement_literal(literal);
    BoolArgumentProto* bool_or = ct->mutable_bool_or();
    for (const int64_t v : reachable_values) {
      bool_or->add_literals(encoding.at(v));
    }
  } else {
    // Bool and encoding.
    absl::flat_hash_set<int64_t> set(reachable_values.begin(),
                                     reachable_values.end());
    ConstraintProto* ct = context->working_model->add_constraints();
    ct->add_enforcement_literal(literal);
    BoolArgumentProto* bool_and = ct->mutable_bool_and();
    for (const auto [value, literal] : encoding) {
      if (!set.contains(value)) {
        bool_and->add_literals(NegatedRef(literal));
      }
    }
  }
}

void ExpandAutomaton(ConstraintProto* ct, PresolveContext* context) {
  AutomatonConstraintProto& proto = *ct->mutable_automaton();

  if (proto.vars_size() == 0) {
    const int64_t initial_state = proto.starting_state();
    for (const int64_t final_state : proto.final_states()) {
      if (initial_state == final_state) {
        context->UpdateRuleStats("automaton: empty and trivially feasible");
        ct->Clear();
        return;
      }
    }
    return (void)context->NotifyThatModelIsUnsat(
        "automaton: empty with an initial state not in the final states.");
  } else if (proto.transition_label_size() == 0) {
    return (void)context->NotifyThatModelIsUnsat(
        "automaton: non-empty with no transition.");
  }

  std::vector<absl::flat_hash_set<int64_t>> reachable_states;
  std::vector<absl::flat_hash_set<int64_t>> reachable_labels;
  PropagateAutomaton(proto, *context, &reachable_states, &reachable_labels);

  // We will model at each time step the current automaton state using Boolean
  // variables. We will have n+1 time step. At time zero, we start in the
  // initial state, and at time n we should be in one of the final states. We
  // don't need to create Booleans at at time when there is just one possible
  // state (like at time zero).
  absl::flat_hash_map<int64_t, int> encoding;
  absl::flat_hash_map<int64_t, int> in_encoding;
  absl::flat_hash_map<int64_t, int> out_encoding;
  bool removed_values = false;

  const int n = proto.vars_size();
  const std::vector<int> vars = {proto.vars().begin(), proto.vars().end()};
  for (int time = 0; time < n; ++time) {
    // All these vector have the same size. We will use them to enforce a
    // local table constraint representing one step of the automaton at the
    // given time.
    std::vector<int64_t> in_states;
    std::vector<int64_t> labels;
    std::vector<int64_t> out_states;
    for (int i = 0; i < proto.transition_label_size(); ++i) {
      const int64_t tail = proto.transition_tail(i);
      const int64_t label = proto.transition_label(i);
      const int64_t head = proto.transition_head(i);

      if (!reachable_states[time].contains(tail)) continue;
      if (!reachable_states[time + 1].contains(head)) continue;
      if (!context->DomainContains(vars[time], label)) continue;

      // TODO(user): if this transition correspond to just one in-state or
      // one-out state or one variable value, we could reuse the corresponding
      // Boolean variable instead of creating a new one!
      in_states.push_back(tail);
      labels.push_back(label);

      // On the last step we don't need to distinguish the output states, so
      // we use zero.
      out_states.push_back(time + 1 == n ? 0 : head);
    }

    // Deal with single tuple.
    const int num_tuples = in_states.size();
    if (num_tuples == 1) {
      if (!context->IntersectDomainWith(vars[time], Domain(labels.front()))) {
        VLOG(1) << "Infeasible automaton.";
        return;
      }

      // Tricky: when the same variable is used more than once, the propagation
      // above might not reach the fixed point, so we do need to fix literal
      // at false.
      std::vector<int> at_false;
      for (const auto [value, literal] : in_encoding) {
        if (value != in_states[0]) at_false.push_back(literal);
      }
      for (const int literal : at_false) {
        if (!context->SetLiteralToFalse(literal)) return;
      }

      in_encoding.clear();
      continue;
    }

    // Fully encode vars[time].
    {
      std::vector<int64_t> transitions = labels;
      gtl::STLSortAndRemoveDuplicates(&transitions);

      encoding.clear();
      if (!context->IntersectDomainWith(
              vars[time], Domain::FromValues(transitions), &removed_values)) {
        VLOG(1) << "Infeasible automaton.";
        return;
      }

      // Fully encode the variable.
      // We can leave the encoding empty for fixed vars.
      if (!context->IsFixed(vars[time])) {
        for (const int64_t v : context->DomainOf(vars[time]).Values()) {
          encoding[v] = context->GetOrCreateVarValueEncoding(vars[time], v);
        }
      }
    }

    // Count how many time each value appear.
    // We use this to reuse literals if possible.
    absl::flat_hash_map<int64_t, int> in_count;
    absl::flat_hash_map<int64_t, int> transition_count;
    absl::flat_hash_map<int64_t, int> out_count;
    for (int i = 0; i < num_tuples; ++i) {
      in_count[in_states[i]]++;
      transition_count[labels[i]]++;
      out_count[out_states[i]]++;
    }

    // For each possible out states, create one Boolean variable.
    //
    // TODO(user): Add exactly one?
    {
      std::vector<int64_t> states = out_states;
      gtl::STLSortAndRemoveDuplicates(&states);

      out_encoding.clear();
      if (states.size() == 2) {
        const int var = context->NewBoolVar();
        out_encoding[states[0]] = var;
        out_encoding[states[1]] = NegatedRef(var);
      } else if (states.size() > 2) {
        struct UniqueDetector {
          void Set(int64_t v) {
            if (!is_unique) return;
            if (is_set) {
              if (v != value) is_unique = false;
            } else {
              is_set = true;
              value = v;
            }
          }
          bool is_set = false;
          bool is_unique = true;
          int64_t value = 0;
        };

        // Optimization to detect if we have an in state that is only matched to
        // a single out state. Same with transition.
        absl::flat_hash_map<int64_t, UniqueDetector> out_to_in;
        absl::flat_hash_map<int64_t, UniqueDetector> out_to_transition;
        for (int i = 0; i < num_tuples; ++i) {
          out_to_in[out_states[i]].Set(in_states[i]);
          out_to_transition[out_states[i]].Set(labels[i]);
        }

        for (const int64_t state : states) {
          // If we have a relation in_state <=> out_state, then we can reuse
          // the in Boolean and do not need to create a new one.
          if (!in_encoding.empty() && out_to_in[state].is_unique) {
            const int64_t unique_in = out_to_in[state].value;
            if (in_count[unique_in] == out_count[state]) {
              out_encoding[state] = in_encoding[unique_in];
              continue;
            }
          }

          // Same if we have an unique transition value that correspond only to
          // this state.
          if (!encoding.empty() && out_to_transition[state].is_unique) {
            const int64_t unique_transition = out_to_transition[state].value;
            if (transition_count[unique_transition] == out_count[state]) {
              out_encoding[state] = encoding[unique_transition];
              continue;
            }
          }

          out_encoding[state] = context->NewBoolVar();
        }
      }
    }

    // Simple encoding. This is enough to properly enforce the constraint, but
    // it propagate less. It creates a lot less Booleans though. Note that we
    // use implicit "exactly one" on the encoding and do not add any extra
    // exactly one if the simple encoding is used.
    //
    // We currently decide which encoding to use depending on the number of new
    // literals needed by the "heavy" encoding compared to the number of states
    // and labels. When the automaton is small, using the full encoding is
    // better, see for instance on rotating-workforce_Example789 were the simple
    // encoding make the problem hard to solve but the full encoding allow the
    // solver to solve it in a couple of seconds!
    //
    // Note that both encoding create about the same number of constraints.
    const int num_involved_variables =
        in_encoding.size() + encoding.size() + out_encoding.size();
    const bool use_light_encoding = (num_tuples > num_involved_variables);
    if (use_light_encoding && !in_encoding.empty() && !encoding.empty() &&
        !out_encoding.empty()) {
      // Part 1: If a in_state is selected, restrict the set of possible labels.
      // We also restrict the set of possible out states, but this is not needed
      // for correctness.
      absl::flat_hash_map<int64_t, std::vector<int64_t>> in_to_label;
      absl::flat_hash_map<int64_t, std::vector<int64_t>> in_to_out;
      for (int i = 0; i < num_tuples; ++i) {
        in_to_label[in_states[i]].push_back(labels[i]);
        in_to_out[in_states[i]].push_back(out_states[i]);
      }
      for (const auto [in_value, in_literal] : in_encoding) {
        AddImplyInReachableValues(in_literal, in_to_label[in_value], encoding,
                                  context);
        AddImplyInReachableValues(in_literal, in_to_out[in_value], out_encoding,
                                  context);
      }

      // Part2, add all 3-clauses: (in_state, label) => out_state.
      for (int i = 0; i < num_tuples; ++i) {
        auto* bool_or =
            context->working_model->add_constraints()->mutable_bool_or();
        bool_or->add_literals(NegatedRef(in_encoding.at(in_states[i])));
        bool_or->add_literals(NegatedRef(encoding.at(labels[i])));
        bool_or->add_literals(out_encoding.at(out_states[i]));
      }

      in_encoding.swap(out_encoding);
      out_encoding.clear();
      continue;
    }

    // Create the tuple literals.
    //
    // TODO(user): Call and use the same heuristics as the table constraint to
    // expand this small table with 3 columns (i.e. compress, negate, etc...).
    std::vector<int> tuple_literals;
    if (num_tuples == 2) {
      const int bool_var = context->NewBoolVar();
      tuple_literals.push_back(bool_var);
      tuple_literals.push_back(NegatedRef(bool_var));
    } else {
      // Note that we do not need the ExactlyOneConstraint(tuple_literals)
      // because it is already implicitly encoded since we have exactly one
      // transition value. But adding one seems to help.
      BoolArgumentProto* exactly_one =
          context->working_model->add_constraints()->mutable_exactly_one();
      for (int i = 0; i < num_tuples; ++i) {
        int tuple_literal;
        if (in_count[in_states[i]] == 1 && !in_encoding.empty()) {
          tuple_literal = in_encoding[in_states[i]];
        } else if (transition_count[labels[i]] == 1 && !encoding.empty()) {
          tuple_literal = encoding[labels[i]];
        } else if (out_count[out_states[i]] == 1 && !out_encoding.empty()) {
          tuple_literal = out_encoding[out_states[i]];
        } else {
          tuple_literal = context->NewBoolVar();
        }

        tuple_literals.push_back(tuple_literal);
        exactly_one->add_literals(tuple_literal);
      }
    }

    if (!in_encoding.empty()) {
      LinkLiteralsAndValues(tuple_literals, in_states, in_encoding, context);
    }
    if (!encoding.empty()) {
      LinkLiteralsAndValues(tuple_literals, labels, encoding, context);
    }
    if (!out_encoding.empty()) {
      LinkLiteralsAndValues(tuple_literals, out_states, out_encoding, context);
    }

    in_encoding.swap(out_encoding);
    out_encoding.clear();
  }

  if (removed_values) {
    context->UpdateRuleStats("automaton: reduced variable domains");
  }
  context->UpdateRuleStats("automaton: expanded");
  ct->Clear();
}

void ExpandNegativeTable(ConstraintProto* ct, PresolveContext* context) {
  TableConstraintProto& table = *ct->mutable_table();
  const int num_vars = table.vars_size();
  const int num_original_tuples = table.values_size() / num_vars;
  std::vector<std::vector<int64_t>> tuples(num_original_tuples);
  int count = 0;
  for (int i = 0; i < num_original_tuples; ++i) {
    for (int j = 0; j < num_vars; ++j) {
      tuples[i].push_back(table.values(count++));
    }
  }

  if (tuples.empty()) {  // Early exit.
    context->UpdateRuleStats("table: empty negated constraint");
    ct->Clear();
    return;
  }

  // Compress tuples.
  std::vector<int64_t> domain_sizes;
  for (int i = 0; i < num_vars; ++i) {
    domain_sizes.push_back(context->DomainOf(table.vars(i)).Size());
  }
  CompressTuples(domain_sizes, &tuples);

  // For each tuple, forbid the variables values to be this tuple.
  std::vector<int> clause;
  for (const std::vector<int64_t>& tuple : tuples) {
    clause.clear();
    for (int i = 0; i < num_vars; ++i) {
      const int64_t value = tuple[i];
      if (value == kTableAnyValue) continue;

      const int literal =
          context->GetOrCreateVarValueEncoding(table.vars(i), value);
      clause.push_back(NegatedRef(literal));
    }

    // Note: if the clause is empty, then the model is infeasible.
    BoolArgumentProto* bool_or =
        context->working_model->add_constraints()->mutable_bool_or();
    for (const int lit : clause) {
      bool_or->add_literals(lit);
    }
  }
  context->UpdateRuleStats("table: expanded negated constraint");
  ct->Clear();
}

// Add the implications and clauses to link one variable (i.e. column) of a
// table to the literals controlling if the tuples are possible or not.
//
// We list of each tuple the possible values the variable can take.
// If the list is empty, then this encode "any value".
void ProcessOneCompressedColumn(
    int variable, const std::vector<int>& tuple_literals,
    const std::vector<absl::InlinedVector<int64_t, 2>>& values,
    PresolveContext* context) {
  DCHECK_EQ(tuple_literals.size(), values.size());

  // Collect pairs of value-literal.
  // Add the constraint literal => one of values.
  //
  // TODO(user): If we have n - 1 values, we could add the constraint that
  // tuple literal => not(last_value) instead?
  std::vector<std::pair<int64_t, int>> pairs;
  std::vector<int> any_values_literals;
  for (int i = 0; i < values.size(); ++i) {
    if (values[i].empty()) {
      any_values_literals.push_back(tuple_literals[i]);
      continue;
    }
    ConstraintProto* clause = context->working_model->add_constraints();
    clause->add_enforcement_literal(tuple_literals[i]);
    for (const int64_t v : values[i]) {
      DCHECK(context->DomainContains(variable, v));
      clause->mutable_bool_or()->add_literals(
          context->GetOrCreateVarValueEncoding(variable, v));
      pairs.emplace_back(v, tuple_literals[i]);
    }
  }

  // Regroup literal with the same value and add for each the clause: If all the
  // tuples containing a value are false, then this value must be false too.
  std::vector<int> selected;
  std::sort(pairs.begin(), pairs.end());
  for (int i = 0; i < pairs.size();) {
    selected.clear();
    const int64_t value = pairs[i].first;
    for (; i < pairs.size() && pairs[i].first == value; ++i) {
      selected.push_back(pairs[i].second);
    }

    BoolArgumentProto* no_support =
        context->working_model->add_constraints()->mutable_bool_or();
    for (const int lit : selected) {
      no_support->add_literals(lit);
    }
    for (const int lit : any_values_literals) {
      no_support->add_literals(lit);
    }

    // And the "value" literal.
    const int value_literal =
        context->GetOrCreateVarValueEncoding(variable, value);
    no_support->add_literals(NegatedRef(value_literal));
  }
}

// Simpler encoding for table constraints with 2 variables.
void AddSizeTwoTable(
    const std::vector<int>& vars,
    const std::vector<std::vector<int64_t>>& tuples,
    const std::vector<absl::flat_hash_set<int64_t>>& values_per_var,
    PresolveContext* context) {
  CHECK_EQ(vars.size(), 2);
  const int left_var = vars[0];
  const int right_var = vars[1];
  if (context->DomainOf(left_var).IsFixed() ||
      context->DomainOf(right_var).IsFixed()) {
    // A table constraint with at most one variable not fixed is trivially
    // enforced after domain reduction.
    return;
  }

  absl::btree_map<int, std::vector<int>> left_to_right;
  absl::btree_map<int, std::vector<int>> right_to_left;

  for (const auto& tuple : tuples) {
    const int64_t left_value(tuple[0]);
    const int64_t right_value(tuple[1]);
    DCHECK(context->DomainContains(left_var, left_value));
    DCHECK(context->DomainContains(right_var, right_value));

    const int left_literal =
        context->GetOrCreateVarValueEncoding(left_var, left_value);
    const int right_literal =
        context->GetOrCreateVarValueEncoding(right_var, right_value);
    left_to_right[left_literal].push_back(right_literal);
    right_to_left[right_literal].push_back(left_literal);
  }

  int num_implications = 0;
  int num_clause_added = 0;
  int num_large_clause_added = 0;
  auto add_support_constraint =
      [context, &num_clause_added, &num_large_clause_added, &num_implications](
          int lit, const std::vector<int>& support_literals,
          int max_support_size) {
        if (support_literals.size() == max_support_size) return;
        if (support_literals.size() == 1) {
          context->AddImplication(lit, support_literals.front());
          num_implications++;
        } else {
          BoolArgumentProto* bool_or =
              context->working_model->add_constraints()->mutable_bool_or();
          for (const int support_literal : support_literals) {
            bool_or->add_literals(support_literal);
          }
          bool_or->add_literals(NegatedRef(lit));
          num_clause_added++;
          if (support_literals.size() > max_support_size / 2) {
            num_large_clause_added++;
          }
        }
      };

  for (const auto& it : left_to_right) {
    add_support_constraint(it.first, it.second, values_per_var[1].size());
  }
  for (const auto& it : right_to_left) {
    add_support_constraint(it.first, it.second, values_per_var[0].size());
  }
  VLOG(2) << "Table: 2 variables, " << tuples.size() << " tuples encoded using "
          << num_clause_added << " clauses, including "
          << num_large_clause_added << " large clauses, " << num_implications
          << " implications";
}

// A "WCSP" (weighted constraint programming) problem is usually encoded as
// a set of table, with one or more variable only there to carry a cost.
//
// If this is the case, we can do special presolving.
bool ReduceTableInPresenceOfUniqueVariableWithCosts(
    std::vector<int>* vars, std::vector<std::vector<int64_t>>* tuples,
    PresolveContext* context) {
  const int num_vars = vars->size();

  std::vector<bool> only_here_and_in_objective(num_vars, false);
  std::vector<int64_t> objective_coeffs(num_vars, 0.0);
  std::vector<int> new_vars;
  std::vector<int> deleted_vars;
  for (int var_index = 0; var_index < num_vars; ++var_index) {
    const int var = (*vars)[var_index];
    // We do not use VariableWithCostIsUniqueAndRemovable() since this one
    // return false if the objective is constraining but we don't care here.
    if (context->VariableWithCostIsUniqueAndRemovable(var)) {
      context->UpdateRuleStats("table: removed unused column with cost");
      only_here_and_in_objective[var_index] = true;
      objective_coeffs[var_index] =
          RefIsPositive(var) ? context->ObjectiveMap().at(var)
                             : -context->ObjectiveMap().at(PositiveRef(var));
      context->RemoveVariableFromObjective(var);
      context->MarkVariableAsRemoved(var);
      deleted_vars.push_back(var);
    } else if (context->VariableIsUniqueAndRemovable(var)) {
      // If there is no cost, we can remove that variable using the same code by
      // just setting the cost to zero.
      context->UpdateRuleStats("table: removed unused column");
      only_here_and_in_objective[var_index] = true;
      objective_coeffs[var_index] = 0;
      context->MarkVariableAsRemoved(var);
      deleted_vars.push_back(var);
    } else {
      new_vars.push_back(var);
    }
  }
  if (new_vars.size() == num_vars) return false;

  // Rewrite the tuples.
  // put the cost last.
  int64_t min_cost = std::numeric_limits<int64_t>::max();
  std::vector<int64_t> temp;
  for (int i = 0; i < tuples->size(); ++i) {
    int64_t cost = 0;
    int new_size = 0;
    temp.clear();
    for (int var_index = 0; var_index < num_vars; ++var_index) {
      const int64_t value = (*tuples)[i][var_index];
      if (only_here_and_in_objective[var_index]) {
        temp.push_back(value);
        const int64_t objective_coeff = objective_coeffs[var_index];
        cost += value * objective_coeff;
      } else {
        (*tuples)[i][new_size++] = value;
      }
    }
    (*tuples)[i].resize(new_size);
    (*tuples)[i].push_back(cost);
    min_cost = std::min(min_cost, cost);

    // Hack: we store the deleted value here so that we can properly encode
    // the postsolve constraints below.
    (*tuples)[i].insert((*tuples)[i].end(), temp.begin(), temp.end());
  }

  // Remove tuples that only differ by their cost.
  // Make sure we will assign the proper value of the removed variable at
  // postsolve.
  {
    int new_size = 0;
    const int old_size = tuples->size();
    std::sort(tuples->begin(), tuples->end());
    for (int i = 0; i < tuples->size(); ++i) {
      // If the prefix (up to new_vars.size()) is the same, skip this tuple.
      if (new_size > 0) {
        bool skip = true;
        for (int var_index = 0; var_index < new_vars.size(); ++var_index) {
          if ((*tuples)[i][var_index] != (*tuples)[new_size - 1][var_index]) {
            skip = false;
            break;
          }
        }
        if (skip) continue;
      }

      // If this tuple is selected, then fix the removed variable value in the
      // mapping model.
      for (int j = 0; j < deleted_vars.size(); ++j) {
        ConstraintProto* new_ct = context->mapping_model->add_constraints();
        for (int var_index = 0; var_index < new_vars.size(); ++var_index) {
          new_ct->add_enforcement_literal(context->GetOrCreateVarValueEncoding(
              new_vars[var_index], (*tuples)[i][var_index]));
        }
        new_ct->mutable_linear()->add_vars(deleted_vars[j]);
        new_ct->mutable_linear()->add_coeffs(1);
        new_ct->mutable_linear()->add_domain(
            (*tuples)[i][new_vars.size() + 1 + j]);
        new_ct->mutable_linear()->add_domain(
            (*tuples)[i][new_vars.size() + 1 + j]);
      }
      (*tuples)[i].resize(new_vars.size() + 1);
      (*tuples)[new_size++] = (*tuples)[i];
    }
    tuples->resize(new_size);
    if (new_size < old_size) {
      context->UpdateRuleStats(
          "table: removed duplicate tuples with different costs");
    }
  }

  if (min_cost > 0) {
    context->AddToObjectiveOffset(min_cost);
    context->UpdateRuleStats("table: transferred min_cost to objective offset");
    for (int i = 0; i < tuples->size(); ++i) {
      (*tuples)[i].back() -= min_cost;
    }
  }

  // This comes from the WCSP litterature. Basically, if by fixing a variable to
  // a value, we have only tuples with a non-zero cost, we can substract the
  // minimum cost of these tuples and transfer it to the variable cost.
  for (int var_index = 0; var_index < new_vars.size(); ++var_index) {
    absl::flat_hash_map<int64_t, int64_t> value_to_min_cost;
    const int num_tuples = tuples->size();
    for (int i = 0; i < num_tuples; ++i) {
      const int64_t v = (*tuples)[i][var_index];
      const int64_t cost = (*tuples)[i].back();
      auto insert = value_to_min_cost.insert({v, cost});
      if (!insert.second) {
        insert.first->second = std::min(insert.first->second, cost);
      }
    }
    for (int i = 0; i < num_tuples; ++i) {
      const int64_t v = (*tuples)[i][var_index];
      (*tuples)[i].back() -= value_to_min_cost.at(v);
    }
    for (const auto entry : value_to_min_cost) {
      if (entry.second == 0) continue;
      context->UpdateRuleStats("table: transferred cost to encoding");
      const int value_literal = context->GetOrCreateVarValueEncoding(
          new_vars[var_index], entry.first);
      context->AddLiteralToObjective(value_literal, entry.second);
    }
  }

  context->UpdateRuleStats(absl::StrCat(
      "table: expansion with column(s) only in objective. Arity = ",
      new_vars.size()));

  *vars = new_vars;
  return true;
}

// Important: the table and variable domains must be presolved before this
// is called. Some checks will fail otherwise.
void CompressAndExpandPositiveTable(bool last_column_is_cost,
                                    const std::vector<int>& vars,
                                    std::vector<std::vector<int64_t>>* tuples,
                                    PresolveContext* context) {
  const int num_tuples_before_compression = tuples->size();

  // If the last column is actually the tuple cost, we compress the table like
  // if this was a normal variable, but afterwards we treat it differently.
  std::vector<int64_t> domain_sizes;
  for (const int var : vars) {
    domain_sizes.push_back(context->DomainOf(var).Size());
  }
  if (last_column_is_cost) {
    domain_sizes.push_back(std::numeric_limits<int64_t>::max());
  }

  // We start by compressing the table with kTableAnyValue only.
  const int compression_level = context->params().table_compression_level();
  if (compression_level > 0) {
    CompressTuples(domain_sizes, tuples);
  }
  const int num_tuples_after_first_compression = tuples->size();

  // Tricky: If the table is big, it is better to compress it as much as
  // possible to reduce the number of created booleans. Otherwise, the more
  // verbose encoding can lead to better linear relaxation. Probably because the
  // tuple literal can encode each variable as sum literal * value. Also because
  // we have more direct implied bounds, which might lead to better cuts.
  //
  // For instance, on lot_sizing_cp_pigment15c.psp, compressing the table more
  // is a lot worse (at least until we can produce better cut).
  //
  // TODO(user): Tweak the heuristic, maybe compute the reduction achieve and
  // decide based on that.
  std::vector<std::vector<absl::InlinedVector<int64_t, 2>>> compressed_table;
  if (compression_level > 2 ||
      (compression_level == 2 && num_tuples_after_first_compression > 1000)) {
    compressed_table = FullyCompressTuples(domain_sizes, tuples);
    if (compressed_table.size() < num_tuples_before_compression) {
      context->UpdateRuleStats("table: fully compress tuples");
    }
  } else {
    // Convert the kTableAnyValue to an empty list format.
    for (int i = 0; i < tuples->size(); ++i) {
      compressed_table.push_back({});
      for (const int64_t v : (*tuples)[i]) {
        if (v == kTableAnyValue) {
          compressed_table.back().push_back({});
        } else {
          compressed_table.back().push_back({v});
        }
      }
    }
    if (compressed_table.size() < num_tuples_before_compression) {
      context->UpdateRuleStats("table: compress tuples");
    }
  }

  VLOG(2) << "Table compression"
          << " var=" << vars.size()
          << " cost=" << domain_sizes.size() - vars.size()
          << " tuples= " << num_tuples_before_compression << " -> "
          << num_tuples_after_first_compression << " -> "
          << compressed_table.size();

  // Affect mznc2017_aes_opt_r10 instance!
  std::sort(compressed_table.begin(), compressed_table.end());

  const int num_vars = vars.size();
  if (compressed_table.size() == 1) {
    // Domains are propagated. We can remove the constraint.
    context->UpdateRuleStats("table: one tuple");
    if (last_column_is_cost) {
      // TODO(user): Because we transfer the cost, this should always be zero,
      // so not needed.
      context->AddToObjectiveOffset(compressed_table[0].back()[0]);
    }
    return;
  }

  // Optimization. If a value is unique and appear alone in a cell, we can use
  // the encoding literal for this line tuple literal instead of creating a new
  // one.
  std::vector<bool> has_any(num_vars, false);
  std::vector<absl::flat_hash_map<int64_t, int>> var_index_to_value_count(
      num_vars);
  for (int i = 0; i < compressed_table.size(); ++i) {
    for (int var_index = 0; var_index < num_vars; ++var_index) {
      if (compressed_table[i][var_index].empty()) {
        has_any[var_index] = true;
        continue;
      }
      for (const int64_t v : compressed_table[i][var_index]) {
        DCHECK_NE(v, kTableAnyValue);
        DCHECK(context->DomainContains(vars[var_index], v));
        var_index_to_value_count[var_index][v]++;
      }
    }
  }

  // Create one Boolean variable per tuple to indicate if it can still be
  // selected or not. Enforce an exactly one between them.
  BoolArgumentProto* exactly_one =
      context->working_model->add_constraints()->mutable_exactly_one();

  int64_t num_reused_variables = 0;
  std::vector<int> tuple_literals(compressed_table.size());
  for (int i = 0; i < compressed_table.size(); ++i) {
    bool create_new_var = true;
    for (int var_index = 0; var_index < num_vars; ++var_index) {
      if (has_any[var_index]) continue;
      if (compressed_table[i][var_index].size() != 1) continue;
      const int64_t v = compressed_table[i][var_index][0];
      if (var_index_to_value_count[var_index][v] != 1) continue;

      ++num_reused_variables;
      create_new_var = false;
      tuple_literals[i] =
          context->GetOrCreateVarValueEncoding(vars[var_index], v);
      break;
    }
    if (create_new_var) {
      tuple_literals[i] = context->NewBoolVar();
    }
    exactly_one->add_literals(tuple_literals[i]);
  }
  if (num_reused_variables > 0) {
    context->UpdateRuleStats("table: reused literals");
  }

  // Set the cost to the corresponding tuple literal. If there is more than one
  // cost, we just choose the first one which is the smallest one.
  if (last_column_is_cost) {
    for (int i = 0; i < tuple_literals.size(); ++i) {
      context->AddLiteralToObjective(tuple_literals[i],
                                     compressed_table[i].back()[0]);
    }
  }

  std::vector<absl::InlinedVector<int64_t, 2>> column;
  for (int var_index = 0; var_index < num_vars; ++var_index) {
    if (context->IsFixed(vars[var_index])) continue;

    column.clear();
    for (int i = 0; i < tuple_literals.size(); ++i) {
      column.push_back(compressed_table[i][var_index]);
    }
    ProcessOneCompressedColumn(vars[var_index], tuple_literals, column,
                               context);
  }

  context->UpdateRuleStats("table: expanded positive constraint");
}

// TODO(user): reinvestigate ExploreSubsetOfVariablesAndAddNegatedTables.
//
// TODO(user): if 2 table constraints share the same valid prefix, the
// tuple literals can be reused.
//
// TODO(user): investigate different encoding for prefix tables. Maybe
// we can remove the need to create tuple literals.
void ExpandPositiveTable(ConstraintProto* ct, PresolveContext* context) {
  const TableConstraintProto& table = ct->table();
  const int num_vars = table.vars_size();
  const int num_original_tuples = table.values_size() / num_vars;

  // Read tuples flat array and recreate the vector of tuples.
  std::vector<int> vars(table.vars().begin(), table.vars().end());
  std::vector<std::vector<int64_t>> tuples(num_original_tuples);
  int count = 0;
  for (int tuple_index = 0; tuple_index < num_original_tuples; ++tuple_index) {
    for (int var_index = 0; var_index < num_vars; ++var_index) {
      tuples[tuple_index].push_back(table.values(count++));
    }
  }

  // Compute the set of possible values for each variable (from the table).
  // Remove invalid tuples along the way.
  std::vector<absl::flat_hash_set<int64_t>> values_per_var(num_vars);
  int new_size = 0;
  for (int tuple_index = 0; tuple_index < num_original_tuples; ++tuple_index) {
    bool keep = true;
    for (int var_index = 0; var_index < num_vars; ++var_index) {
      const int64_t value = tuples[tuple_index][var_index];
      if (!context->DomainContains(vars[var_index], value)) {
        keep = false;
        break;
      }
    }
    if (keep) {
      for (int var_index = 0; var_index < num_vars; ++var_index) {
        values_per_var[var_index].insert(tuples[tuple_index][var_index]);
      }
      std::swap(tuples[tuple_index], tuples[new_size]);
      new_size++;
    }
  }
  tuples.resize(new_size);

  if (tuples.empty()) {
    context->UpdateRuleStats("table: empty");
    return (void)context->NotifyThatModelIsUnsat();
  }

  // Update variable domains. It is redundant with presolve, but we could be
  // here with presolve = false.
  // Also counts the number of fixed variables.
  int num_fixed_variables = 0;
  for (int var_index = 0; var_index < num_vars; ++var_index) {
    CHECK(context->IntersectDomainWith(
        vars[var_index],
        Domain::FromValues({values_per_var[var_index].begin(),
                            values_per_var[var_index].end()})));
    if (context->DomainOf(vars[var_index]).IsFixed()) {
      num_fixed_variables++;
    }
  }

  if (num_fixed_variables == num_vars - 1) {
    context->UpdateRuleStats("table: one variable not fixed");
    ct->Clear();
    return;
  } else if (num_fixed_variables == num_vars) {
    context->UpdateRuleStats("table: all variables fixed");
    ct->Clear();
    return;
  }

  // Tables with two variables do not need tuple literals.
  //
  // TODO(user): If there is an unique variable with cost, it is better to
  // detect it. But if the detection fail, we should still call
  // AddSizeTwoTable() unlike what happen here.
  if (num_vars == 2 && !context->params().detect_table_with_cost()) {
    AddSizeTwoTable(vars, tuples, values_per_var, context);
    context->UpdateRuleStats(
        "table: expanded positive constraint with two variables");
    ct->Clear();
    return;
  }

  bool last_column_is_cost = false;
  if (context->params().detect_table_with_cost()) {
    last_column_is_cost =
        ReduceTableInPresenceOfUniqueVariableWithCosts(&vars, &tuples, context);
  }

  CompressAndExpandPositiveTable(last_column_is_cost, vars, &tuples, context);
  ct->Clear();
}

bool AllDiffShouldBeExpanded(const Domain& union_of_domains,
                             ConstraintProto* ct, PresolveContext* context) {
  const AllDifferentConstraintProto& proto = *ct->mutable_all_diff();
  const int num_exprs = proto.exprs_size();
  int num_fully_encoded = 0;
  for (int i = 0; i < num_exprs; ++i) {
    if (context->IsFullyEncoded(proto.exprs(i))) {
      num_fully_encoded++;
    }
  }

  if ((union_of_domains.Size() <= 2 * proto.exprs_size()) ||
      (union_of_domains.Size() <= 32)) {
    // Small domains.
    return true;
  }

  if (num_fully_encoded == num_exprs && union_of_domains.Size() < 256) {
    // All variables fully encoded, and domains are small enough.
    return true;
  }
  return false;
}

// Replaces a constraint literal => ax + by != cte by a set of clauses.
// This is performed if the domains are small enough, and the variables are
// fully encoded.
//
// We do it during the expansion as we want the first pass of the presolve to be
// complete.
void ExpandSomeLinearOfSizeTwo(ConstraintProto* ct, PresolveContext* context) {
  const LinearConstraintProto& arg = ct->linear();
  if (arg.vars_size() != 2) return;

  const int var1 = arg.vars(0);
  const int var2 = arg.vars(1);
  if (context->IsFixed(var1) || context->IsFixed(var2)) return;

  const int64_t coeff1 = arg.coeffs(0);
  const int64_t coeff2 = arg.coeffs(1);
  const Domain reachable_rhs_superset =
      context->DomainOf(var1)
          .MultiplicationBy(coeff1)
          .RelaxIfTooComplex()
          .AdditionWith(context->DomainOf(var2)
                            .MultiplicationBy(coeff2)
                            .RelaxIfTooComplex());
  const Domain infeasible_reachable_values =
      reachable_rhs_superset.IntersectionWith(
          ReadDomainFromProto(arg).Complement());

  // We only deal with != cte constraints.
  if (infeasible_reachable_values.Size() != 1) return;

  // coeff1 * v1 + coeff2 * v2 != cte.
  int64_t a = coeff1;
  int64_t b = coeff2;
  int64_t cte = infeasible_reachable_values.FixedValue();
  int64_t x0 = 0;
  int64_t y0 = 0;
  if (!SolveDiophantineEquationOfSizeTwo(a, b, cte, x0, y0)) {
    // no solution.
    context->UpdateRuleStats("linear: expand always feasible ax + by != cte");
    ct->Clear();
    return;
  }
  const Domain reduced_domain =
      context->DomainOf(var1)
          .AdditionWith(Domain(-x0))
          .InverseMultiplicationBy(b)
          .IntersectionWith(context->DomainOf(var2)
                                .AdditionWith(Domain(-y0))
                                .InverseMultiplicationBy(-a));

  if (reduced_domain.Size() > 16) return;

  // Check if all the needed values are encoded.
  // TODO(user): Do we force encoding for very small domains? Current
  // experiments says no, but revisit later.
  const int64_t size1 = context->DomainOf(var1).Size();
  const int64_t size2 = context->DomainOf(var2).Size();
  for (const int64_t z : reduced_domain.Values()) {
    const int64_t value1 = x0 + b * z;
    const int64_t value2 = y0 - a * z;
    DCHECK(context->DomainContains(var1, value1)) << "value1 = " << value1;
    DCHECK(context->DomainContains(var2, value2)) << "value2 = " << value2;
    DCHECK_EQ(coeff1 * value1 + coeff2 * value2,
              infeasible_reachable_values.FixedValue());
    // TODO(user): Presolve if one or two variables are Boolean.
    if (!context->HasVarValueEncoding(var1, value1, nullptr) || size1 == 2) {
      return;
    }
    if (!context->HasVarValueEncoding(var2, value2, nullptr) || size2 == 2) {
      return;
    }
  }

  // All encoding literals already exist and the number of clauses to create
  // is small enough. We can encode the constraint using just clauses.
  for (const int64_t z : reduced_domain.Values()) {
    const int64_t value1 = x0 + b * z;
    const int64_t value2 = y0 - a * z;
    // We cannot have both lit1 and lit2 true.
    const int lit1 = context->GetOrCreateVarValueEncoding(var1, value1);
    const int lit2 = context->GetOrCreateVarValueEncoding(var2, value2);
    auto* bool_or =
        context->working_model->add_constraints()->mutable_bool_or();
    bool_or->add_literals(NegatedRef(lit1));
    bool_or->add_literals(NegatedRef(lit2));
    for (const int lit : ct->enforcement_literal()) {
      bool_or->add_literals(NegatedRef(lit));
    }
  }

  context->UpdateRuleStats("linear: expand small ax + by != cte");
  ct->Clear();
}

// Note that we used to do that at loading time, but we prefer to do that as
// part of the presolve so that all variables are available for sharing between
// subworkers and also are accessible by the linear relaxation.
//
// TODO(user): Note that currently both encoding introduce extra solutions
// if the constraint has some enforcement literal(). We can either fix this by
// supporting enumeration on a subset of variable. Or add extra constraint to
// fix all new Boolean to false if the initial constraint is not enforced.
void ExpandComplexLinearConstraint(int c, ConstraintProto* ct,
                                   PresolveContext* context) {
  // TODO(user): We treat the linear of size 1 differently because we need them
  // as is to recognize value encoding. Try to still creates needed Boolean now
  // so that we can share more between the different workers. Or revisit how
  // linear1 are propagated.
  if (ct->linear().domain().size() <= 2) return;
  if (ct->linear().vars().size() == 1) return;

  const SatParameters& params = context->params();
  if (params.encode_complex_linear_constraint_with_integer()) {
    // Integer encoding.
    //
    // Here we add a slack with domain equal to rhs and transform
    // expr \in rhs to expr - slack = 0
    const Domain rhs = ReadDomainFromProto(ct->linear());
    const int slack = context->NewIntVar(rhs);
    ct->mutable_linear()->add_vars(slack);
    ct->mutable_linear()->add_coeffs(-1);
    ct->mutable_linear()->clear_domain();
    ct->mutable_linear()->add_domain(0);
    ct->mutable_linear()->add_domain(0);
  } else {
    // Boolean encoding.
    int single_bool;
    BoolArgumentProto* clause = nullptr;
    std::vector<int> domain_literals;
    if (ct->enforcement_literal().empty() && ct->linear().domain_size() == 4) {
      // We cover the special case of no enforcement and two choices by creating
      // a single Boolean.
      single_bool = context->NewBoolVar();
    } else {
      clause = context->working_model->add_constraints()->mutable_bool_or();
      for (const int ref : ct->enforcement_literal()) {
        clause->add_literals(NegatedRef(ref));
      }
    }

    // Save enforcement literals for the enumeration.
    const std::vector<int> enforcement_literals(
        ct->enforcement_literal().begin(), ct->enforcement_literal().end());
    ct->mutable_enforcement_literal()->Clear();
    for (int i = 0; i < ct->linear().domain_size(); i += 2) {
      const int64_t lb = ct->linear().domain(i);
      const int64_t ub = ct->linear().domain(i + 1);

      int subdomain_literal;
      if (clause != nullptr) {
        subdomain_literal = context->NewBoolVar();
        clause->add_literals(subdomain_literal);
        domain_literals.push_back(subdomain_literal);
      } else {
        if (i == 0) domain_literals.push_back(single_bool);
        subdomain_literal = i == 0 ? single_bool : NegatedRef(single_bool);
      }

      // Create a new constraint which is a copy of the original, but with a
      // simple sub-domain and enforcement literal.
      ConstraintProto* new_ct = context->working_model->add_constraints();
      *new_ct = *ct;
      new_ct->add_enforcement_literal(subdomain_literal);
      FillDomainInProto(Domain(lb, ub), new_ct->mutable_linear());
    }

    // Make sure all booleans are tights when enumerating all solutions.
    if (context->params().enumerate_all_solutions() &&
        !enforcement_literals.empty()) {
      int linear_is_enforced;
      if (enforcement_literals.size() == 1) {
        linear_is_enforced = enforcement_literals[0];
      } else {
        linear_is_enforced = context->NewBoolVar();
        BoolArgumentProto* maintain_linear_is_enforced =
            context->working_model->add_constraints()->mutable_bool_or();
        for (const int e_lit : enforcement_literals) {
          context->AddImplication(NegatedRef(e_lit),
                                  NegatedRef(linear_is_enforced));
          maintain_linear_is_enforced->add_literals(NegatedRef(e_lit));
        }
        maintain_linear_is_enforced->add_literals(linear_is_enforced);
      }

      for (const int lit : domain_literals) {
        context->AddImplication(NegatedRef(linear_is_enforced),
                                NegatedRef(lit));
      }
    }
    ct->Clear();
  }

  context->UpdateRuleStats("linear: expanded complex rhs");
  context->InitializeNewDomains();
  context->UpdateNewConstraintsVariableUsage();
  context->UpdateConstraintVariableUsage(c);
}

bool IsVarEqOrNeqValue(PresolveContext* context,
                       const LinearConstraintProto& lin) {
  if (lin.vars_size() != 1) return false;
  const Domain rhs = ReadDomainFromProto(lin);
  if (rhs.IsFixed()) return true;
  return rhs.InverseMultiplicationBy(lin.coeffs(0))
      .Complement()
      .IntersectionWith(context->DomainOf(lin.vars(0)))
      .IsFixed();
}

// This method will scan all constraints of all variables appearing in an
// all_diff.
// There are 3 outcomes:
//    - maybe expand to Boolean variables (depending on the size)
//    - keep integer all_different constraint (and cuts)
//    - expand and keep
//
// Expand is selected if the variable is fully encoded, or will be when
//   expanding other constraints: index of element, table, automaton.
//   It will check AllDiffShouldBeExpanded() before doing the actual expansion.
// Keep is forced is the variable appears in a linear equation with at least 3
// terms, and with a tight domain ( == cst).
// TODO(user): The above rule is complex. Revisit.
void ScanModelAndDecideAllDiffExpansion(
    ConstraintProto* all_diff_ct, PresolveContext* context,
    absl::flat_hash_set<int>& domain_of_var_is_used,
    absl::flat_hash_set<int>& bounds_of_var_are_used,
    absl::flat_hash_set<int>& processed_variables, bool& expand, bool& keep) {
  CHECK_EQ(all_diff_ct->constraint_case(), ConstraintProto::kAllDiff);

  bool at_least_one_var_domain_is_used = false;
  bool at_least_one_var_bound_is_used = false;

  // Scan variables.
  for (const LinearExpressionProto& expr : all_diff_ct->all_diff().exprs()) {
    // Skip constant expressions.
    if (expr.vars().empty()) continue;
    DCHECK_EQ(1, expr.vars_size());
    const int var = expr.vars(0);
    DCHECK(RefIsPositive(var));
    if (context->IsFixed(var)) continue;

    bool at_least_one_var_domain_is_used = false;
    bool at_least_one_var_bound_is_used = false;

    // Check cache.
    if (!processed_variables.insert(var).second) {
      at_least_one_var_domain_is_used = bounds_of_var_are_used.contains(var);
      at_least_one_var_bound_is_used = domain_of_var_is_used.contains(var);
    } else {
      bool domain_is_used = false;
      bool bounds_are_used = false;

      // Note: Boolean constraints are ignored.
      for (const int ct_index : context->VarToConstraints(var)) {
        // Skip artificial constraints.
        if (ct_index < 0) continue;

        const ConstraintProto& other_ct =
            context->working_model->constraints(ct_index);
        switch (other_ct.constraint_case()) {
          case ConstraintProto::ConstraintCase::kBoolOr:
            break;
          case ConstraintProto::ConstraintCase::kBoolAnd:
            break;
          case ConstraintProto::ConstraintCase::kAtMostOne:
            break;
          case ConstraintProto::ConstraintCase::kExactlyOne:
            break;
          case ConstraintProto::ConstraintCase::kBoolXor:
            break;
          case ConstraintProto::ConstraintCase::kIntDiv:
            break;
          case ConstraintProto::ConstraintCase::kIntMod:
            break;
          case ConstraintProto::ConstraintCase::kLinMax:
            bounds_are_used = true;
            break;
          case ConstraintProto::ConstraintCase::kIntProd:
            break;
          case ConstraintProto::ConstraintCase::kLinear:
            if (IsVarEqOrNeqValue(context, other_ct.linear()) &&
                var == other_ct.linear().vars(0)) {
              // Encoding literals.
              domain_is_used = true;
            } else if (other_ct.linear().vars_size() > 2 &&
                       other_ct.linear().domain_size() == 2 &&
                       other_ct.linear().domain(0) ==
                           other_ct.linear().domain(1)) {
              // We assume all_diff cuts will only be useful if the linear
              // constraint has a fixed domain.
              bounds_are_used = true;
            }
            break;
          case ConstraintProto::ConstraintCase::kAllDiff:
            // We ignore all_diffs as we are trying to decide their expansion
            // from the rest of the model.
            break;
          case ConstraintProto::ConstraintCase::kDummyConstraint:
            break;
          case ConstraintProto::ConstraintCase::kElement:
            // Note: elements should have been expanded.
            if (other_ct.element().index() == var) {
              domain_is_used = true;
            }
            break;
          case ConstraintProto::ConstraintCase::kCircuit:
            break;
          case ConstraintProto::ConstraintCase::kRoutes:
            break;
          case ConstraintProto::ConstraintCase::kInverse:
            domain_is_used = true;
            break;
          case ConstraintProto::ConstraintCase::kReservoir:
            break;
          case ConstraintProto::ConstraintCase::kTable:
            domain_is_used = true;
            break;
          case ConstraintProto::ConstraintCase::kAutomaton:
            domain_is_used = true;
            break;
          case ConstraintProto::ConstraintCase::kInterval:
            bounds_are_used = true;
            break;
          case ConstraintProto::ConstraintCase::kNoOverlap:
            // Will be covered by the interval case.
            break;
          case ConstraintProto::ConstraintCase::kNoOverlap2D:
            // Will be covered by the interval case.
            break;
          case ConstraintProto::ConstraintCase::kCumulative:
            // Will be covered by the interval case.
            break;
          case ConstraintProto::ConstraintCase::CONSTRAINT_NOT_SET:
            break;
        }

        // Exit early.
        if (domain_is_used && bounds_are_used) break;
      }  // Loop on other_ct.

      // Update cache.
      if (domain_is_used) domain_of_var_is_used.insert(var);
      if (bounds_are_used) bounds_of_var_are_used.insert(var);

      // Update the usage of the variable.
      at_least_one_var_domain_is_used |= domain_is_used;
      at_least_one_var_bound_is_used |= bounds_are_used;
    }  // End of model scanning.

    if (at_least_one_var_domain_is_used && at_least_one_var_bound_is_used) {
      break;  // No need to scan the rest of the all_diff.
    }
  }  // End of var processing.

  expand = at_least_one_var_domain_is_used;
  keep = at_least_one_var_bound_is_used;
}

void MaybeExpandAllDiff(ConstraintProto* ct, PresolveContext* context,
                        absl::flat_hash_set<int>& domain_of_var_is_used,
                        absl::flat_hash_set<int>& bounds_of_var_are_used,
                        absl::flat_hash_set<int>& processed_variable) {
  const bool expand_all_diff_from_parameters =
      context->params().expand_alldiff_constraints();
  AllDifferentConstraintProto& proto = *ct->mutable_all_diff();
  if (proto.exprs_size() <= 1) return;

  bool keep_after_expansion = false;
  bool expand_all_diff_from_usage = false;
  ScanModelAndDecideAllDiffExpansion(
      ct, context, domain_of_var_is_used, bounds_of_var_are_used,
      processed_variable, expand_all_diff_from_usage, keep_after_expansion);

  const int num_exprs = proto.exprs_size();
  Domain union_of_domains = context->DomainSuperSetOf(proto.exprs(0));
  for (int i = 1; i < num_exprs; ++i) {
    union_of_domains =
        union_of_domains.UnionWith(context->DomainSuperSetOf(proto.exprs(i)));
  }

  const bool expand_all_diff_from_size =
      AllDiffShouldBeExpanded(union_of_domains, ct, context);

  // Decide expansion:
  //  - always expand if expand_all_diff_from_parameters
  //  - expand if size is compatible (expand_all_diff_from_size) and
  //    expansion is desired:
  //       expand_all_diff_from_usage || !keep_after_expansion
  const bool should_expand =
      expand_all_diff_from_parameters ||
      (expand_all_diff_from_size &&
       (expand_all_diff_from_usage || !keep_after_expansion));
  if (!should_expand) return;

  const bool is_a_permutation = num_exprs == union_of_domains.Size();

  // Collect all possible variables that can take each value, and add one linear
  // equation per value stating that this value can be assigned at most once, or
  // exactly once in case of permutation.
  for (const int64_t v : union_of_domains.Values()) {
    // Collect references which domain contains v.
    std::vector<LinearExpressionProto> possible_exprs;
    int fixed_expression_count = 0;
    for (const LinearExpressionProto& expr : proto.exprs()) {
      if (!context->DomainContains(expr, v)) continue;
      possible_exprs.push_back(expr);
      if (context->IsFixed(expr)) {
        fixed_expression_count++;
      }
    }

    if (fixed_expression_count > 1) {
      // Violates the definition of AllDifferent.
      return (void)context->NotifyThatModelIsUnsat();
    } else if (fixed_expression_count == 1) {
      // Remove values from other domains.
      for (const LinearExpressionProto& expr : possible_exprs) {
        if (context->IsFixed(expr)) continue;
        if (!context->IntersectDomainWith(expr, Domain(v).Complement())) {
          VLOG(1) << "Empty domain for a variable in MaybeExpandAllDiff()";
          return;
        }
      }
    }

    BoolArgumentProto* at_most_or_equal_one =
        is_a_permutation
            ? context->working_model->add_constraints()->mutable_exactly_one()
            : context->working_model->add_constraints()->mutable_at_most_one();
    for (const LinearExpressionProto& expr : possible_exprs) {
      // The above propagation can remove a value after the expressions was
      // added to possible_exprs.
      if (!context->DomainContains(expr, v)) continue;

      // If the expression is fixed, the created literal will be the true
      // literal. We still need to fail if two expressions are fixed to the same
      // value.
      const int encoding = context->GetOrCreateAffineValueEncoding(expr, v);
      at_most_or_equal_one->add_literals(encoding);
    }
  }

  context->UpdateRuleStats(
      absl::StrCat("all_diff:", is_a_permutation ? " permutation" : "",
                   " expanded", keep_after_expansion ? " and kept" : ""));
  if (!keep_after_expansion) ct->Clear();
}

}  // namespace

void ExpandCpModel(PresolveContext* context) {
  if (context->params().disable_constraint_expansion()) return;
  if (context->ModelIsUnsat()) return;

  // None of the function here need to be run twice. This is because we never
  // create constraint that need to be expanded during presolve.
  if (context->ModelIsExpanded()) return;

  // Make sure all domains are initialized.
  context->InitializeNewDomains();

  // Clear the precedence cache.
  context->ClearPrecedenceCache();

  bool has_all_diffs = false;

  // First pass: we look at constraints that may fully encode variables.
  for (int c = 0; c < context->working_model->constraints_size(); ++c) {
    ConstraintProto* const ct = context->working_model->mutable_constraints(c);
    bool skip = false;
    switch (ct->constraint_case()) {
      case ConstraintProto::kLinear:
        // If we only do expansion, we do that as part of the main loop.
        // This way we don't need to call FinalExpansionForLinearConstraint().
        if (ct->linear().domain().size() > 2 &&
            !context->params().cp_model_presolve()) {
          ExpandComplexLinearConstraint(c, ct, context);
        }
        break;
      case ConstraintProto::kReservoir:
        if (context->params().expand_reservoir_constraints()) {
          for (const LinearExpressionProto& demand_expr :
               ct->reservoir().level_changes()) {
            if (!context->IsFixed(demand_expr)) {
              skip = true;
              break;
            }
          }
          if (skip) {
            context->UpdateRuleStats(
                "reservoir: expansion is not supported with  variable level "
                "changes");
          } else {
            ExpandReservoir(ct, context);
          }
        }
        break;
      case ConstraintProto::kIntMod:
        ExpandIntMod(ct, context);
        break;
      case ConstraintProto::kIntProd:
        ExpandIntProd(ct, context);
        break;
      case ConstraintProto::kElement:
        ExpandElement(ct, context);
        break;
      case ConstraintProto::kInverse:
        ExpandInverse(ct, context);
        break;
      case ConstraintProto::kAutomaton:
        ExpandAutomaton(ct, context);
        break;
      case ConstraintProto::kTable:
        if (ct->table().negated()) {
          ExpandNegativeTable(ct, context);
        } else {
          ExpandPositiveTable(ct, context);
        }
        break;
      case ConstraintProto::kAllDiff:
        has_all_diffs = true;
        skip = true;
        break;
      default:
        skip = true;
        break;
    }
    if (skip) continue;  // Nothing was done for this constraint.

    // Update variable-constraint graph.
    context->UpdateNewConstraintsVariableUsage();
    if (ct->constraint_case() == ConstraintProto::CONSTRAINT_NOT_SET) {
      context->UpdateConstraintVariableUsage(c);
    }

    // Early exit if the model is unsat.
    if (context->ModelIsUnsat()) {
      SOLVER_LOG(context->logger(), "UNSAT after expansion of ",
                 ProtobufShortDebugString(*ct));
      return;
    }
  }

  // Second pass. We may decide to expand constraints if all their variables
  // are fully encoded.
  //
  // Cache for variable scanning.
  absl::flat_hash_set<int> domain_of_var_is_used;
  absl::flat_hash_set<int> bounds_of_var_are_used;
  absl::flat_hash_set<int> processed_variables;
  for (int i = 0; i < context->working_model->constraints_size(); ++i) {
    ConstraintProto* const ct = context->working_model->mutable_constraints(i);
    bool skip = false;
    switch (ct->constraint_case()) {
      case ConstraintProto::kAllDiff:
        MaybeExpandAllDiff(ct, context, domain_of_var_is_used,
                           bounds_of_var_are_used, processed_variables);
        break;
      case ConstraintProto::kLinear:
        ExpandSomeLinearOfSizeTwo(ct, context);
        break;
      default:
        skip = true;
        break;
    }

    if (skip) continue;  // Nothing was done for this constraint.

    // Update variable-constraint graph.
    context->UpdateNewConstraintsVariableUsage();
    if (ct->constraint_case() == ConstraintProto::CONSTRAINT_NOT_SET) {
      context->UpdateConstraintVariableUsage(i);
    }

    // Early exit if the model is unsat.
    if (context->ModelIsUnsat()) {
      SOLVER_LOG(context->logger(), "UNSAT after expansion of ",
                 ProtobufShortDebugString(*ct));
      return;
    }
  }

  // The precedence cache can become invalid during presolve as it does not
  // handle variable substitution. It is safer just to clear it at the end
  // of the expansion phase.
  context->ClearPrecedenceCache();

  // Make sure the context is consistent.
  context->InitializeNewDomains();

  // Update any changed domain from the context.
  for (int i = 0; i < context->working_model->variables_size(); ++i) {
    FillDomainInProto(context->DomainOf(i),
                      context->working_model->mutable_variables(i));
  }

  context->NotifyThatModelIsExpanded();
}

void FinalExpansionForLinearConstraint(PresolveContext* context) {
  if (context->params().disable_constraint_expansion()) return;
  if (context->ModelIsUnsat()) return;
  for (int c = 0; c < context->working_model->constraints_size(); ++c) {
    ConstraintProto* const ct = context->working_model->mutable_constraints(c);
    switch (ct->constraint_case()) {
      case ConstraintProto::kLinear:
        if (ct->linear().domain().size() > 2) {
          ExpandComplexLinearConstraint(c, ct, context);
        }
        break;
      default:
        break;
    }
  }
}

}  // namespace sat
}  // namespace operations_research
