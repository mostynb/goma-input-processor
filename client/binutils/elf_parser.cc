// Copyright 2011 The Goma Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "elf_parser.h"

#ifdef __linux__
#include <elf.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sstream>
#include <string>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/strings/str_split.h"
#include "glog/logging.h"
#include "glog/stl_logging.h"
#include "lib/path_util.h"
#include "lib/scoped_fd.h"

namespace devtools_goma {

template <typename Ehdr, typename Phdr, typename Shdr, typename Dyn>
class ElfParserImpl : public ElfParser {
 public:
  ElfParserImpl(const std::string& filename,
                ScopedFd&& fd,
                const char elfIdent[EI_NIDENT])
      : ElfParser(),
        filename_(filename),
        fd_(std::move(fd)),
        valid_(false),
        use_program_header_(true),
        dynamic_phdr_(nullptr),
        strtab_shdr_(nullptr),
        dynamic_shdr_(nullptr),
        text_offset_(0) {
    VLOG(1) << "Elf:" << filename;
    memset(&ehdr_, 0, sizeof ehdr_);
    memcpy(ehdr_.e_ident, elfIdent, EI_NIDENT);
    int elf_class = elfIdent[EI_CLASS];
    VLOG(1) << "elf_class=" << elf_class;
    int elf_data = elfIdent[EI_DATA];
    VLOG(1) << "elf_data=" << elf_data;

    valid_ = (memcmp(elfIdent, ELFMAG, SELFMAG) == 0);
    if (valid_) {
      valid_ = elfIdent[EI_DATA] == ELFDATA2LSB;
      LOG_IF(ERROR, !valid_)
          << "unsupported data encoding:" << elfIdent[EI_DATA];
    }
    CheckIdent();
  }
  ~ElfParserImpl() override = default;
  ElfParserImpl(const ElfParserImpl&) = delete;
  ElfParserImpl& operator=(const ElfParserImpl&) = delete;

  bool valid() const override { return valid_; }
  void UseProgramHeader(bool use_program_header) override {
    use_program_header_ = use_program_header;
  }
  bool ReadDynamicNeeded(std::vector<std::string>* needed) override {
    VLOG(1) << "ReadDynamicNeeded:" << filename_;
    if (!valid_) {
      LOG(ERROR) << "not valid:" << filename_;
      return false;
    }
    if (!ReadEhdr()) {
      return false;
    }
    if (use_program_header_) {
      if (!ReadPhdrs()) {
        return false;
      }
      if (!ReadDynamicSegment()) {
        return false;
      }
    } else {
      if (!ReadShdrs()) {
        return false;
      }
      if (!ReadDynamicSection()) {
        return false;
      }
    }
    if (!ReadDtStrtab()) {
      return false;
    }
    if (dyntab_.empty()) {
      LOG(ERROR) << "empty dyntab? " << filename_;
      return false;
    }
    if (dt_strtab_.empty()) {
      LOG(ERROR) << "empty dt_strtab? " << filename_;
      return false;
    }
    ReadStringEntryInDynamic(DT_NEEDED, needed);
    return true;
  }

  bool ReadDynamicNeededAndRpath(std::vector<std::string>* needed,
                                 std::vector<std::string>* rpath) override {
    if (!ReadDynamicNeeded(needed))
      return false;

    std::vector<std::string> rpath_entries;
    ReadStringEntryInDynamic(DT_RUNPATH, &rpath_entries);
    // A loader checks DT_RPATH if and only if there are no DT_RUNPATH.
    if (rpath_entries.empty()) {
      ReadStringEntryInDynamic(DT_RPATH, &rpath_entries);
    }
    for (const auto& entry : rpath_entries) {
      for (const auto& path : absl::StrSplit(entry, ':')) {
        rpath->push_back(std::string(path));
      }
    }
    return true;
  }

  bool HasDynamic() const override { return !no_dynamic_; }

