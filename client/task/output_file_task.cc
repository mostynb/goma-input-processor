// Copyright 2019 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "task/output_file_task.h"

#include "callback.h"
#include "compile_task.h"
#include "glog/logging.h"
#include "goma_data_util.h"
#include "lib/goma_data.pb.h"
#include "worker_thread_manager.h"

namespace devtools_goma {

OutputFileTask::OutputFileTask(
    WorkerThreadManager* wm,
    std::unique_ptr<BlobClient::Downloader> blob_downloader,
    CompileTask* task,
    int output_index,
    const ExecResult_Output& output,
    OutputFileInfo* info)
    : wm_(wm),
      thread_id_(wm->GetCurrentThreadId()),
      blob_downloader_(std::move(blob_downloader)),
      task_(task),
      output_index_(output_index),
      output_(output),
      output_size_(output.blob().file_size()),
      info_(info),
      success_(false) {
  timer_.Start();
  task_->StartOutputFileTask();
}

OutputFileTask::~OutputFileTask() {
  task_->MaybeRunOutputFileCallback(output_index_, true);
}

void OutputFileTask::Run(OneshotClosure* closure) {
  VLOG(1) << task_->trace_id() << " output " << info_->filename;
  success_ = blob_downloader_->Download(output_, info_);
  if (success_) {
    // TODO: fix to support cas digest.
    info_->hash_key = ComputeFileBlobHashKey(output_.blob());
  } else {
    LOG(WARNING) << task_->trace_id() << " "
                 << (task_->cache_hit() ? "cached" : "no-cached")
                 << " output file failed:" << info_->filename;
  }
  wm_->RunClosureInThread(FROM_HERE, thread_id_, closure,
                          WorkerThread::PRIORITY_LOW);
}

bool OutputFileTask::IsInMemory() const {
  return info_->tmp_filename.empty();
}

}  // namespace devtools_goma
