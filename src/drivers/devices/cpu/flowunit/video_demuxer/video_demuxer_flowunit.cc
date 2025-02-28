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

#include "video_demuxer_flowunit.h"
#include "modelbox/flowunit.h"
#include "modelbox/flowunit_api_helper.h"

using namespace modelbox;

VideoDemuxerFlowUnit::VideoDemuxerFlowUnit(){};
VideoDemuxerFlowUnit::~VideoDemuxerFlowUnit(){};

modelbox::Status VideoDemuxerFlowUnit::Open(
    const std::shared_ptr<modelbox::Configuration> &opts) {
  return modelbox::STATUS_OK;
}
modelbox::Status VideoDemuxerFlowUnit::Close() { return modelbox::STATUS_OK; }

modelbox::Status VideoDemuxerFlowUnit::Reconnect(
    Status &status, std::shared_ptr<modelbox::DataContext> &ctx) {
  auto ret = modelbox::STATUS_CONTINUE;
  DeferCond { return ret == modelbox::STATUS_SUCCESS; };
  DeferCondAdd { WriteEnd(ctx); };
  auto source_context = std::static_pointer_cast<SourceContext>(
      ctx->GetPrivate(DEMUX_RETRY_CONTEXT));
  if (source_context == nullptr) {
    if (status == modelbox::STATUS_NODATA) {
      ret = modelbox::STATUS_SUCCESS;
      return ret;
    }
    return status;
  }

  source_context->SetLastProcessStatus(status);
  auto retry_status = source_context->NeedRetry();
  if (retry_status == RETRY_NONEED) {
    ret = modelbox::STATUS_FAULT;
  } else if (retry_status == RETRY_STOP) {
    ret = modelbox::STATUS_SUCCESS;
  } else {
    auto timer_task =
        std::static_pointer_cast<TimerTask>(ctx->GetPrivate(DEMUX_TIMER_TASK));
    TimerGlobal::Schedule(timer_task, source_context->GetRetryInterval(), 0);
  }
  return ret;
}

modelbox::Status VideoDemuxerFlowUnit::Process(
    std::shared_ptr<modelbox::DataContext> ctx) {
  auto video_demuxer = std::static_pointer_cast<FfmpegVideoDemuxer>(
      ctx->GetPrivate(DEMUXER_CTX));
  Status demux_status = modelbox::STATUS_FAULT;
  std::shared_ptr<AVPacket> pkt;
  if (video_demuxer != nullptr) {
    demux_status = video_demuxer->Demux(pkt);
  }

  if (demux_status == modelbox::STATUS_OK) {
    auto ret = WriteData(ctx, pkt, video_demuxer);
    if (!ret) {
      return ret;
    }

    auto event = std::make_shared<FlowUnitEvent>();
    ctx->SendEvent(event);
    return STATUS_CONTINUE;
  }

  return Reconnect(demux_status, ctx);
}

void VideoDemuxerFlowUnit::WriteEnd(
    std::shared_ptr<modelbox::DataContext> &ctx) {
  auto video_demuxer = std::static_pointer_cast<FfmpegVideoDemuxer>(
      ctx->GetPrivate(DEMUXER_CTX));
  auto video_packet_output = ctx->Output(VIDEO_PACKET_OUTPUT);
  video_packet_output->Build({1});
  auto end_packet = video_packet_output->At(0);
  int32_t rate_num;
  int32_t rate_den;
  video_demuxer->GetFrameRate(rate_num, rate_den);
  end_packet->Set("rate_num", rate_num);
  end_packet->Set("rate_den", rate_den);
  end_packet->Set("duration", video_demuxer->GetDuration());
  end_packet->Set("time_base", video_demuxer->GetTimeBase());
}

modelbox::Status VideoDemuxerFlowUnit::WriteData(
    std::shared_ptr<modelbox::DataContext> &ctx, std::shared_ptr<AVPacket> &pkt,
    std::shared_ptr<FfmpegVideoDemuxer> video_demuxer) {
  auto video_packet_output = ctx->Output(VIDEO_PACKET_OUTPUT);
  std::vector<size_t> shape(1, (size_t)pkt->size);
  if (pkt->size == 0) {
    // Tell decoder end of stream
    video_packet_output->Build({1});
  } else {
    video_packet_output->BuildFromHost(
        shape, pkt->data, pkt->size,
        [pkt](void *ptr) { /* Only capture pkt */ });
  }

  auto packet_buffer = video_packet_output->At(0);
  packet_buffer->Set("pts", pkt->pts);
  packet_buffer->Set("dts", pkt->dts);
  packet_buffer->Set("time_base", video_demuxer->GetTimeBase());
  int32_t rate_num;
  int32_t rate_den;
  int32_t frame_width;
  int32_t frame_height;
  int32_t rotate_angle = video_demuxer->GetFrameRotate();
  video_demuxer->GetFrameRate(rate_num, rate_den);
  video_demuxer->GetFrameMeta(&frame_width, &frame_height);
  packet_buffer->Set("rate_num", rate_num);
  packet_buffer->Set("rate_den", rate_den);
  packet_buffer->Set("width", frame_width);
  packet_buffer->Set("height", frame_height);
  packet_buffer->Set("rotate_angle", rotate_angle);
  packet_buffer->Set("duration", video_demuxer->GetDuration());
  return STATUS_SUCCESS;
}