 private:
  void CheckIdent();
  bool ReadEhdr() {
    if (!valid_)
      return false;
    if (read(fd_.fd(), reinterpret_cast<char*>(&ehdr_) + EI_NIDENT,
             sizeof(Ehdr) - EI_NIDENT) != (sizeof(Ehdr) - EI_NIDENT)) {
      PLOG(ERROR) << "read ehdr:" << filename_;
      valid_ = false;
      return false;
    }
    VLOG(1) << DumpEhdr(ehdr_);
    return true;
  }
  bool ReadPhdrs() {
    if (!valid_)
      return false;
    if (lseek(fd_.fd(), ehdr_.e_phoff, SEEK_SET) == static_cast<off_t>(-1)) {
      PLOG(ERROR) << "seek phoff:" << ehdr_.e_phoff << " " << filename_;
      valid_ = false;
      return false;
    }
    for (int i = 0; i < ehdr_.e_phnum; ++i) {
      auto phdr = absl::make_unique<Phdr>();
      if (read(fd_.fd(), reinterpret_cast<char*>(phdr.get()), sizeof(Phdr)) !=
          sizeof(Phdr)) {
        PLOG(ERROR) << "read phdr:" << i << " " << filename_;
        valid_ = false;
        return false;
      }
      VLOG(1) << i << ":" << DumpPhdr(*phdr);
      switch (phdr->p_type) {
        case PT_DYNAMIC:
          LOG_IF(ERROR, dynamic_phdr_ != nullptr)
              << filename_ << " PT_DYNAMIC " << DumpPhdr(*dynamic_phdr_) << " "
              << DumpPhdr(*phdr);
          dynamic_phdr_ = phdr.get();
          break;
        case PT_LOAD:
          // The first segment, which contains dynstr, is being mapped
          // in non-zero address. Update text_offset_ to adjust the
          // offset of dynstr later.
          if (phdr->p_offset == 0 && phdr->p_vaddr) {
            LOG_IF(ERROR, ehdr_.e_type != ET_EXEC)
                << "Non zero vaddr for non EXEC ELF (" << ehdr_.e_type
                << "): " << DumpPhdr(*phdr);
            text_offset_ = phdr->p_vaddr;
          }
          break;
        default:
          break;
      }
      phdrs_.push_back(std::move(phdr));
    }
    if (dynamic_phdr_ == nullptr) {
      no_dynamic_ = true;
    }
    return valid_;
  }
  bool ReadShdrs() {
    if (!valid_)
      return false;
    if (lseek(fd_.fd(), ehdr_.e_shoff, SEEK_SET) == static_cast<off_t>(-1)) {
      PLOG(ERROR) << "seek shoff:" << ehdr_.e_shoff << " " << filename_;
      valid_ = false;
      return false;
    }
    for (int i = 0; i < ehdr_.e_shnum; ++i) {
      auto shdr = absl::make_unique<Shdr>();
      if (read(fd_.fd(), reinterpret_cast<char*>(shdr.get()), sizeof(Shdr)) !=
          sizeof(Shdr)) {
        PLOG(ERROR) << "read shdr:" << i << " " << filename_;
        valid_ = false;
        return false;
      }
      VLOG(1) << i << ":" << DumpShdr(*shdr);
      // TODO: This cannot handle ET_EXEC as this doesn't
      //               update text_offset_.
      switch (shdr->sh_type) {
        case SHT_STRTAB:
          // May have several STRTAB. Last one is ok?
          strtab_shdr_ = shdr.get();
          break;
        case SHT_DYNAMIC:
          LOG_IF(ERROR, dynamic_shdr_ != nullptr)
              << filename_ << " SHT_DYNAMIC " << DumpShdr(*dynamic_shdr_) << " "
              << DumpShdr(*shdr);
          dynamic_shdr_ = shdr.get();
          break;
        default:
          break;
      }
      shdrs_.push_back(std::move(shdr));
    }
    if (dynamic_shdr_ == nullptr) {
      no_dynamic_ = true;
    }
    if (strtab_shdr_ != nullptr)
      ReadStrtab();
    return valid_;
  }

  bool ReadStrtab() {
    if (!valid_)
      return false;
    if (strtab_shdr_ == nullptr)
      return false;
    VLOG(1) << "strtab:" << DumpShdr(*strtab_shdr_);
    return ReadSectionData(*strtab_shdr_, &strtab_);
  }

  bool ReadDynamicSegment() {
    if (!valid_)
      return false;
    if (dynamic_phdr_ == nullptr)
      return false;
    VLOG(1) << "dynamic:" << DumpPhdr(*dynamic_phdr_);
    return ReadSegmentData(*dynamic_phdr_, &dyntab_);
  }

