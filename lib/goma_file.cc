// Copyright 2010 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "lib/goma_file.h"

#include <fcntl.h>
#include <stdio.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <memory>
#include <stack>
#include <utility>

#include "base/compiler_specific.h"
#include "glog/logging.h"
#include "goma_data_util.h"
#include "lib/file_data_output.h"
#include "lib/scoped_fd.h"

MSVC_PUSH_DISABLE_WARNING_FOR_PROTO()
#include "lib/goma_data.pb.h"
MSVC_POP_WARNING()

namespace {

const size_t kLargeFileThreshold = 2 * 1024 * 1024UL;  // 2MB
const off_t kFileChunkSize = 2 * 1024 * 1024L;

const int kNumChunksInStreamRequest = 5;

}  // anonymous namespace

namespace devtools_goma {

static std::string GetHashKeyInLookupFileReq(const LookupFileReq& req, int i) {
  CHECK_GE(i, 0);
  if (i < req.hash_key_size())
    return req.hash_key(i);
  return "(out of range)";
}

bool FileServiceClient::CreateFileBlob(const std::string& filename,
                                       bool store_large,
                                       FileBlob* blob) {
  VLOG(1) << "CreateFileBlob " << filename;
  blob->set_blob_type(FileBlob::FILE);
  blob->set_file_size(-1);
  bool ok = false;

  std::unique_ptr<FileReader> reader(reader_factory_->NewFileReader(filename));
  size_t file_size = 0;
  if (!reader->valid()) {
    LOG(WARNING) << "open failed: " << filename;
    return false;
  }
  if (!reader->GetFileSize(&file_size)) {
    LOG(WARNING) << "stat failed: " << filename;
    return false;
  }
  blob->set_file_size(file_size);
  VLOG(1) << filename << " size=" << file_size;
  if (file_size > kLargeFileThreshold) {
    ok = CreateFileChunks(reader.get(), file_size, store_large, blob);
  } else {
    ok = ReadFileContent(reader.get(), 0, file_size, blob);
  }

  if (ok) {
    VLOG(1) << "CreateFileBlob " << filename << " ok";
  } else {
    LOG(WARNING) << "CreateFileBlob " << filename << " failed";
  }
  return ok;
}

bool FileServiceClient::StoreFileBlob(const FileBlob& blob) {
  VLOG(1) << "StoreFileBlob";
  if (blob.blob_type() == FileBlob::FILE && blob.file_size() < 0) {
    VLOG(1) << "Invalid FileBlob";
    return false;
  }

  FileBlob* req_blob = const_cast<FileBlob*>(&blob);
  StoreFileReq req;
  StoreFileResp resp;
  req.add_blob()->Swap(req_blob);
  if (requester_info_ != nullptr) {
    *req.mutable_requester_info() = *requester_info_;
  }
  bool ok = StoreFile(&req, &resp);
  req_blob->Swap(req.mutable_blob(0));
  VLOG(1) << "StoreFileBlob " << (ok ? "ok" : "failed");
  return ok;
}

bool FileServiceClient::StoreFileBlobs(const std::vector<FileBlob*>& blobs) {
  VLOG(1) << "StoreFileBlobs num=" << blobs.size();
  StoreFileReq req;
  StoreFileResp resp;
  for (size_t i = 0; i < blobs.size(); ++i) {
    if (blobs[i]->blob_type() == FileBlob::FILE && blobs[i]->file_size() < 0) {
      LOG(WARNING) << "blobs[" << i << "] is invalid FileBlob";
      return false;
    }
    req.add_blob()->Swap(blobs[i]);
  }
  if (requester_info_ != nullptr) {
    *req.mutable_requester_info() = *requester_info_;
  }
  bool ok = StoreFile(&req, &resp);
  for (size_t i = 0; i < blobs.size(); ++i) {
    blobs[i]->Swap(req.mutable_blob(i));
  }
  return ok;
}

bool FileServiceClient::GetFileBlob(const std::string& hash_key,
                                    FileBlob* blob) {
  VLOG(1) << "GetFileBlob " << hash_key;
  LookupFileReq req;
  LookupFileResp resp;
  req.add_hash_key(hash_key);
  if (requester_info_ != nullptr) {
    *req.mutable_requester_info() = *requester_info_;
  }
  if (!LookupFile(&req, &resp)) {
    VLOG(1) << "LookupFile failed";
    return false;
  }
  if (resp.blob_size() < 1) {
    LOG(WARNING) << "no resp.blob()";
    return false;
  }
  blob->Swap(resp.mutable_blob(0));
  return true;
}

bool FileServiceClient::GetFileBlobs(const std::vector<std::string>& hash_keys,
                                     std::vector<FileBlob*>* blobs) {
  VLOG(1) << "GetFileBlobs num=" << hash_keys.size();
  LookupFileReq req;
  LookupFileResp resp;
  for (const auto& key : hash_keys) {
    req.add_hash_key(key);
  }
  if (requester_info_ != nullptr) {
    *req.mutable_requester_info() = *requester_info_;
  }
  if (!LookupFile(&req, &resp)) {
    VLOG(1) << "LookupFile failed";
    return false;
  }
  DCHECK_EQ(hash_keys.size(), static_cast<unsigned int>(resp.blob_size()));
  for (int i = 0; i < resp.blob_size(); ++i) {
    FileBlob* blob = new FileBlob;
    blob->Swap(resp.mutable_blob(i));
    blobs->push_back(blob);
  }
  return true;
}

bool FileServiceClient::OutputFileBlob(const FileBlob& blob,
                                       FileDataOutput* output) {
  if (!output->IsValid()) {
    LOG(ERROR) << "invalid output:" << output->ToString();
    return false;
  }
  if (!IsValidFileBlob(blob)) {
    LOG(ERROR) << "invalid blob of type "
               << FileBlob_BlobType_Name(blob.blob_type()) << "["
               << blob.blob_type() << "]"
               << " offset=" << blob.offset()
               << " content_size=" << blob.content().size()
               << " file_size=" << blob.file_size()
               << " num_hash_keys=" << blob.hash_key().size();
    return false;
  }
  bool ret = false;
  switch (blob.blob_type()) {
    case FileBlob::FILE:
      if (blob.file_size() >= 0) {
        ret = output->WriteAt(0, blob.content());
      } else {
        LOG(ERROR) << "Invalid FileBlob "
                   << "blob_type=FILE file_size=" << blob.file_size();
      }
      break;

    case FileBlob::FILE_META:
      ret = OutputFileChunks(blob, output);
      break;

    case FileBlob::FILE_CHUNK:
      LOG(ERROR) << "Can't write FILE_CHUNK";
      break;

    case FileBlob::FILE_REF: {
      FileBlob stored_blob;
      if (!GetFileBlob(blob.hash_key(0), &stored_blob) ||
          stored_blob.blob_type() != FileBlob::FILE ||
          !IsValidFileBlob(stored_blob)) {
        LOG(ERROR) << "Blob of type=FILE_REF was invalid: " << blob.hash_key(0);
        return false;
      }
      ret = output->WriteAt(0, stored_blob.content());
    } break;

    default:
      LOG(ERROR) << "Unknown blob_type:" << blob.blob_type();
      break;
  }
  if (!output->Close()) {
    PLOG(ERROR) << "Write close failed? " << output->ToString();
    ret = false;
  }
  return ret;
}

bool FileServiceClient::FinishStoreFileTask(
    std::unique_ptr<AsyncTask<StoreFileReq, StoreFileResp>> task) {
  if (!task)
    return true;
  VLOG(1) << "Wait StoreFileTask";
  task->Wait();
  VLOG(1) << "Finish StoreFileTask";
  if (!task->IsSuccess()) {
    LOG(WARNING) << "Finish StoreFileTask failed.";
    return false;
  }
  int num_failed = 0;
  for (int i = 0; i < task->resp().hash_key_size(); ++i) {
    if (task->resp().hash_key(i).empty()) {
      VLOG(1) << "No response at " << i;
      num_failed++;
    }
  }
  if (num_failed > 0) {
    LOG(WARNING) << "StoreFileTask failed " << num_failed << " chunks";
    return false;
  }
  return true;
}

bool FileServiceClient::CreateFileChunks(
    FileReader* fr, off_t size, bool store, FileBlob* blob) {
  VLOG(1) << "CreateFileChunks size=" << size;
  blob->set_blob_type(FileBlob::FILE_META);

  std::unique_ptr<AsyncTask<StoreFileReq, StoreFileResp> > task(
      NewAsyncStoreFileTask());
  if (store && task.get()) {
    // Streaming available.
    VLOG(1) << "Streaming mode";
    if (requester_info_ != nullptr) {
      *task->mutable_req()->mutable_requester_info() = *requester_info_;
    }
    std::unique_ptr<AsyncTask<StoreFileReq, StoreFileResp> > in_flight_task;
    for (off_t offset = 0; offset < size; offset += kFileChunkSize) {
      FileBlob* chunk = task->mutable_req()->add_blob();
      int chunk_size = std::min(kFileChunkSize, size - offset);
      if (!ReadFileContent(fr, offset, chunk_size, chunk)) {
        LOG(WARNING) << "ReadFile failed."
                     << " offset=" << offset << " chunk_size=" << chunk_size;
        return false;
      }
      chunk->set_blob_type(FileBlob::FILE_CHUNK);
      chunk->set_offset(offset);
      chunk->set_file_size(chunk_size);
      std::string hash_key = ComputeFileBlobHashKey(*chunk);
      LOG(INFO) << "chunk hash_key:" << hash_key;
      blob->add_hash_key(hash_key);
      if (task->req().blob_size() >= kNumChunksInStreamRequest) {
        if (!FinishStoreFileTask(std::move(in_flight_task)))
          return false;
        task->Run();
        in_flight_task = std::move(task);
        task = NewAsyncStoreFileTask();
        if (requester_info_ != nullptr) {
          *task->mutable_req()->mutable_requester_info() = *requester_info_;
        }
      }
    }
    VLOG(1) << "ReadFile done";
    if (task->req().blob_size() > 0)
      task->Run();
    else
      task.reset(nullptr);
    if (!FinishStoreFileTask(std::move(in_flight_task))) {
      FinishStoreFileTask(std::move(task));
      return false;
    }
    return FinishStoreFileTask(std::move(task));
  }

  for (off_t offset = 0; offset < size; offset += kFileChunkSize) {
    StoreFileReq req;
    StoreFileResp resp;
    if (requester_info_ != nullptr) {
      *req.mutable_requester_info() = *requester_info_;
    }
    FileBlob* chunk = req.add_blob();
    int chunk_size = std::min(kFileChunkSize, size - offset);
    if (!ReadFileContent(fr, offset, chunk_size, chunk)) {
      LOG(WARNING) << "ReadFile failed."
                   << " offset=" << offset << " chunk_size=" << chunk_size;
      return false;
    }
    chunk->set_blob_type(FileBlob::FILE_CHUNK);
    chunk->set_offset(offset);
    chunk->set_file_size(chunk_size);
    std::string hash_key = ComputeFileBlobHashKey(*chunk);
    VLOG(1) << "chunk hash_key:" << hash_key;
    blob->add_hash_key(hash_key);
    if (store) {
      if (!StoreFile(&req, &resp)) {
        LOG(WARNING) << "StoreFile failed";
        return false;
      }
      if (resp.hash_key(0) != hash_key) {
        LOG(WARNING) << "Wrong hash_key:" << resp.hash_key(0)
                     << "!=" << hash_key;
        return false;
      }
    }
  }
  return true;
}

bool FileServiceClient::ReadFileContent(FileReader* fr,
                                        off_t offset, off_t chunk_size,
                                        FileBlob* blob) {
  VLOG(1) << "ReadFileContent"
          << " offset=" << offset << " chunk_size=" << chunk_size;
  std::string* buf = blob->mutable_content();
  buf->resize(chunk_size);
  if (offset > 0) {
    blob->set_blob_type(FileBlob::FILE_CHUNK);
    blob->set_offset(offset);
  } else {
    blob->set_blob_type(FileBlob::FILE);
  }
  if (fr->Seek(offset, ScopedFd::SeekAbsolute) != offset) {
    PLOG(WARNING) << "Seek failed " << offset;
    blob->clear_content();
    return false;
  }
  off_t nread = 0;
  while (nread < chunk_size) {
    int n = fr->Read(&((*buf)[nread]), chunk_size - nread);
    if (n < 0) {
      PLOG(WARNING) << "read failed.";
      blob->clear_content();
      return false;
    }
    nread += n;
  }
  return true;
}

bool FileServiceClient::OutputLookupFileResp(const LookupFileReq& req,
                                             const LookupFileResp& resp,
                                             FileDataOutput* output) {
  for (int i = 0; i < resp.blob_size(); ++i) {
    const FileBlob& blob = resp.blob(i);
    if (!IsValidFileBlob(blob)) {
      LOG(WARNING) << "no FILE_CHUNK available at " << i << ": "
                   << GetHashKeyInLookupFileReq(req, i)
                   << " blob=" << blob.DebugString();
      return false;
    }
    if (blob.blob_type() == FileBlob::FILE_META) {
      LOG(WARNING) << "Wrong blob_type at " << i << ": "
                   << GetHashKeyInLookupFileReq(req, i)
                   << " blob=" << blob.DebugString();
      return false;
    }
    if (!output->WriteAt(static_cast<off_t>(blob.offset()), blob.content())) {
      LOG(WARNING) << "WriteFileContent failed.";
      return false;
    }
  }
  return true;
}

bool FileServiceClient::FinishLookupFileTask(
    std::unique_ptr<AsyncTask<LookupFileReq, LookupFileResp>> task,
    FileDataOutput* output) {
  if (!task)
    return true;
  VLOG(1) << "Wait LookupFileTask";
  task->Wait();
  VLOG(1) << "Finish LookupFileTask";
  if (!task->IsSuccess()) {
    LOG(WARNING) << "Finish LookupFileTask failed.";
    return false;
  }
  return OutputLookupFileResp(task->req(), task->resp(), output);
}

bool FileServiceClient::OutputFileChunks(const FileBlob& blob,
                                         FileDataOutput* output) {
  VLOG(1) << "OutputFileChunks";
  if (blob.blob_type() != FileBlob::FILE_META) {
    LOG(WARNING) << "wrong blob_type " << blob.blob_type();
    return false;
  }

  std::unique_ptr<AsyncTask<LookupFileReq, LookupFileResp> > task(
      NewAsyncLookupFileTask());
  if (task.get()) {
    // Streaming available.
    VLOG(1) << "Streaming mode";
    if (requester_info_ != nullptr) {
      *task->mutable_req()->mutable_requester_info() = *requester_info_;
    }
    std::unique_ptr<AsyncTask<LookupFileReq, LookupFileResp> > in_flight_task;
    for (const auto& key : blob.hash_key()) {
      task->mutable_req()->add_hash_key(key);
      VLOG(1) << "chunk hash_key:" << key;
      if (task->req().hash_key_size() >= kNumChunksInStreamRequest) {
        if (!FinishLookupFileTask(std::move(in_flight_task), output))
          return false;
        task->Run();
        in_flight_task = std::move(task);
        task = NewAsyncLookupFileTask();
        if (requester_info_ != nullptr) {
          *task->mutable_req()->mutable_requester_info() = *requester_info_;
        }
      }
    }
    VLOG(1) << "LookupFile done";
    if (task->req().hash_key_size() > 0)
      task->Run();
    else
      task.reset(nullptr);
    if (!FinishLookupFileTask(std::move(in_flight_task), output)) {
      FinishLookupFileTask(std::move(task), output);
      return false;
    }

    return FinishLookupFileTask(std::move(task), output);
  }

  for (const auto& key : blob.hash_key()) {
    LookupFileReq req;
    LookupFileResp resp;
    req.add_hash_key(key);
    if (requester_info_ != nullptr) {
      *req.mutable_requester_info() = *requester_info_;
    }
    VLOG(1) << "chunk hash_key:" << key;
    if (!LookupFile(&req, &resp)) {
      LOG(WARNING) << "Lookup failed.";
      return false;
    }
    if (resp.blob_size() < 1) {
      LOG(WARNING) << "no resp.blob()";
      return false;
    }
    if (!OutputLookupFileResp(req, resp, output)) {
      LOG(WARNING) << "Write response failed";
      return false;
    }
  }
  return true;
}

}  // namespace devtools_goma