modelbox::Status VideoDemuxerFlowUnit::CreateRetryTask(
    std::shared_ptr<modelbox::DataContext> &data_ctx) {
  auto stream_meta = data_ctx->GetInputMeta(STREAM_META_INPUT);
  if (stream_meta == nullptr) {
    return modelbox::STATUS_FAULT;
  }

  auto source_context = std::static_pointer_cast<SourceContext>(
      stream_meta->GetMeta(DEMUX_RETRY_CONTEXT));
  if (source_context == nullptr) {
    return modelbox::STATUS_FAULT;
  }

  data_ctx->SetPrivate(DEMUX_RETRY_CONTEXT, source_context);
  source_context->SetLastProcessStatus(modelbox::STATUS_FAULT);
  std::weak_ptr<VideoDemuxerFlowUnit> flowunit = shared_from_this();
  std::weak_ptr<modelbox::DataContext> ctx = data_ctx;
  auto timer_task = std::make_shared<TimerTask>([flowunit, ctx]() {
    std::shared_ptr<VideoDemuxerFlowUnit> flow_unit_ = flowunit.lock();
    std::shared_ptr<modelbox::DataContext> data_context = ctx.lock();
    if (flow_unit_ == nullptr || data_context == nullptr) {
      return;
    }

    auto event = std::make_shared<FlowUnitEvent>();
    auto source_context = std::static_pointer_cast<SourceContext>(
        data_context->GetPrivate(DEMUX_RETRY_CONTEXT));
    auto source_url = source_context->GetSourceURL();
    modelbox::Status status = modelbox::STATUS_FAULT;
    if (source_url) {
      auto status = flow_unit_->InitDemuxer(data_context, source_url);
    }

    source_context->SetLastProcessStatus(status);
    data_context->SendEvent(event);
  });
  timer_task->SetName("DemuxerReconnect");
  data_ctx->SetPrivate(DEMUX_TIMER_TASK, timer_task);
  return modelbox::STATUS_OK;
}

std::shared_ptr<std::string> VideoDemuxerFlowUnit::GetSourceUrl(
    std::shared_ptr<modelbox::DataContext> data_ctx) {
  // Try get url in input meta
  auto stream_meta = data_ctx->GetInputMeta(STREAM_META_INPUT);
  if (stream_meta != nullptr) {
    auto meta_value = stream_meta->GetMeta(SOURCE_URL);
    if (meta_value != nullptr) {
      return std::static_pointer_cast<std::string>(meta_value);
    }
  }

  // Try get url in input buffer
  auto inputs = data_ctx->Input(STREAM_META_INPUT);
  if (inputs->Size() == 0) {
    MBLOG_ERROR << "source url not found in input";
    return nullptr;
  }

  if (inputs->Size() > 1) {
    MBLOG_WARN << "only supports one url for a stream";
  }

  auto input_buffer = inputs->At(0);
  if (input_buffer == nullptr) {
    MBLOG_ERROR << "input buffer for demuxer is nullptr";
    return nullptr;
  }

  return std::make_shared<std::string>(
      (const char *)(input_buffer->ConstData()), input_buffer->GetBytes());
}