  bool ReadDynamicSection() {
    if (!valid_)
      return false;
    if (dynamic_shdr_ == nullptr)
      return false;
    VLOG(1) << "dynamic:" << DumpShdr(*dynamic_shdr_);
    return ReadSectionData(*dynamic_shdr_, &dyntab_);
  }

  bool ReadSegmentData(const Phdr& phdr, std::string* data) {
    VLOG(1) << "read:" << DumpPhdr(phdr);
    return ReadFromFile(phdr.p_offset, phdr.p_filesz, data);
  }
  bool ReadSectionData(const Shdr& shdr, std::string* data) {
    VLOG(1) << "read:" << DumpShdr(shdr);
    return ReadFromFile(shdr.sh_offset, shdr.sh_size, data);
  }

  bool ReadFromFile(off_t offset, size_t size, std::string* data) {
    if (!valid_)
      return false;
    if (lseek(fd_.fd(), offset, SEEK_SET) == static_cast<off_t>(-1)) {
      PLOG(ERROR) << "seek:" << offset << " " << filename_;
      valid_ = false;
      return false;
    }
    data->resize(size);
    if (read(fd_.fd(), const_cast<char*>(data->data()), size) !=
        static_cast<ssize_t>(size)) {
      PLOG(ERROR) << "read data:" << size << " " << filename_;
      valid_ = false;
      return false;
    }
    return true;
  }

  bool ReadDtStrtab() {
    if (!valid_)
      return false;
    if (dyntab_.empty())
      return false;
    off_t off = 0;
    size_t size = 0;
    for (size_t pos = 0; pos < dyntab_.size(); pos += sizeof(Dyn)) {
      const Dyn* dyn = reinterpret_cast<const Dyn*>(dyntab_.data() + pos);
      VLOG(2) << DumpDyn(*dyn);
      if (dyn->d_tag == DT_STRTAB)
        off = dyn->d_un.d_ptr - text_offset_;
      else if (dyn->d_tag == DT_STRSZ)
        size = dyn->d_un.d_val;
    }
    VLOG(1) << "dt_strtab: off=" << off << " size=" << size;
    return ReadFromFile(off, size, &dt_strtab_);
  }

  void ReadStringEntryInDynamic(int type, std::vector<std::string>* out) {
    for (size_t pos = 0; pos < dyntab_.size(); pos += sizeof(Dyn)) {
      const Dyn* dyn = reinterpret_cast<const Dyn*>(dyntab_.data() + pos);
      if (dyn->d_tag == type) {
        if (dyn->d_un.d_val > dt_strtab_.size()) {
          LOG(ERROR) << "out of range dt_strtab:" << dyn->d_un.d_val
                     << " dt_strtab.size=" << dt_strtab_.size();
          continue;
        }
        out->push_back(dt_strtab_.data() + dyn->d_un.d_val);
      }
    }
  }

  std::string DumpEhdr(const Ehdr& ehdr) {
    std::stringstream ss;
    ss << "Elf:";
    ss << " type:" << ehdr.e_type;
    ss << " machine:" << ehdr.e_machine;
    ss << " version:" << ehdr.e_version;
    ss << " entry:" << ehdr.e_entry;
    ss << " phoff:" << ehdr.e_phoff;
    ss << " shoff:" << ehdr.e_shoff;
    ss << " flags:" << ehdr.e_flags;
    ss << " ehsize:" << ehdr.e_ehsize;
    ss << " phentsize:" << ehdr.e_phentsize;
    ss << " phnum:" << ehdr.e_phnum;
    ss << " shentsize:" << ehdr.e_shentsize;
    ss << " shnum:" << ehdr.e_shnum;
    ss << " shstrndx:" << ehdr.e_shstrndx;
    return ss.str();
  }

  std::string DumpPhdr(const Phdr& phdr) {
    std::stringstream ss;
    ss << "Program:";
    ss << " type:" << phdr.p_type;
    ss << " offset:" << phdr.p_offset;
    ss << " vaddr:" << phdr.p_vaddr;
    ss << " paddr:" << phdr.p_paddr;
    ss << " filesz:" << phdr.p_filesz;
    ss << " memsz:" << phdr.p_memsz;
    ss << " flags:" << phdr.p_flags;
    ss << " align:" << phdr.p_align;
    return ss.str();
  }

