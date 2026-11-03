#include <revolution/nand.h>
#include <aurora/aurora.h>
#include "../io.hpp"
#include "../internal.hpp"
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <filesystem>
#include <memory>

namespace {
aurora::Module Log("aurora::nand");

constexpr size_t NAND_MAX_NAME = 12;

std::filesystem::path g_rootPath;
std::filesystem::path g_homePath;
std::string g_homeDir;

std::filesystem::path resolve_path(const char* path) {
  if (*path != '/') {
    return g_homePath / path;
  }
  while (*path == '/') {
    ++path;
  }
  return g_rootPath / path;
}

s32 result_from_error(const std::error_code& ec) {
  if (ec == std::errc::no_such_file_or_directory) {
    return NAND_RESULT_NOEXISTS;
  }
  if (ec == std::errc::file_exists) {
    return NAND_RESULT_EXISTS;
  }
  if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) {
    return NAND_RESULT_ACCESS;
  }
  if (ec == std::errc::directory_not_empty) {
    return NAND_RESULT_NOTEMPTY;
  }
  if (ec == std::errc::not_a_directory || ec == std::errc::is_a_directory) {
    return NAND_RESULT_INVALID;
  }
  if (ec == std::errc::no_space_on_device) {
    return NAND_RESULT_MAXBLOCKS;
  }
  return NAND_RESULT_UNKNOWN;
}

s32 check_type(const std::filesystem::path& path, std::filesystem::file_type expected) {
  std::error_code ec;
  const auto type = std::filesystem::status(path, ec).type();
  if (type == std::filesystem::file_type::not_found) {
    return NAND_RESULT_NOEXISTS;
  }
  if (ec) {
    return result_from_error(ec);
  }
  return type == expected ? NAND_RESULT_OK : NAND_RESULT_INVALID;
}

struct NANDFile {
  std::string path;
  u8 accType;
  aurora::io::Stream reader;
  aurora::io::AtomicFileWriter writer;
  bool writeFailed = false;

  SDL_IOStream* get() const noexcept { return accType == NAND_ACCESS_READ ? reader.get() : writer.get(); }
};

NANDFile* get_file(NANDFileInfo* info) { return reinterpret_cast<NANDFile*>(info->internal[0]); }
} // namespace

void NANDInit() {
  g_rootPath = aurora::io::fs_path_from_string(aurora::g_config.userPath) / "NAND";
  g_homePath = g_rootPath / aurora::g_config.appName;
  g_homeDir = std::string{"/"} + aurora::g_config.appName;
  std::error_code ec;
  std::filesystem::create_directories(g_homePath, ec);
  // /tmp is cleared on boot
  std::filesystem::remove_all(g_rootPath / "tmp", ec);
  std::filesystem::create_directories(g_rootPath / "tmp", ec);
}

// Info query implementation
s32 NANDGetHomeDir(char* dst) {
  strncpy(dst, g_homeDir.c_str(), NAND_MAX_PATH);
  return NAND_RESULT_OK;
}

// Sync API implementation

s32 NANDCreate(const char* path, u8 perm, u8 attr) {
  const auto fsPath = resolve_path(path);
  std::error_code ec;
  if (std::filesystem::exists(fsPath, ec)) {
    return NAND_RESULT_EXISTS;
  }
  if (!std::filesystem::is_directory(fsPath.parent_path(), ec)) {
    return NAND_RESULT_NOEXISTS;
  }
  if (!aurora::io::open_file(fsPath, "wbx")) {
    Log.error("Failed to create '{}': {}", path, SDL_GetError());
    return NAND_RESULT_UNKNOWN;
  }
  return NAND_RESULT_OK;
}

s32 NANDOpen(const char* path, NANDFileInfo* info, u8 accType) {
  const auto fsPath = resolve_path(path);
  if (const s32 result = check_type(fsPath, std::filesystem::file_type::regular); result != NAND_RESULT_OK) {
    return result;
  }

  auto file = std::make_unique<NANDFile>();
  file->path = path;
  file->accType = accType;
  switch (accType) {
  case NAND_ACCESS_READ:
    file->reader = aurora::io::open_file(fsPath, "rb");
    break;
  case NAND_ACCESS_WRITE:
  case NAND_ACCESS_RW:
    file->writer = aurora::io::open_atomic_file(fsPath, aurora::io::AtomicFileMode::UpdateExisting);
    break;
  default:
    return NAND_RESULT_INVALID;
  }
  if (file->get() == nullptr) {
    Log.error("Failed to open '{}': {}", path, SDL_GetError());
    return NAND_RESULT_UNKNOWN;
  }

  info->internal[0] = reinterpret_cast<uintptr_t>(file.release());
  return NAND_RESULT_OK;
}

s32 NANDClose(NANDFileInfo* info) {
  std::unique_ptr<NANDFile> file{get_file(info)};
  if (!file) {
    return NAND_RESULT_INVALID;
  }
  info->internal[0] = 0;

  if (file->accType != NAND_ACCESS_READ && !file->writeFailed && !file->writer.commit()) {
    Log.error("Failed to commit '{}': {}", file->path, SDL_GetError());
    return NAND_RESULT_CORRUPT;
  }
  return NAND_RESULT_OK;
}

