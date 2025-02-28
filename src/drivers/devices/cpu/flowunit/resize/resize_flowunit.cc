/*
 * Copyright 2021 The Modelbox Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "resize_flowunit.h"
#include <securec.h>
#include "modelbox/flowunit.h"
#include "modelbox/flowunit_api_helper.h"

CVResizeFlowUnit::CVResizeFlowUnit(){};
CVResizeFlowUnit::~CVResizeFlowUnit(){};

std::map<std::string, cv::InterpolationFlags> kCVResizeMethod = {
    {"inter_nearest", cv::INTER_NEAREST},
    {"inter_linear", cv::INTER_LINEAR},
    {"inter_cubic", cv::INTER_CUBIC},
    {"inter_area", cv::INTER_AREA},
    {"inter_lanczos4", cv::INTER_LANCZOS4},
    {"inter_max", cv::INTER_MAX},
    {"warp_fill_outliers", cv::WARP_FILL_OUTLIERS},
    {"warp_inverse_map", cv::WARP_INVERSE_MAP},
};

modelbox::Status CVResizeFlowUnit::Open(
    const std::shared_ptr<modelbox::Configuration> &opts) {
  dest_width_ = opts->GetUint32("width", 0);
  if (dest_width_ == 0) {
    dest_width_ = opts->GetUint32("image_width", 0);
  }

  dest_height_ = opts->GetUint32("height", 0);
  if (dest_height_ == 0) {
    dest_height_ = opts->GetUint32("image_height", 0);
  }
  if (dest_width_ <= 0 || dest_height_ <= 0) {
    auto errMsg = "resize width or height is not configured or invalid.";
    MBLOG_ERROR << errMsg;
    return {modelbox::STATUS_BADCONF, errMsg};
  }

  auto interpolation_str = opts->GetString("interpolation", "inter_linear");
  auto item = kCVResizeMethod.find(interpolation_str);
  if (item == kCVResizeMethod.end()) {
    auto errMsg =
        "resize interpolation is invalid, configure is :" + interpolation_str;
    MBLOG_ERROR << errMsg;
    std::string validmethod;
    for (const auto &iter : kCVResizeMethod) {
      if (validmethod.length() > 0) {
        validmethod += ", ";
      }
      validmethod += iter.first;
    }
    MBLOG_ERROR << "Valid interpolation method is: " << validmethod;
    return {modelbox::STATUS_BADCONF, errMsg};
  }

  interpolation_ = item->second;
  MBLOG_DEBUG << "resize dest width " << dest_width_ << ", resize dest height "
              << dest_height_ << ", resize interpolation method "
              << interpolation_str;
  return modelbox::STATUS_OK;
}
modelbox::Status CVResizeFlowUnit::Close() { return modelbox::STATUS_OK; }

modelbox::Status CVResizeFlowUnit::Process(
    std::shared_ptr<modelbox::DataContext> ctx) {
  MBLOG_DEBUG << "process image cvresize";

  auto input_bufs = ctx->Input("in_image");
  auto output_bufs = ctx->Output("out_image");

  if (input_bufs->Size() <= 0) {
    auto errMsg = "input images batch is " + std::to_string(input_bufs->Size());
    MBLOG_ERROR << errMsg;
    return {modelbox::STATUS_FAULT, errMsg};
  }

  size_t channel = RGB_CHANNELS;
  std::vector<size_t> sub_shape{dest_width_, dest_height_, channel};
  std::vector<size_t> tensor_shape(
      input_bufs->Size(), modelbox::Volume(sub_shape) * sizeof(u_char));
  output_bufs->Build(tensor_shape);

  for (size_t i = 0; i < input_bufs->Size(); ++i) {
    int32_t width;
    int32_t height;
    int32_t channel;
    std::string pix_fmt;
    bool exists = false;
    exists = input_bufs->At(i)->Get("height", height);
    if (!exists) {
      MBLOG_ERROR << "meta don't have key height";
      return {modelbox::STATUS_NOTSUPPORT, "meta don't have key height"};
    }

    exists = input_bufs->At(i)->Get("width", width);
    if (!exists) {
      MBLOG_ERROR << "meta don't have key width";
      return {modelbox::STATUS_NOTSUPPORT, "meta don't have key width"};
    }

    exists = input_bufs->At(i)->Get("pix_fmt", pix_fmt);
    if (!exists && !input_bufs->At(i)->Get("channel", channel)) {
      MBLOG_ERROR << "meta don't have key pix_fmt or channel";
      return {modelbox::STATUS_NOTSUPPORT,
              "meta don't have key pix_fmt or channel"};
    }

    if (exists && pix_fmt != "rgb" && pix_fmt != "bgr") {
      MBLOG_ERROR << "unsupport pix format.";
      return {modelbox::STATUS_NOTSUPPORT, "unsupport pix format."};
    }

    channel = RGB_CHANNELS;
    MBLOG_DEBUG << "get " << width << " rows " << height << " channel "
                << channel;

    auto input_data =
        static_cast<const u_char *>(input_bufs->ConstBufferData(i));

    cv::Mat img_data(cv::Size(width, height), CV_8UC3);
    memcpy_s(img_data.data, img_data.total() * img_data.elemSize(), input_data,
             input_bufs->At(i)->GetBytes());

    MBLOG_DEBUG << "ori image : cols " << img_data.cols << " rows "
                << img_data.rows << " channel " << img_data.channels();

    // resize image
    cv::Size destSize = cv::Size(dest_width_, dest_height_);
    cv::Mat img_dest;
    cv::resize(img_data, img_dest, destSize, 0, 0, interpolation_);

    // output resize image
    auto output = static_cast<uchar *>(output_bufs->MutableBufferData(i));
    memcpy_s(output, output_bufs->At(i)->GetBytes(), img_dest.data,
             img_dest.total() * img_dest.elemSize());
    output_bufs->At(i)->Set("width", (int32_t)dest_width_);
    output_bufs->At(i)->Set("height", (int32_t)dest_height_);
    output_bufs->At(i)->Set("width_stride", (int32_t)dest_width_);
    output_bufs->At(i)->Set("height_stride", (int32_t)dest_height_);
    output_bufs->At(i)->Set("channel", channel);
    output_bufs->At(i)->Set("pix_fmt", pix_fmt);
    output_bufs->At(i)->Set("type", modelbox::ModelBoxDataType::MODELBOX_UINT8);
    output_bufs->At(i)->Set(
        "shape",
        std::vector<size_t>{(size_t)dest_height_, (size_t)dest_width_, 3});
    output_bufs->At(i)->Set("layout", std::string("hwc"));
  }

  return modelbox::STATUS_OK;
}

MODELBOX_FLOWUNIT(CVResizeFlowUnit, desc) {
  desc.SetFlowUnitName(FLOWUNIT_NAME);
  desc.SetFlowUnitGroupType("Image");
  desc.AddFlowUnitInput(modelbox::FlowUnitInput("in_image", FLOWUNIT_TYPE));
  desc.AddFlowUnitOutput(modelbox::FlowUnitOutput("out_image", FLOWUNIT_TYPE));
  desc.SetFlowType(modelbox::NORMAL);
  desc.SetInputContiguous(false);
  desc.SetDescription(FLOWUNIT_DESC);
  desc.AddFlowUnitOption(modelbox::FlowUnitOption("image_width", "int", true,
                                                  "640", "the resize width"));
  desc.AddFlowUnitOption(modelbox::FlowUnitOption("image_height", "int", true,
                                                  "480", "the resize height"));

  std::map<std::string, std::string> method_list;

  for (auto &item : kCVResizeMethod) {
    method_list[item.first] = item.first;
  }

  desc.AddFlowUnitOption(
      modelbox::FlowUnitOption("interpolation", "list", true, "inter_linear",
                               "the resize interpolation method", method_list));
}

MODELBOX_DRIVER_FLOWUNIT(desc) {
  desc.Desc.SetName(FLOWUNIT_NAME);
  desc.Desc.SetClass(modelbox::DRIVER_CLASS_FLOWUNIT);
  desc.Desc.SetType(FLOWUNIT_TYPE);
  desc.Desc.SetDescription(FLOWUNIT_DESC);
  desc.Desc.SetVersion("1.0.0");
}
