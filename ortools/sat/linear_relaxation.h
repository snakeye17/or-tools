// Copyright 2010-2021 Google LLC
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

#ifndef OR_TOOLS_SAT_LINEAR_RELAXATION_H_
#define OR_TOOLS_SAT_LINEAR_RELAXATION_H_

#include <vector>

#include "ortools/sat/cp_model_mapping.h"
#include "ortools/sat/integer.h"
#include "ortools/sat/linear_constraint.h"
#include "ortools/sat/linear_programming_constraint.h"
#include "ortools/sat/model.h"

namespace operations_research {
namespace sat {

struct LinearRelaxation {
  std::vector<LinearConstraint> linear_constraints;
  std::vector<std::vector<Literal>> at_most_ones;
  std::vector<CutGenerator> cut_generators;
};

// If the given IntegerVariable is fully encoded (li <=> var == xi), adds to the
// constraints vector the following linear relaxation of its encoding:
//   - Sum li == 1
//   - Sum li * xi == var
// Note that all the literal (li) of the encoding must have an IntegerView,
// otherwise this function just does nothing.
//
// Returns false, if the relaxation couldn't be added because this variable
// was not fully encoded or not all its associated literal had a view.
bool AppendFullEncodingRelaxation(IntegerVariable var, const Model& model,
                                  LinearRelaxation* relaxation);

// When the set of (li <=> var == xi) do not cover the full domain of xi, we
// do something a bit more involved. Let min/max the min and max value of the
// domain of var that is NOT part of the encoding. We add:
//   - Sum li <= 1
//   - (Sum li * xi) + (1 - Sum li) * min <= var
//   - var <= (Sum li * xi) + (1 - Sum li) * max
//
// Note that if it turns out that the partial encoding is full, this will just
// use the same encoding as AppendFullEncodingRelaxation(). Any literal that
// do not have an IntegerView will be skipped, there is no point adding them
// to the LP if they are not used in any other constraint, the relaxation will
// have the same "power" without them.
void AppendPartialEncodingRelaxation(IntegerVariable var, const Model& model,
                                     LinearRelaxation* relaxation);

// This is a different relaxation that use a partial set of literal li such that
// (li <=> var >= xi). In which case we use the following encoding:
//   - li >= l_{i+1} for all possible i. Note that the xi need to be sorted.
//   - var >= min + l0 * (x0 - min) + Sum_{i>0} li * (xi - x_{i-1})
//   - and same as above for NegationOf(var) for the upper bound.
//
// Like for AppendPartialEncodingRelaxation() we skip any li that do not have
// an integer view.
void AppendPartialGreaterThanEncodingRelaxation(IntegerVariable var,
                                                const Model& model,
                                                LinearRelaxation* relaxation);

// Adds linearization of int max constraints. Returns a vector of z vars such
// that: z_vars[l] == 1 <=> target = exprs[l].
//
// Consider the Lin Max constraint with d expressions and n variables in the
// form: target = max {exprs[l] = Sum (wli * xi + bl)}. l in {1,..,d}.
//   Li = lower bound of xi
//   Ui = upper bound of xi.
// Let zl be in {0,1} for all l in {1,..,d}.
// The target = exprs[l] when zl = 1.
//
// The following is a valid linearization for Lin Max.
//   target >= exprs[l], for all l in {1,..,d}
//   target <= Sum_i(wki * xi) + Sum_l((Nkl + bl) * zl), for all k in {1,..,d}
// Where Nkl is a large number defined as:
//   Nkl = Sum_i(max((wli - wki)*Li, (wli - wki)*Ui))
//       = Sum (max corner difference for variable i, target expr k, max expr l)
// Reference: "Strong mixed-integer programming formulations for trained neural
// networks" by Ross Anderson et. (https://arxiv.org/pdf/1811.01988.pdf).
// TODO(user): Support linear expression as target.
std::vector<IntegerVariable> AppendLinMaxRelaxation(
    IntegerVariable target, const std::vector<LinearExpression>& exprs,
    Model* model, LinearRelaxation* relaxation);

void AppendBoolOrRelaxation(const ConstraintProto& ct, Model* model,
                            LinearRelaxation* relaxation);

void AppendBoolAndRelaxation(const ConstraintProto& ct, Model* model,
                             LinearRelaxation* relaxation);

void AppendAtMostOneRelaxation(const ConstraintProto& ct, Model* model,
                               LinearRelaxation* relaxation);

void AppendExactlyOneRelaxation(const ConstraintProto& ct, Model* model,
                                LinearRelaxation* relaxation);

void AppendIntMaxRelaxation(const ConstraintProto& ct,
                            bool encode_other_direction, Model* model,
                            LinearRelaxation* relaxation);

void AppendIntMinRelaxation(const ConstraintProto& ct,
                            bool encode_other_direction, Model* model,
                            LinearRelaxation* relaxation);

// Appends linear constraints to the relaxation. This also handles the
// relaxation of linear constraints with enforcement literals.
// A linear constraint lb <= ax <= ub with enforcement literals {ei} is relaxed
// as following.
// lb <= (Sum Negated(ei) * (lb - implied_lb)) + ax <= inf
// -inf <= (Sum Negated(ei) * (ub - implied_ub)) + ax <= ub
// Where implied_lb and implied_ub are trivial lower and upper bounds of the
// constraint.
void AppendLinearConstraintRelaxation(const ConstraintProto& ct,
                                      bool linearize_enforced_constraints,
                                      Model* model,
                                      LinearRelaxation* relaxation);

void AppendCircuitRelaxation(const ConstraintProto& ct, Model* model,
                             LinearRelaxation* relaxation);

void AppendIntervalRelaxation(const ConstraintProto& ct, Model* model,
                              LinearRelaxation* relaxation);

// Adds linearization of no overlap constraints.
// It adds an energetic equation linking the duration of all potential tasks to
// the actual span of the no overlap constraint.
void AppendNoOverlapRelaxation(const CpModelProto& model_proto,
                               const ConstraintProto& ct, Model* model,
                               LinearRelaxation* relaxation);

// Adds linearization of cumulative constraints.The second part adds an
// energetic equation linking the duration of all potential tasks to the actual
// max span * capacity of the cumulative constraint.
void AppendCumulativeRelaxation(const CpModelProto& model_proto,
                                const ConstraintProto& ct, Model* model,
                                LinearRelaxation* relaxation);

// Adds linearization of different types of constraints.
void TryToLinearizeConstraint(const CpModelProto& model_proto,
                              const ConstraintProto& ct,
                              int linearization_level, Model* model,
                              LinearRelaxation* relaxation);

// Cut generators.
void AddCircuitCutGenerator(const ConstraintProto& ct, Model* m,
                            LinearRelaxation* relaxation);

void AddRoutesCutGenerator(const ConstraintProto& ct, Model* m,
                           LinearRelaxation* relaxation);

void AddIntProdCutGenerator(const ConstraintProto& ct, Model* m,
                            LinearRelaxation* relaxation);

void AddAllDiffCutGenerator(const ConstraintProto& ct, Model* m,
                            LinearRelaxation* relaxation);

void AddCumulativeCutGenerator(const ConstraintProto& ct, Model* m,
                               LinearRelaxation* relaxation);

void AddNoOverlapCutGenerator(const ConstraintProto& ct, Model* m,
                              LinearRelaxation* relaxation);

void AddNoOverlap2dCutGenerator(const ConstraintProto& ct, Model* m,
                                LinearRelaxation* relaxation);

void AddLinMaxCutGenerator(const ConstraintProto& ct, Model* m,
                           LinearRelaxation* relaxation);

// Scan the model and add cut generators.
void TryToAddCutGenerators(const ConstraintProto& ct, int linearization_level,
                           Model* m, LinearRelaxation* relaxation);

// Builds the linear relaxaton of a CpModelProto and stores it in the
// LinearRelaxation container.
void ComputeLinearRelaxation(const CpModelProto& model_proto,
                             int linearization_level, Model* m,
                             LinearRelaxation* relaxation);

}  // namespace sat
}  // namespace operations_research

#endif  // OR_TOOLS_SAT_LINEAR_RELAXATION_H_