s32 NANDRead(NANDFileInfo* info, void* buf, u32 length) {
  NANDFile* file = get_file(info);
  if (file == nullptr) {
    return NAND_RESULT_INVALID;
  }
  if (file->accType == NAND_ACCESS_WRITE) {
    return NAND_RESULT_ACCESS;
  }

  auto* bytes = static_cast<u8*>(buf);
  u32 total = 0;
  while (total < length) {
    const size_t read = SDL_ReadIO(file->get(), bytes + total, length - total);
    if (read == 0) {
      if (SDL_GetIOStatus(file->get()) == SDL_IO_STATUS_EOF) {
        break;
      }
      Log.error("Failed to read '{}': {}", file->path, SDL_GetError());
      return NAND_RESULT_CORRUPT;
    }
    total += static_cast<u32>(read);
  }
  return static_cast<s32>(total);
}

s32 NANDWrite(NANDFileInfo* info, const void* buf, u32 length) {
  NANDFile* file = get_file(info);
  if (file == nullptr) {
    return NAND_RESULT_INVALID;
  }
  if (file->accType == NAND_ACCESS_READ) {
    return NAND_RESULT_ACCESS;
  }

  if (!aurora::io::write_exact(file->get(), buf, length)) {
    Log.error("Failed to write '{}': {}", file->path, SDL_GetError());
    file->writeFailed = true;
    return NAND_RESULT_CORRUPT;
  }
  return static_cast<s32>(length);
}

s32 NANDSeek(NANDFileInfo* info, s32 offset, s32 whence) {
  NANDFile* file = get_file(info);
  if (file == nullptr) {
    return NAND_RESULT_INVALID;
  }

  SDL_IOWhence sdlWhence;
  switch (whence) {
  case NAND_SEEK_SET:
    sdlWhence = SDL_IO_SEEK_SET;
    break;
  case NAND_SEEK_CUR:
    sdlWhence = SDL_IO_SEEK_CUR;
    break;
  case NAND_SEEK_END:
    sdlWhence = SDL_IO_SEEK_END;
    break;
  default:
    return NAND_RESULT_INVALID;
  }

  const Sint64 pos = SDL_SeekIO(file->get(), offset, sdlWhence);
  if (pos < 0) {
    return NAND_RESULT_INVALID;
  }
  return static_cast<s32>(pos);
}

s32 NANDGetLength(NANDFileInfo* info, u32* size) {
  NANDFile* file = get_file(info);
  if (file == nullptr) {
    return NAND_RESULT_INVALID;
  }

  const Sint64 length = SDL_GetIOSize(file->get());
  if (length < 0) {
    Log.error("Failed to get length of '{}': {}", file->path, SDL_GetError());
    return NAND_RESULT_UNKNOWN;
  }
  *size = static_cast<u32>(length);
  return NAND_RESULT_OK;
}

s32 NANDTell(NANDFileInfo* info, u32* offset) {
  NANDFile* file = get_file(info);
  if (file == nullptr) {
    return NAND_RESULT_INVALID;
  }

  const Sint64 pos = SDL_TellIO(file->get());
  if (pos < 0) {
    Log.error("Failed to get position in '{}': {}", file->path, SDL_GetError());
    return NAND_RESULT_UNKNOWN;
  }
  *offset = static_cast<u32>(pos);
  return NAND_RESULT_OK;
}

s32 NANDDelete(const char* path) {
  std::error_code ec;
  const auto removed = std::filesystem::remove_all(resolve_path(path), ec);
  if (ec) {
    Log.error("Failed to delete '{}': {}", path, ec.message());
    return result_from_error(ec);
  }
  return removed == 0 ? NAND_RESULT_NOEXISTS : NAND_RESULT_OK;
}

s32 NANDMove(const char* path, const char* destDir) {
  const auto oldPath = resolve_path(path);
  const auto newDir = resolve_path(destDir);
  std::error_code ec;
  if (!std::filesystem::exists(oldPath, ec) || !std::filesystem::is_directory(newDir, ec)) {
    return NAND_RESULT_NOEXISTS;
  }

  const auto oldPathString = aurora::io::fs_path_to_string(oldPath);
  const auto newPathString = aurora::io::fs_path_to_string(newDir / oldPath.filename());
  if (!SDL_RenamePath(oldPathString.c_str(), newPathString.c_str())) {
    Log.error("Failed to move '{}' to '{}': {}", path, destDir, SDL_GetError());
    return NAND_RESULT_UNKNOWN;
  }
  return NAND_RESULT_OK;
}

s32 NANDGetStatus(const char* path, NANDStatus* stat) {
  std::error_code ec;
  if (!std::filesystem::exists(resolve_path(path), ec)) {
    return NAND_RESULT_NOEXISTS;
  }
  *stat = NANDStatus{
      .ownerId = 0,
      .groupId = 0,
      .attribute = 0,
      .permission = NAND_PERM_RUSR | NAND_PERM_WUSR | NAND_PERM_RGRP | NAND_PERM_WGRP,
  };
  return NAND_RESULT_OK;
}

