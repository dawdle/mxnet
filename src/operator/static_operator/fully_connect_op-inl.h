/*!
 * Copyright (c) 2015 by Contributors
 * \file fully_connect_op-inl.h
 * \brief fully connect operator and symbol
*/
#ifndef MXNET_OPERATOR_STATIC_OPERATOR_FULLY_CONNECT_OP_INL_H_
#define MXNET_OPERATOR_STATIC_OPERATOR_FULLY_CONNECT_OP_INL_H_

#include <dmlc/logging.h>
#include <mxnet/operator.h>
#include <mxnet/symbolic.h>
#include <vector>
#include <string>
#include <utility>
#include "./static_operator_common.h"
#include "./param.h"

namespace mxnet {
namespace op {
// Declare enumeration of input order to make code more intuitive.
// These enums are only visible within this header
enum FullyConnectOpInputs {kData, kWeight, kBias};
enum FullyConnectOpOutputs {kOut};

/**
 * \brief This is the implementation of fully connected layer.
 *
 * \tparam xpu The device that the op will be executed on.
 */
template<typename xpu>
class FullyConnectOp : public StaticOperator {
 public:
  /*!
   * \brief constructor with parameters. Used in Bind() in corresponding symbol.
   */
  explicit FullyConnectOp(Param p) {
    this->param_ = p;
  }

  virtual void Forward(Option opt,
                       RunContext ctx,
                       const std::vector<TBlob> &in_data,
                       const std::vector<OpReqType> &req,
                       const std::vector<TBlob> &out_data) {
    using namespace mshadow;
    using namespace mshadow::expr;
    CHECK_EQ(req[kOut], kWriteTo);
    size_t expected = param_.no_bias == 0 ? 3 : 2;
    CHECK_EQ(in_data.size(), expected);
    CHECK_EQ(out_data.size(), 1);
    // TODO(bing): check the BLAS Handle, be careful
    // maybe need blas handle from context
    Stream<xpu> *s = static_cast<Stream<xpu> *>(ctx.stream);
    Tensor<xpu, 2> data = in_data[kData].FlatTo2D<xpu, real_t>(s);
    Tensor<xpu, 2> wmat = in_data[kWeight].get<xpu, 2, real_t>(s);
    Tensor<xpu, 2> out = out_data[kOut].FlatTo2D<xpu, real_t>(s);
    out = dot(data, wmat.T());
    if (param_.no_bias == 0) {
      Tensor<xpu, 1> bias = in_data[kBias].get<xpu, 1, real_t>(s);
      out += repmat(bias, data.size(0));
    }
  }

  virtual void Backward(RunContext ctx,
                        const std::vector<TBlob> &out_grad,
                        const std::vector<TBlob> &in_data,
                        const std::vector<TBlob> &out_data,
                        const std::vector<OpReqType> &req,
                        const std::vector<TBlob> &in_grad) {
    using namespace mshadow;
    using namespace mshadow::expr;
    CHECK_EQ(out_grad.size(), 1);
    size_t expected = param_.no_bias == 0 ? 3 : 2;
    CHECK(in_data.size() == expected && in_grad.size() == expected);
    CHECK_EQ(req.size(), expected);
    // TODO(bing): check the BLAS Handle, be careful
    //  maybe need blas handle from context
    Stream<xpu> *s = static_cast<Stream<xpu> *>(ctx.stream);
    Tensor<xpu, 2> data = in_data[kData].FlatTo2D<xpu, real_t>(s);
    Tensor<xpu, 2> wmat = in_data[kWeight].get<xpu, 2, real_t>(s);
    Tensor<xpu, 2> grad = out_grad[kOut].FlatTo2D<xpu, real_t>(s);
    //  backprop
    CHECK_NE(req[kWeight], kWriteInplace) << "cannot write weight inplace";
    // gradient of weight
    Tensor<xpu, 2> gwmat = in_grad[kWeight].get<xpu, 2, real_t>(s);
    Assign(gwmat, req[kWeight], dot(grad.T(), data));
    // gradient of bias
    if (param_.no_bias == 0) {
      Tensor<xpu, 1> gbias = in_grad[kBias].get<xpu, 1, real_t>(s);
      Assign(gbias, req[kBias], sum_rows(grad));
    }
    // gradient of data
    Tensor<xpu, 2> gdata = in_grad[kData].FlatTo2D<xpu, real_t>(s);
    Assign(gdata, req[kData], dot(grad, wmat));
  }

 private:
  /** The param of the fully connected layer.*/
  Param param_;
};  // class FullyConnectOp

// Decalre factory function, used for dispatch specialization
template<typename xpu>
StaticOperator* CreateFullyConnectedOp(Param param);

#if DMLC_USE_CXX11
/**
 * @brief The symbol part of the fully connected layer.
 */
class FullyConnectSymbol : public AtomicSymbol {
 public:
  virtual std::vector<std::string> ListArguments() const {
    if (param_.no_bias == 0) {
      return {"data", "weight", "bias"};
    } else {
      return {"data", "weight"};
    }
  }

  virtual void SetParam(const char *name, const char *val) {
    param_.SetParam(name, val);
  }

  virtual bool InferShape(std::vector<TShape> *in_shape,
                          std::vector<TShape> *out_shape) const {
    using namespace mshadow;
    if (param_.no_bias == 0) {
      CHECK_EQ(in_shape->size(), 3) << "Input:[data, weight, bias]";
    } else {
      CHECK_EQ(in_shape->size(), 2) << "Input:[data, weight]";
    }
    CHECK_GT(param_.num_hidden, 0);
    const TShape &dshape = (*in_shape)[0];
    CHECK_EQ(dshape.ndim(), 4) << \
        "Input data should be 4D in batch-1-1-hidden";
    CHECK_NE(dshape.ndim(), 0) << "Require data shape to be known";
    ShapeAssignCheck((*in_shape)[kWeight], Shape2(param_.num_hidden, dshape[3]));
    if (param_.no_bias == 0) {
      ShapeAssignCheck((*in_shape)[kBias], Shape1(param_.num_hidden));
    }
    out_shape->clear();
    out_shape->push_back(dshape);
    (*out_shape)[0][3] = param_.num_hidden;
    return true;
  }

  virtual AtomicSymbol* Copy() const {
    FullyConnectSymbol* fc_sym = new FullyConnectSymbol();
    fc_sym->param_ = this->param_;
    return fc_sym;
  }

  virtual std::string TypeString() const {
    return "FullyConnected";
  }
  // decalre dependency and inplace optimization options
  virtual std::vector<int> DeclareBackwardDependency(
      const std::vector<int> &out_grad,
      const std::vector<int> &in_data,
      const std::vector<int> &out_data) const {
    return {out_grad[kOut], in_data[kData], in_data[kWeight]};
  }

  virtual std::vector<std::pair<int, int> > BackwardInplaceOption(
      const std::vector<int> &out_grad,
      const std::vector<int> &in_data,
      const std::vector<int> &out_data,
      const std::vector<int> &in_grad) const {
    return {{in_grad[kData], in_data[kData]}};
  }

  // bind function
  StaticOperator* Bind(Context ctx) const;

 private:
  /** The param of the fully connected layer.*/
  Param param_;
};  // class FullyConnectSymbol
#endif
}  // namespace op
}  // namespace mxnet

#endif  // MXNET_OPERATOR_STATIC_OPERATOR_FULLY_CONNECT_OP_INL_H_