  std::string DumpShdr(const Shdr& shdr) {
    std::stringstream ss;
    ss << "Section:";
    ss << " name:" << shdr.sh_name;
    if (shdr.sh_name < strtab_.size()) {
      ss << "'" << (strtab_.data() + shdr.sh_name) << "'";
    }
    ss << " type:" << shdr.sh_type;
    ss << " flag:" << shdr.sh_flags;
    ss << " addr:" << shdr.sh_offset;
    ss << " offset:" << shdr.sh_size;
    ss << " size:" << shdr.sh_size;
    ss << " link:" << shdr.sh_link;
    ss << " info:" << shdr.sh_info;
    ss << " addralign:" << shdr.sh_addralign;
    ss << " entsize:" << shdr.sh_entsize;

    return ss.str();
  }

  std::string DumpDyn(const Dyn& dyn) {
    std::stringstream ss;
    ss << "Dyn:";
    ss << " tag:" << dyn.d_tag;
    ss << " val:" << dyn.d_un.d_val << " ptr:" << dyn.d_un.d_ptr;
    return ss.str();
  }

  const std::string filename_;
  ScopedFd fd_;
  bool valid_;
  bool use_program_header_;
  bool no_dynamic_ = false;
  Ehdr ehdr_;
  std::vector<std::unique_ptr<Phdr>> phdrs_;
  Phdr* dynamic_phdr_;
  std::vector<std::unique_ptr<Shdr>> shdrs_;
  Shdr* strtab_shdr_;
  std::string strtab_;
  Shdr* dynamic_shdr_;
  std::string dyntab_;
  std::string dt_strtab_;
  size_t text_offset_;
};

template <>
void ElfParserImpl<Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr, Elf32_Dyn>::
    CheckIdent() {
  if (valid_) {
    valid_ = (ehdr_.e_ident[EI_CLASS] == ELFCLASS32);
    LOG_IF(ERROR, !valid_) << "not elf class32";
  }
}

template <>
void ElfParserImpl<Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr, Elf64_Dyn>::
    CheckIdent() {
  if (valid_) {
    valid_ = (ehdr_.e_ident[EI_CLASS] = ELFCLASS64);
    LOG_IF(ERROR, !valid_) << "not elf class64";
  }
}

static ScopedFd OpenElf(const std::string& filename, char* elfIdent) {
  ScopedFd fd(ScopedFd::OpenForRead(filename));
  if (!fd.valid()) {
    PLOG(WARNING) << "open:" << filename;
    return ScopedFd();
  }
  if (read(fd.fd(), elfIdent, EI_NIDENT) != EI_NIDENT) {
    PLOG(WARNING) << "read elf ident:" << filename;
    return ScopedFd();
  }
  if (memcmp(elfIdent, ELFMAG, SELFMAG) != 0) {
    LOG(WARNING) << "not elf: " << filename
                 << " ident:" << std::string(elfIdent, SELFMAG);
    return ScopedFd();
  }
  return fd;
}

/* static */
std::unique_ptr<ElfParser> ElfParser::NewElfParser(
    const std::string& filename) {
  DCHECK(IsPosixAbsolutePath(filename)) << "not absolute path: " << filename;
  char elfIdent[EI_NIDENT];
  ScopedFd fd(OpenElf(filename.c_str(), elfIdent));
  if (!fd.valid()) {
    PLOG(ERROR) << "open elf:" << filename;
    return nullptr;
  }
  std::unique_ptr<ElfParser> parser;
  switch (elfIdent[EI_CLASS]) {
    case ELFCLASS32:
      parser = absl::make_unique<
          ElfParserImpl<Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr, Elf32_Dyn>>(
          filename, std::move(fd), elfIdent);
      break;
    case ELFCLASS64:
      parser = absl::make_unique<
          ElfParserImpl<Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr, Elf64_Dyn>>(
          filename, std::move(fd), elfIdent);
      break;
    default:
      LOG(ERROR) << "Unknown elf class:" << elfIdent[EI_CLASS];
      return nullptr;
  }
  if (!parser->valid()) {
    return nullptr;
  }
  return parser;
}

/* static */
bool ElfParser::IsElf(const std::string& filename) {
  char elfIdent[EI_NIDENT];
  DCHECK(IsPosixAbsolutePath(filename)) << "not absolute path: " << filename;
  ScopedFd fd(OpenElf(filename.c_str(), elfIdent));
  return fd.valid();
}

}  // namespace devtools_goma
