/* Copyright (c) 2017 PaddlePaddle Authors. All Rights Reserve.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "MKLDNNFcLayer.h"
#include "paddle/utils/Logging.h"

using namespace mkldnn;  // NOLINT
typedef memory::format format;
typedef inner_product_forward fc_fwd;
typedef inner_product_backward_weights fc_bwdWgt;
typedef inner_product_backward_data fc_bwdData;

namespace paddle {

REGISTER_LAYER(mkldnn_fc, MKLDNNFcLayer);

bool MKLDNNFcLayer::init(const LayerMap& layerMap,
                         const ParameterMap& parameterMap) {
  if (!MKLDNNLayer::init(layerMap, parameterMap)) {
    return false;
  }

  CHECK_EQ(inputLayers_.size(), 1) << "Only support one input layer yet";
  CHECK_EQ(inputLayers_.size(), parameters_.size());
  CHECK(!parameters_[0]->isSparse()) << "Do not support sparse yet";

  // output size, cat not be changed
  oc_ = getSize();
  oh_ = 1;
  ow_ = 1;
  ih_ = 1;
  iw_ = 1;

  // input size can not change in FC
  iLayerSize_ = inputLayers_[0]->getSize();
  CHECK_EQ(parameters_[0]->getSize(), iLayerSize_ * oc_);

  // create weight
  weight_ =
      std::unique_ptr<Weight>(new Weight(oc_, iLayerSize_, parameters_[0], 0));

  // create biases
  if (biasParameter_.get() != NULL) {
    biases_ = std::unique_ptr<Weight>(new Weight(1, oc_, biasParameter_));
  }
  return true;
}

void MKLDNNFcLayer::convertWeightsFromPaddle() {
  if (hasInitedWgt_) {
    return;
  }

  CHECK(wgtVal_) << "should have been initialized";
  bool hasNoSpatial_ = ih_ == 1 && iw_ == 1;
  auto targetDim = wgtVal_->getDims();
  auto srcFmt = hasNoSpatial_ ? memory::format::io : memory::format::ihwo;
  wgtVal_->reorderDataFrom(wgtVal_, srcFmt, targetDim);
  hasInitedWgt_ = true;
}

void MKLDNNFcLayer::convertWeightsToPaddle() {
  CHECK(wgtVal_) << "should have been initialized";
  bool hasNoSpatial_ = ih_ == 1 && iw_ == 1;
  auto targetDim = wgtVal_->getDims();
  auto dstFmt = hasNoSpatial_ ? memory::format::io : memory::format::ihwo;
  wgtVal_->reorderDataTo(wgtVal_, dstFmt, targetDim);
}

void MKLDNNFcLayer::reshape(
    int& bs, int& ic, int& ih, int& iw, int oc, int& oh, int& ow) {
  reshapeInput(bs, ih, iw);

  CHECK_EQ(iLayerSize_, inputLayers_[0]->getSize());
  ic = iLayerSize_ / (ih * iw);
  CHECK_EQ(size_t(ic * ih * iw), iLayerSize_) << "not divisible";
  CHECK_EQ(size_t(oc), getSize());

  reshapeOutput(oh, ow);
  resizeOutput(bs, oc);

  printSizeInfo();
}

void MKLDNNFcLayer::resetFwd(std::vector<mkldnn::primitive>& pipeline,
                             MKLDNNMatrixPtr& in,
                             MKLDNNMatrixPtr& wgt,
                             MKLDNNMatrixPtr& bias,
                             MKLDNNMatrixPtr& out) {
  pipeline.clear();
  bool hasBias = biases_ && biases_->getW();
  const MatrixPtr& wgtVal = weight_->getW();
  const MatrixPtr& biasVal = hasBias ? biases_->getW() : nullptr;
  const MatrixPtr& outVal = output_.value;

  if (inputIsOnlyMKLDNN()) {
    const MatrixPtr& inVal = getInputValue(0);
    in = std::dynamic_pointer_cast<MKLDNNMatrix>(inVal);
    CHECK(in) << "Input should be MKLDNNMatrix";
  } else {
    CHECK_EQ(getPrev(0)->getDeviceId(), CPU_DEVICE) << "Only support CPU yet";
    const MatrixPtr& inVal = getInputValue(0, CPU_DEVICE);
    in = MKLDNNMatrix::create(
        inVal, memory::dims{bs_, ic_, ih_, iw_}, format::nchw, engine_);
  }
  in->downSpatial();
  wgt = MKLDNNMatrix::create(
      wgtVal, memory::dims{oc_, ic_, ih_, iw_}, format::oihw, engine_);
  wgt->downSpatial();
  bias = hasBias ? MKLDNNMatrix::create(biasVal, {oc_}, format::x, engine_)
                 : nullptr;
  out = MKLDNNMatrix::create(outVal, {bs_, oc_}, format::nc, engine_);

  // change original output value to mkldnn output value
  output_.value = std::dynamic_pointer_cast<Matrix>(out);
  if (!outputIsOnlyMKLDNN()) {
    // fc cpu output value do not need create convert
    // just share point
    getOutput(CPU_DEVICE).value->setData(output_.value->getData());
  }

  // create forward handle
  prop_kind pk = prop_kind::forward;
  fc_fwd::desc fwdDesc = hasBias ? fc_fwd::desc(pk,
                                                in->getMemoryDesc(),
                                                wgt->getMemoryDesc(),
                                                bias->getMemoryDesc(),
                                                out->getMemoryDesc())
                                 : fc_fwd::desc(pk,
                                                in->getMemoryDesc(),
                                                wgt->getMemoryDesc(),
                                                out->getMemoryDesc());
  fc_fwd::primitive_desc fwdPD = fc_fwd::primitive_desc(fwdDesc, engine_);
  if (hasBias) {
    fwd_.reset(new fc_fwd(fwdPD, *in, *wgt, *bias, *out));
  } else {
    fwd_.reset(new fc_fwd(fwdPD, *in, *wgt, *out));
  }
  printValueFormatFlow();

  pipeline.push_back(*fwd_);
}

void MKLDNNFcLayer::resetBwd(std::vector<mkldnn::primitive>& pipeline,
                             MKLDNNMatrixPtr& in,
                             MKLDNNMatrixPtr& wgt,
                             MKLDNNMatrixPtr& bias,
                             MKLDNNMatrixPtr& out) {
  pipeline.clear();
  if (!needResetBwd_) {
    return;
  }
  needResetBwd_ = false;
  bool hasBias = biases_ && biases_->getWGrad();

  /// backward weight
  CHECK(inVal_) << "Should have input value";
  const MatrixPtr& wgtGrad = weight_->getWGrad();
  const MatrixPtr& biasGrad = hasBias ? biases_->getWGrad() : nullptr;

  // TODO(TJ): merge outgrad
  int device = outputIsOnlyMKLDNN() ? MKLDNN_DEVICE : CPU_DEVICE;
  // for MKLDNN device:
  // can not directly cast outputgrad to mkldnnmatrix,
  // since each layer can not write the inputgrad to mkldnn inputgrad.
  // So just create from matrix with outputvalue format.
  // for CPU device:
  // fc do not need to convert from cpu device since output is always nc format
  // only need create from cpu device
  const MatrixPtr& outGrad = getOutput(device).grad;
  out = MKLDNNMatrix::create(outGrad, outVal_->getPrimitiveDesc());
  wgt = MKLDNNMatrix::create(wgtGrad, wgtVal_->getPrimitiveDesc());
  bias = hasBias ? MKLDNNMatrix::create(biasGrad, biasVal_->getPrimitiveDesc())
                 : nullptr;

  // create memory primitive desc
  fc_fwd::desc fwdDesc = fc_fwd::desc(prop_kind::forward,
                                      inVal_->getMemoryDesc(),
                                      wgt->getMemoryDesc(),
                                      out->getMemoryDesc());
  fc_fwd::primitive_desc fwdPD = fc_fwd::primitive_desc(fwdDesc, engine_);
  fc_bwdWgt::desc bwdWgtDesc = hasBias
                                   ? fc_bwdWgt::desc(inVal_->getMemoryDesc(),
                                                     wgt->getMemoryDesc(),
                                                     bias->getMemoryDesc(),
                                                     out->getMemoryDesc())
                                   : fc_bwdWgt::desc(inVal_->getMemoryDesc(),
                                                     wgt->getMemoryDesc(),
                                                     out->getMemoryDesc());
  fc_bwdWgt::primitive_desc bwdWgtPD =
      fc_bwdWgt::primitive_desc(bwdWgtDesc, engine_, fwdPD);

  if (hasBias) {
    bwdWgt_.reset(new fc_bwdWgt(bwdWgtPD, *inVal_, *out, *wgt, *bias));
  } else {
    bwdWgt_.reset(new fc_bwdWgt(bwdWgtPD, *inVal_, *out, *wgt));
  }
  pipeline.push_back(*bwdWgt_);

  /// backward data
  const MatrixPtr& inGrad = inputLayers_[0]->getOutput().grad;
  if (inGrad == nullptr) {
    return;
  }
  if (getInput(0, MKLDNN_DEVICE).getAllCount() > 1) {
    // TODO(TJ): use outputMaps_ ways to get the inGrad_ when merge outgrad done
  } else {
    in = MKLDNNMatrix::create(inGrad, inVal_->getPrimitiveDesc());
  }

  fc_bwdData::desc bwdDataDesc = fc_bwdData::desc(
      inVal_->getMemoryDesc(), wgt->getMemoryDesc(), out->getMemoryDesc());
  fc_bwdData::primitive_desc bwdDataPD =
      fc_bwdData::primitive_desc(bwdDataDesc, engine_, fwdPD);

  CHECK(wgtVal_) << "Should have weight memory";
  bwdData_.reset(new fc_bwdData(bwdDataPD, *out, *wgtVal_, *in));
  printGradFormatFlow();
  pipeline.push_back(*bwdData_);
}

void MKLDNNFcLayer::updateInputData() {
  inVal_->setData(getInputValue(0, CPU_DEVICE)->getData());
}

void MKLDNNFcLayer::updateWeights(const UpdateCallback& callback) {
  weight_->getParameterPtr()->incUpdate(callback);
  if (biases_ && biases_->getWGrad()) {
    biases_->getParameterPtr()->incUpdate(callback);
  }
}
}  // namespace paddle
