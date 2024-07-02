// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "compiler_proxy_histogram.h"

#include <sstream>

#include "autolock_timer.h"
#include "compile_stats.h"
#include "compiler_specific.h"
#include "glog/logging.h"
#include "histogram.h"
MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "lib/goma_stats.pb.h"
MSVC_POP_WARNING()
#include "time_util.h"
#include "util.h"

namespace devtools_goma {

static const char* GetHistogramItemName(size_t i) {
  // This list needs to be in sync with HistogramItems
  static const char* HistogramItemNames[] = {
    "PendingTime",
    "CompilerInfoProcessTime",
    "IncludePreprocessTime",
    "IncludeProcessorWaitTime",
    "IncludeProcessorRunTime",
    "IncludeFileloadTime",
    "UploadingInputFile",
    "MissingInputFile",
    "RPCCallTime",
    "FileResponseTime",
    "CompilerProxyHandlerTime",
    "GomaccReqSize",
    "GomaccRespSize",
    "ExecReqSize",
    "ExecReqRawSize",
    "ExecReqCompressionRatio",
    "ExecReqBulidTime",
    "ExecReqTime",
    "ExecReqKbps",
    "ExecWaitTime",
    "ExecRespSize",
    "ExecRespRawSize",
    "ExecRespCompressionRatio",
    "ExecRespTime",
    "ExecRespKbps",
    "ExecRespParseTime",
    "InputFileTime",
    "InputFileSize",
    "InputFileKbps",
    "InputFileReqRawSize",
    "InputFileReqCompressionRatio",
    "OutputFileTime",
    "OutputFileSize",
    "ChunkRespSize",
    "OutputFileKbps",
    "OutputFileRespRawSize",
    "OutputFileRespCompressionRatio",
    "LocalDelayTime",
    "LocalPendingTime",
    "LocalRunTime",
    "LocalMemSize",
    "LocalOutputFileTime",
    "LocalOutputFileSize",
    "ThreadpoolHttpServerRequestSize",
    "ThreadpoolHttpServerResponseSize",
    "ThreadpoolHttpServerWaitingTime",
    "ThreadpoolHttpServerReadRequestTime",
    "ThreadpoolHttpServerHandlerTime",
    "ThreadpoolHttpServerWriteResponseTime",
    "NumCols"
  };
  return HistogramItemNames[i];
}

CompilerProxyHistogram::CompilerProxyHistogram()
    : histogram_(NumCols) {
  for (size_t i = 0; i < NumCols; ++i)
    histogram_[i].SetName(GetHistogramItemName(i));
}

CompilerProxyHistogram::~CompilerProxyHistogram() {
}

void CompilerProxyHistogram::UpdateThreadpoolHttpServerStat(
    const ThreadpoolHttpServer::Stat& stat) {
  AUTOLOCK(lock, &mu_);
  histogram_[THSReqSize].Add(stat.req_size);
  histogram_[THSRespSize].Add(stat.resp_size);
  histogram_[THSWaitingTime].AddTimeAsMilliseconds(stat.waiting_time);
  histogram_[THSReadReqTime].AddTimeAsMilliseconds(stat.read_req_time);
  histogram_[THSHandlerTime].AddTimeAsMilliseconds(stat.handler_time);
  histogram_[THSWriteRespTime].AddTimeAsMilliseconds(stat.write_resp_time);
}

void CompilerProxyHistogram::UpdateCompileStat(const CompileStats& stats) {
  AUTOLOCK(lock, &mu_);
  if (stats.pending_time > absl::ZeroDuration())
    histogram_[PendingTime].AddTimeAsMilliseconds(stats.pending_time);
  if (stats.compiler_info_process_time > absl::ZeroDuration())
    histogram_[CompilerInfoProcessTime].AddTimeAsMilliseconds(
        stats.compiler_info_process_time);
  if (stats.include_preprocess_time > absl::ZeroDuration())
    histogram_[IncludePreprocessTime].AddTimeAsMilliseconds(
        stats.include_preprocess_time);
  if (stats.include_processor_wait_time > absl::ZeroDuration()) {
    histogram_[IncludeProcessorWaitTime].AddTimeAsMilliseconds(
        stats.include_processor_wait_time);
  }
  if (stats.include_processor_run_time > absl::ZeroDuration()) {
    histogram_[IncludeProcessorRunTime].AddTimeAsMilliseconds(
        stats.include_processor_run_time);
  }
  if (stats.include_fileload_time > absl::ZeroDuration()) {
    histogram_[IncludeFileloadTime].AddTimeAsMilliseconds(
        stats.include_fileload_time);
  }
  if (stats.exec_log.num_uploading_input_file_size() > 0) {
    histogram_[UploadingInputFile].Add(
        SumRepeatedInt32(stats.exec_log.num_uploading_input_file()));
  }
  if (stats.exec_log.num_missing_input_file_size() > 0) {
    histogram_[MissingInputFile].Add(
        SumRepeatedInt32(stats.exec_log.num_missing_input_file()));
  }

  if (stats.total_rpc_call_time > absl::ZeroDuration()) {
    histogram_[RPCCallTime].AddTimeAsMilliseconds(stats.total_rpc_call_time);
  }
  if (stats.file_response_time > absl::ZeroDuration()) {
    histogram_[FileResponseTime].AddTimeAsMilliseconds(
        stats.file_response_time);
  }
  if (stats.handler_time > absl::ZeroDuration()) {
    histogram_[CompilerProxyHandlerTime].AddTimeAsMilliseconds(
        stats.handler_time);
  }
  if (stats.gomacc_req_size)
    histogram_[GomaccReqSize].Add(stats.gomacc_req_size);
  if (stats.gomacc_resp_size)
    histogram_[GomaccRespSize].Add(stats.gomacc_resp_size);

  // Exec call.
  int64_t rpc_req_size = 0;
  if (stats.exec_log.rpc_req_size_size() > 0) {
    rpc_req_size = SumRepeatedInt32(stats.exec_log.rpc_req_size());
    histogram_[ExecReqSize].Add(rpc_req_size);
  }
  if (stats.exec_log.rpc_raw_req_size_size() > 0) {
    int64_t rpc_raw_req_size =
        SumRepeatedInt32(stats.exec_log.rpc_raw_req_size());
    histogram_[ExecReqRawSize].Add(rpc_raw_req_size);
    if (rpc_raw_req_size > 0) {
      histogram_[ExecReqCompressionRatio].Add(
          100 * rpc_req_size / rpc_raw_req_size);
    }
  }
  if (stats.total_rpc_req_build_time > absl::ZeroDuration()) {
    histogram_[ExecReqBuildTime].AddTimeAsMilliseconds(
        stats.total_rpc_req_build_time);
  }
  if (stats.total_rpc_req_send_time > absl::ZeroDuration()) {
    histogram_[ExecReqTime].AddTimeAsMilliseconds(
        stats.total_rpc_req_send_time);
    histogram_[ExecReqKbps].Add(
        ComputeDataRateInKBps(rpc_req_size, stats.total_rpc_req_send_time));
  }
  if (stats.total_rpc_wait_time > absl::ZeroDuration()) {
    histogram_[ExecWaitTime].AddTimeAsMilliseconds(stats.total_rpc_wait_time);
  }

  int64_t rpc_resp_size = 0;
  if (stats.exec_log.rpc_resp_size_size() > 0) {
    rpc_resp_size = SumRepeatedInt32(stats.exec_log.rpc_resp_size());
    histogram_[ExecRespSize].Add(rpc_resp_size);
  }
  if (stats.exec_log.rpc_raw_resp_size_size() > 0) {
    int64_t rpc_raw_resp_size =
        SumRepeatedInt32(stats.exec_log.rpc_raw_resp_size());
    histogram_[ExecRespRawSize].Add(rpc_raw_resp_size);
    if (rpc_raw_resp_size > 0) {
      histogram_[ExecRespCompressionRatio].Add(
          100 * rpc_resp_size / rpc_raw_resp_size);
    }
  }
  if (stats.total_rpc_resp_recv_time > absl::ZeroDuration()) {
    histogram_[ExecRespTime].AddTimeAsMilliseconds(
        stats.total_rpc_resp_recv_time);
    histogram_[ExecRespKbps].Add(
        ComputeDataRateInKBps(rpc_resp_size, stats.total_rpc_resp_recv_time));
  }
  if (stats.total_rpc_resp_parse_time > absl::ZeroDuration()) {
    histogram_[ExecRespParseTime].AddTimeAsMilliseconds(
        stats.total_rpc_resp_parse_time);
  }
  // Look into protobuf response.

  // FileService
  int64_t input_file_time = 0;
  if (stats.exec_log.input_file_time_size() > 0) {
    input_file_time = SumRepeatedInt32(stats.exec_log.input_file_time());
    histogram_[InputFileTime].Add(input_file_time);
  }
  if (stats.exec_log.input_file_size_size() > 0) {
    int64_t input_file_size =
        SumRepeatedInt32(stats.exec_log.input_file_size());
    histogram_[InputFileSize].Add(input_file_size);
    if (input_file_time > 0) {
      histogram_[InputFileKbps].Add(input_file_size / input_file_time);
    }
  }
  if (stats.input_file_rpc_raw_size > 0) {
    histogram_[InputFileReqRawSize].Add(stats.input_file_rpc_raw_size);
    histogram_[InputFileReqCompressionRatio].Add(
        100 * stats.input_file_rpc_size / stats.input_file_rpc_raw_size);
  }
  if (stats.output_file_time > absl::ZeroDuration()) {
    histogram_[OutputFileTime].AddTimeAsMilliseconds(stats.output_file_time);
  }
  if (stats.exec_log.output_file_size_size() > 0) {
    int64_t output_file_size =
        SumRepeatedInt32(stats.exec_log.output_file_size());
    histogram_[OutputFileSize].Add(output_file_size);
    if (stats.output_file_time > absl::ZeroDuration()) {
      histogram_[OutputFileKbps].Add(
          ComputeDataRateInKBps(output_file_size, stats.output_file_time));
    }
  }
  if (stats.output_file_rpc_raw_size > 0) {
    histogram_[OutputFileRespRawSize].Add(stats.output_file_rpc_raw_size);
    histogram_[OutputFileRespCompressionRatio].Add(
        100 * stats.output_file_rpc_size / stats.output_file_rpc_raw_size);
  }
  if (stats.exec_log.chunk_resp_size_size() > 0)
    histogram_[ChunkRespSize].Add(
        SumRepeatedInt32(stats.exec_log.chunk_resp_size()));

  if (stats.local_delay_time > absl::ZeroDuration())
    histogram_[LocalDelayTime].AddTimeAsMilliseconds(stats.local_delay_time);
  if (stats.local_pending_time > absl::ZeroDuration())
    histogram_[LocalPendingTime].AddTimeAsMilliseconds(
        stats.local_pending_time);
  if (stats.local_run_time > absl::ZeroDuration())
    histogram_[LocalRunTime].AddTimeAsMilliseconds(stats.local_run_time);
  if (stats.exec_log.local_mem_kb() > 0)
    histogram_[LocalMemSize].Add(stats.exec_log.local_mem_kb());
  if (stats.exec_log.local_output_file_time_size() > 0) {
    histogram_[LocalOutputFileTime].AddTimeAsMilliseconds(
        stats.total_local_output_file_time);
  }
  if (stats.exec_log.local_output_file_size_size() > 0) {
    histogram_[LocalOutputFileSize].Add(
        SumRepeatedInt32(stats.exec_log.local_output_file_size()));
  }
}

int64_t CompilerProxyHistogram::GetStatMean(HistogramItems item) const {
  DCHECK_GE(item, 0);
  DCHECK_LT(item, NumCols);
  AUTOLOCK(lock, &mu_);
  const Histogram& h = histogram_[item];
  if (h.count() == 0)
    return 0;
  return h.mean();
}

double CompilerProxyHistogram::GetStatStandardDeviation(
    HistogramItems item) const {
  DCHECK_GE(item, 0);
  DCHECK_LT(item, NumCols);
  AUTOLOCK(lock, &mu_);
  const Histogram& h = histogram_[item];
  if (h.count() == 0)
    return 0.0;
  return h.standard_deviation();
}

void CompilerProxyHistogram::DumpString(std::ostringstream* ss) {
  AUTOLOCK(lock, &mu_);
  for (size_t i = 0; i < NumCols; ++i) {
    if (histogram_[i].count() > 0)
      (*ss) << histogram_[i].DebugString() << "\n";
  }
}

void CompilerProxyHistogram::DumpToProto(GomaHistograms* hist) {
  AUTOLOCK(lock, &mu_);
  histogram_[RPCCallTime].DumpToProto(hist->mutable_rpc_call_time());
}

void CompilerProxyHistogram::Reset() {
  AUTOLOCK(lock, &mu_);
  for (size_t i = 0; i < NumCols; ++i)
    histogram_[i].Reset();
}

}  // namespace devtools_goma