s32 NANDSetStatus(const char* path, const NANDStatus* stat) {
  std::error_code ec;
  if (!std::filesystem::exists(resolve_path(path), ec)) {
    return NAND_RESULT_NOEXISTS;
  }
  return NAND_RESULT_OK;
}

s32 NANDReadDir(const char* path, char* nameList, u32* num) {
  const auto fsPath = resolve_path(path);
  if (const s32 result = check_type(fsPath, std::filesystem::file_type::directory); result != NAND_RESULT_OK) {
    return result;
  }

  std::vector<std::string> names;
  std::error_code ec;
  for (std::filesystem::directory_iterator it{fsPath, ec}, end; !ec && it != end; it.increment(ec)) {
    auto name = aurora::io::fs_path_to_string(it->path().filename());
    if (name.size() <= NAND_MAX_NAME) {
      names.push_back(std::move(name));
    }
  }
  if (ec) {
    Log.error("Failed to read directory '{}': {}", path, ec.message());
    return result_from_error(ec);
  }

  if (nameList == nullptr) {
    *num = static_cast<u32>(names.size());
    return NAND_RESULT_OK;
  }

  u32 count = 0;
  for (const auto& name : names) {
    if (count >= *num) {
      break;
    }
    std::memcpy(nameList, name.c_str(), name.size() + 1);
    nameList += name.size() + 1;
    ++count;
  }
  *num = count;
  return NAND_RESULT_OK;
}

s32 NANDCreateDir(const char* path, u8 perm, u8 attr) {
  std::error_code ec;
  if (!std::filesystem::create_directory(resolve_path(path), ec)) {
    return ec ? result_from_error(ec) : NAND_RESULT_EXISTS;
  }
  return NAND_RESULT_OK;
}

s32 NANDCheck(u32 fsBlock, u32 inode, u32* answer) {
  *answer = 0;
  return NAND_RESULT_OK;
}

// Async API.
// Just calls the sync API and calls the callback immediately.

s32 NANDCreateAsync(const char* path, u8 perm, u8 attr, NANDCallback cb) {
  s32 ret = NANDCreate(path, perm, attr);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDOpenAsync(const char* path, NANDFileInfo* info, u8 accType, NANDCallback cb) {
  s32 ret = NANDOpen(path, info, accType);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDCloseAsync(NANDFileInfo* info, NANDCallback cb) {
  s32 ret = NANDClose(info);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDReadAsync(NANDFileInfo* info, void* buf, u32 length, NANDCallback cb) {
  s32 ret = NANDRead(info, buf, length);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDWriteAsync(NANDFileInfo* info, const void* buf, u32 length, NANDCallback cb) {
  s32 ret = NANDWrite(info, buf, length);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDSeekAsync(NANDFileInfo* info, s32 offset, s32 whence, NANDCallback cb) {
  s32 ret = NANDSeek(info, offset, whence);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDGetLengthAsync(NANDFileInfo* info, u32* size, NANDCallback cb) {
  s32 ret = NANDGetLength(info, size);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDTellAsync(NANDFileInfo* info, u32* offset, NANDCallback cb) {
  s32 ret = NANDTell(info, offset);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDReadDirAsync(const char* path, char* nameList, u32* num, NANDCallback cb) {
  s32 ret = NANDReadDir(path, nameList, num);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDCreateDirAsync(const char* path, u8 perm, u8 attr, NANDCallback cb) {
  s32 ret = NANDCreateDir(path, perm, attr);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDDeleteAsync(const char* path, NANDCallback cb) {
  s32 ret = NANDDelete(path);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDMoveAsync(const char* oldName, const char* newName, NANDCallback cb) {
  s32 ret = NANDMove(oldName, newName);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDGetStatusAsync(const char* path, NANDStatus* stat, NANDCallback cb) {
  s32 ret = NANDGetStatus(path, stat);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDSetStatusAsync(const char* path, const NANDStatus* stat, NANDCallback cb) {
  s32 ret = NANDSetStatus(path, stat);
  cb(ret);
  return NAND_RESULT_OK;
}
s32 NANDCheckAsync(u32 fsBlock, u32 inode, u32* answer, NANDCallback cb) {
  s32 ret = NANDCheck(fsBlock, inode, answer);
  cb(ret);
  return NAND_RESULT_OK;
}

// Banner API.

static void copy_banner_comment(u16* dst, const u16* src) {
  if (src[0] == 0) {
    dst[0] = u' ';
    return;
  }
  for (u32 i = 0; i < NAND_BANNER_COMMENT_SIZE && src[i] != 0; ++i) {
    dst[i] = src[i];
  }
}

void NANDInitBanner(NANDBanner* bnr, u32 const flag, const u16* title, const u16* comment) {
  memset(bnr, 0, sizeof(NANDBanner));
  bnr->signature = 0x5749424E;
  bnr->flag = flag;
  copy_banner_comment(bnr->comment[0], title);
  copy_banner_comment(bnr->comment[1], comment);
}
