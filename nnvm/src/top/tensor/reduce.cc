/*!
 *  Copyright (c) 2017 by Contributors
 * \file reduce.cc
 * \brief reduce operator.
 */
#include <nnvm/op.h>
#include <nnvm/node.h>
#include <nnvm/op_attr_types.h>
#include <nnvm/compiler/op_attr_types.h>
#include <nnvm/compiler/util.h>
#include <nnvm/top/tensor.h>
#include <numeric>
#include "../op_common.h"
#include "../elemwise_op_common.h"
#include "topi/detail/constant_utils.h"
#include "topi/elemwise.h"
#include "topi/reduction.h"
#include "topi/transform.h"

namespace nnvm {
namespace top {
using namespace tvm;
using namespace nnvm::compiler;

// reduce
DMLC_REGISTER_PARAMETER(ReduceParam);

inline TShape GetReduceAxes(const uint32_t indim,
                            const TShape& axis,
                            bool exclude) {
  if (axis.ndim() == 0) {
    TShape r_axes(indim);
    std::iota(r_axes.begin(), r_axes.end(), 0);
    return r_axes;
  }

  CHECK_LT(axis[axis.ndim() - 1], indim)
    << "Reduction axis " << axis[axis.ndim() - 1]
    << " exceeds input dimensions " << indim;

  TShape in_axis = axis;
  for (auto& i : in_axis) {
    i = i < 0 ? i + indim : i;
    CHECK_GE(i, 0) << "axis out of bounds in reduce operator";
    CHECK_LT(i, indim) << "axis out of bounds in reduce operator";
  }
  std::sort(in_axis.begin(), in_axis.end());
  if (!exclude) return in_axis;
  TShape r_axis(indim - in_axis.ndim());
  for (unsigned i = 0, j = 0, k = 0; i < indim; ++i) {
    if (i == in_axis[j]) {
        ++j;
        continue;
    }
    r_axis[k++] = i;
  }
  return r_axis;
}

inline TShape ReduceShapeImpl(const TShape& ishape,
                              const TShape& axis,
                              bool keepdims,
                              bool exclude) {
  uint32_t indim = ishape.ndim();
  TShape r_axes = GetReduceAxes(indim, axis, exclude);
  if (!r_axes.ndim()) return ishape;
  if (r_axes.ndim() == indim)
    return TShape(keepdims ? indim : 1);

  if (keepdims) {
    TShape oshape(ishape);
    for (unsigned i = 0, j = 0; i < indim; ++i) {
      if (i != r_axes[j]) continue;
      oshape[i] = 1;
      ++j;
    }
    return oshape;
  }

  TShape oshape(indim - r_axes.ndim());
  for (unsigned i = 0, j = 0, k = 0; i < indim; ++i) {
    if (i == r_axes[j]) {
      ++j;
      continue;
    }
    oshape[k++] = ishape[i];
  }
  return oshape;
}

inline bool ReduceShape(const nnvm::NodeAttrs& attrs,
                        std::vector<TShape>* in_attrs,
                        std::vector<TShape>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 1U);
  CHECK_EQ(out_attrs->size(), 1U);
  if ((*in_attrs)[0].ndim() == 0) return false;
  const ReduceParam& param = nnvm::get<ReduceParam>(attrs.parsed);
  NNVM_ASSIGN_INPUT_SHAPE(
      attrs, *out_attrs, 0,
      ReduceShapeImpl((*in_attrs)[0], param.axis,
                      param.keepdims, param.exclude));
  return true;
}

inline bool CollapseShape(const nnvm::NodeAttrs& attrs,
                          std::vector<TShape>* in_attrs,
                          std::vector<TShape>* out_attrs) {
  CHECK_EQ(in_attrs->size(), 2U);
  CHECK_EQ(out_attrs->size(), 1U);
  if ((*in_attrs)[0].ndim() == 1) return false;
  NNVM_ASSIGN_OUTPUT_SHAPE(attrs, *out_attrs, 0, (*in_attrs)[1]);
  return true;
}

template<typename PType>
inline void AxesParamParser(nnvm::NodeAttrs* attrs) {
  PType param;
  param.Init(attrs->dict);
  std::sort(&param.axis[0], &param.axis[param.axis.ndim()]);
  attrs->parsed = std::move(param);
}

#define NNVM_REGISTER_BASE_REDUCE_OP(op)                                 \
  NNVM_REGISTER_OP(op)                                                   \
  .add_arguments(ReduceParam::__FIELDS__())                              \
  .set_attr_parser(AxesParamParser<ReduceParam>)                         \
  .set_attr<FGetAttrDict>("FGetAttrDict", ParamGetAttrDict<ReduceParam>) \
  .set_num_outputs(1)

#define NNVM_REGISTER_REDUCE_OP(op)                                     \
  NNVM_REGISTER_BASE_REDUCE_OP(op)                                      \
  .add_argument("data", "Tensor", "The input")                          \
  .set_attr<FInferShape>("FInferShape", ReduceShape)                    \
  .set_attr<FInferType>("FInferType", ElemwiseType<1, 1>)               \
  .set_attr<FCorrectLayout>("FCorrectLayout",                           \
    ElemwiseFixedLayoutUnknownOut<1, 1>)                                \
  .set_num_inputs(1)

NNVM_REGISTER_REDUCE_OP(sum)
.describe(R"code(Computes the sum of array elements over given axes.

Example::

  data = [[[1,2],[2,3],[1,3]],
          [[1,4],[4,3],[5,2]],
          [[7,1],[7,2],[7,3]]]

  sum(data, axis=1)
  [[  4.   8.]
   [ 10.   9.]
   [ 21.   6.]]

  sum(data, axis=[1,2])
  [ 12.  19.  27.]

)code" NNVM_ADD_FILELINE)
.set_attr<FTVMCompute>(
  "FTVMCompute", [](const NodeAttrs& attrs,
                    const Array<Tensor>& inputs,
                    const Array<Tensor>& out_info) {
    const ReduceParam& param = nnvm::get<ReduceParam>(attrs.parsed);
    TShape r_axes = GetReduceAxes(inputs[0]->shape.size(),
                                  param.axis, param.exclude);
    if (!r_axes.ndim()) return Array<Tensor> { topi::identity(inputs[0]) };
    auto axis = ShapeToArray(r_axes);
    return Array<Tensor>{
      topi::sum(inputs[0], axis, param.keepdims) };
})
.set_attr<FGradient>(
  "FGradient", [](const NodePtr& n,
                  const std::vector<NodeEntry>& ograds){
    const ReduceParam& param = nnvm::get<ReduceParam>(n->attrs.parsed);
    std::ostringstream axis; axis << param.axis;
    return std::vector<NodeEntry>{
      MakeNode("expand_like", n->attrs.name + "_grad",
               {ograds[0], n->inputs[0]},
               {{"axis", axis.str()},
                {"exclude", std::to_string(param.exclude)}})
  };
});

NNVM_REGISTER_REDUCE_OP(max)
.describe(R"code(Computes the max of array elements over given axes.

)code" NNVM_ADD_FILELINE)
.set_attr<FTVMCompute>(
  "FTVMCompute", [](const NodeAttrs& attrs,
                    const Array<Tensor>& inputs,
                    const Array<Tensor>& out_info) {
    const ReduceParam& param = nnvm::get<ReduceParam>(attrs.parsed);
    TShape r_axes = GetReduceAxes(inputs[0]->shape.size(),
                                  param.axis, param.exclude);
    auto axis = ShapeToArray(r_axes);
    return Array<Tensor>{
      topi::max(inputs[0], axis, param.keepdims) };
})
.set_attr<FGradient>(
  "FGradient", [](const NodePtr& n,
                  const std::vector<NodeEntry>& ograds){
    const ReduceParam& param = nnvm::get<ReduceParam>(n->attrs.parsed);
    std::ostringstream axis; axis << param.axis;
    NodeEntry sub0 = MakeNode("expand_like", n->attrs.name + "_grad_sub0",
                             {ograds[0], n->inputs[0]},
                             {{"axis", axis.str()},
                              {"keepdims", std::to_string(param.keepdims)},
                              {"exclude", std::to_string(param.exclude)}});
    NodeEntry sub1 = MakeNode("_max_mask", n->attrs.name + "_grad_sub1",
                              {ograds[0]},
                              {{"axis", axis.str()},
                               {"exclude", std::to_string(param.exclude)}});
    return std::vector<NodeEntry>{
      MakeNode("elemwise_mul", n->attrs.name + "_grad", {sub0, sub1})
    };
});

NNVM_REGISTER_REDUCE_OP(min)
.describe(R"code(Computes the min of array elements over given axes.

)code" NNVM_ADD_FILELINE)
.set_attr<FTVMCompute>(
  "FTVMCompute", [](const NodeAttrs& attrs,
                    const Array<Tensor>& inputs,
                    const Array<Tensor>& out_info) {
    const ReduceParam& param = nnvm::get<ReduceParam>(attrs.parsed);
    TShape r_axes = GetReduceAxes(inputs[0]->shape.size(),
                                  param.axis, param.exclude);
    auto axis = ShapeToArray(r_axes);
    return Array<Tensor>{
      topi::min(inputs[0], axis, param.keepdims) };
})
.set_attr<FGradient>(
  "FGradient", [](const NodePtr& n,
                  const std::vector<NodeEntry>& ograds){
    const ReduceParam& param = nnvm::get<ReduceParam>(n->attrs.parsed);
    std::ostringstream axis; axis << param.axis;
    NodeEntry sub0 = MakeNode("expand_like", n->attrs.name + "_grad_sub0",
                              {ograds[0], n->inputs[0]},
                              {{"axis", axis.str()},
                               {"keepdims", std::to_string(param.keepdims)},
                               {"exclude", std::to_string(param.exclude)}});
    NodeEntry sub1 = MakeNode("_min_mask", n->attrs.name + "_grad_sub1",
                              {ograds[0]},
                              {{"axis", axis.str()},
                               {"exclude", std::to_string(param.exclude)}});
    return std::vector<NodeEntry>{
      MakeNode("elemwise_mul", n->attrs.name + "_grad", {sub0, sub1})
    };
});

NNVM_REGISTER_BASE_REDUCE_OP(collapse_sum)
.add_argument("data", "Tensor", "The input")
.add_argument("as", "Tensor", "The reference")
.set_attr<FInferShape>("FInferShape", CollapseShape)
.set_attr<FInferType>("FInferType", ElemwiseType<2, 1>)
.set_attr<FCorrectLayout>("FCorrectLayout", ElemwiseFixedLayoutUnknownOut<2, 1>)
.set_num_inputs(2)
.describe(R"code(Reduces lhs to the shape of rhs via sum)code" NNVM_ADD_FILELINE)
.set_attr<FTVMCompute>(
  "FTVMCompute", [](const NodeAttrs& attrs,
                    const Array<Tensor>& inputs,
                    const Array<Tensor>& out_info) {
    return Array<Tensor>{ topi::collapse_sum(inputs[0], inputs[1]->shape) };
});

}  // namespace top
}  // namespace nnvm