modelbox::Status VideoDemuxerFlowUnit::DataPre(
    std::shared_ptr<modelbox::DataContext> data_ctx) {
  auto source_url_ptr = GetSourceUrl(data_ctx);
  if (source_url_ptr == nullptr) {
    MBLOG_ERROR << "Source url is null, please fill input url correctly";
    return STATUS_FAULT;
  }

  auto codec_id = std::make_shared<AVCodecID>();
  auto profile_id = std::make_shared<int32_t>();
  auto source_url = std::make_shared<std::string>();
  auto meta = std::make_shared<DataMeta>();
  meta->SetMeta(CODEC_META, codec_id);
  meta->SetMeta(PROFILE_META, profile_id);
  meta->SetMeta(SOURCE_URL, source_url);
  data_ctx->SetOutputMeta(VIDEO_PACKET_OUTPUT, meta);
  data_ctx->SetPrivate(VIDEO_PACKET_OUTPUT, meta);

  auto demuxer_status = InitDemuxer(data_ctx, source_url_ptr);

  if (demuxer_status != modelbox::STATUS_OK) {
    MBLOG_INFO << "failed init Demuxer";
  }

  auto ret = CreateRetryTask(data_ctx);
  if (!ret && !demuxer_status) {
    return modelbox::STATUS_FAULT;
  }

  return modelbox::STATUS_SUCCESS;
}

void VideoDemuxerFlowUnit::UpdateStatsInfo(
    const std::shared_ptr<modelbox::DataContext> &ctx,
    const std::shared_ptr<FfmpegVideoDemuxer> &demuxer) {
  auto stats = ctx->GetStatistics();
  int32_t frame_rate_num = 0;
  int32_t frame_rate_den = 0;
  demuxer->GetFrameRate(frame_rate_num, frame_rate_den);
  stats->AddItem("frame_rate_num", frame_rate_num, true);
  stats->AddItem("frame_rate_den", frame_rate_den, true);
}

modelbox::Status VideoDemuxerFlowUnit::InitDemuxer(
    std::shared_ptr<modelbox::DataContext> &ctx,
    std::shared_ptr<std::string> &source_url) {
  auto reader = std::make_shared<FfmpegReader>();
  auto ret = reader->Open(*source_url);
  if (ret != STATUS_SUCCESS) {
    MBLOG_INFO << "Open reader falied, set DEMUX_STATUS failed";
    return STATUS_FAULT;
  }

  auto video_demuxer = std::make_shared<FfmpegVideoDemuxer>();
  ret = video_demuxer->Init(reader, false);
  if (ret != STATUS_SUCCESS) {
    MBLOG_INFO << "video demux init falied, set DEMUX_STATUS failed";
    return STATUS_FAULT;
  }
  video_demuxer->LogStreamInfo();

  auto codec_id = video_demuxer->GetCodecID();
  auto profile_id = video_demuxer->GetProfileID();
  // reset meta value
  auto meta =
      std::static_pointer_cast<DataMeta>(ctx->GetPrivate(VIDEO_PACKET_OUTPUT));
  auto code_meta = std::static_pointer_cast<int>(meta->GetMeta(CODEC_META));
  *code_meta = codec_id;
  auto profile_meta =
      std::static_pointer_cast<int>(meta->GetMeta(PROFILE_META));
  *profile_meta = profile_id;
  auto uri_meta =
      std::static_pointer_cast<std::string>(meta->GetMeta(SOURCE_URL));
  *uri_meta = *source_url;

  ctx->SetPrivate(DEMUXER_CTX, video_demuxer);
  ctx->SetPrivate(SOURCE_URL, source_url);

  UpdateStatsInfo(ctx, video_demuxer);
  return STATUS_SUCCESS;
}

modelbox::Status VideoDemuxerFlowUnit::DataPost(
    std::shared_ptr<modelbox::DataContext> data_ctx) {
  auto timer_task = std::static_pointer_cast<TimerTask>(
      data_ctx->GetPrivate(DEMUX_TIMER_TASK));

  if (timer_task) {
    timer_task->Stop();
  }
  return modelbox::STATUS_OK;
}

MODELBOX_FLOWUNIT(VideoDemuxerFlowUnit, desc) {
  desc.SetFlowUnitName(FLOWUNIT_NAME);
  desc.SetFlowUnitGroupType("Video");
  desc.AddFlowUnitInput({STREAM_META_INPUT, FLOWUNIT_TYPE});
  desc.AddFlowUnitOutput({VIDEO_PACKET_OUTPUT, FLOWUNIT_TYPE});
  desc.SetFlowType(FlowType::STREAM);
  desc.SetStreamSameCount(false);
  desc.SetDescription(FLOWUNIT_DESC);
}

MODELBOX_DRIVER_FLOWUNIT(desc) {
  desc.Desc.SetName(FLOWUNIT_NAME);
  desc.Desc.SetClass(modelbox::DRIVER_CLASS_FLOWUNIT);
  desc.Desc.SetType(FLOWUNIT_TYPE);
  desc.Desc.SetDescription(FLOWUNIT_DESC);
  desc.Desc.SetVersion("1.0.0");
}
