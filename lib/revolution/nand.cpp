
#include <revolution/nand.h>
#include <aurora/aurora.h>
#include "../io.hpp"
#include "../internal.hpp"
#include <filesystem>

namespace {
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
} // namespace

void NANDInit() {
  g_rootPath = aurora::io::fs_path_from_string(aurora::g_config.userPath) / "NAND";
  g_homePath = g_rootPath / aurora::g_config.appName;
  g_homeDir = std::string{"/"} + aurora::g_config.appName;
  std::error_code ec;
  std::filesystem::create_directories(g_homePath, ec);
  std::filesystem::create_directories(g_rootPath / "tmp", ec);
}

// Info query implementation
s32 NANDGetHomeDir(char* dst) {
  strncpy(dst, g_homeDir.c_str(), NAND_MAX_PATH);
  return NAND_RESULT_OK;
}

// Sync API implementation

struct NANDFileInfoInternal {
  aurora::io::Stream m_stream;
};
static_assert(sizeof(NANDFileInfoInternal) <= sizeof(NANDFileInfo));

static aurora::io::Stream& get_stream(NANDFileInfo* info) { return ((NANDFileInfoInternal*)info)->m_stream; }

s32 NANDCreate(const char* path, u8 perm, u8 attr) {
  auto stream = aurora::io::open_file(resolve_path(path), "rb+");
  return NAND_RESULT_OK;
}

static char const* get_open_type(u8 accType) {
  switch (accType) {
  case NAND_ACCESS_READ:
  default:
    return "rb";
  case NAND_ACCESS_WRITE:
    return "wb";
  case NAND_ACCESS_RW:
    return "rb+";
  }
}

s32 NANDOpen(const char* path, NANDFileInfo* info, u8 accType) {
  NANDFileInfoInternal* internal = ((NANDFileInfoInternal*)info);
  internal->m_stream = aurora::io::open_file(resolve_path(path), get_open_type(accType));
  if (internal->m_stream == nullptr) {
    // assume this is because the file doesn't exist?
    return NAND_RESULT_NOEXISTS;
  }
  return NAND_RESULT_OK;
}

s32 NANDClose(NANDFileInfo* info) {
  get_stream(info).reset();
  return NAND_RESULT_OK;
}

s32 NANDRead(NANDFileInfo* info, void* buf, u32 length) {
  return aurora::io::read_exact(get_stream(info).get(), buf, length) ? NAND_RESULT_OK : NAND_RESULT_CORRUPT;
}

s32 NANDWrite(NANDFileInfo* info, const void* buf, u32 length) {
  return aurora::io::write_exact(get_stream(info).get(), buf, length) ? NAND_RESULT_OK : NAND_RESULT_CORRUPT;
}

static SDL_IOWhence sdl_whence_from_nand_whence(s32 whence) {
  switch (whence) {
  case NAND_SEEK_SET:
  default:
    return SDL_IO_SEEK_SET;
  case NAND_SEEK_CUR:
    return SDL_IO_SEEK_CUR;
  case NAND_SEEK_END:
    return SDL_IO_SEEK_END;
  }
}

s32 NANDSeek(NANDFileInfo* info, s32 offset, s32 whence) {
  SDL_SeekIO(get_stream(info).get(), offset, sdl_whence_from_nand_whence(whence));
  return NAND_RESULT_OK;
}

s32 NANDGetLength(NANDFileInfo* info, u32* size) {
  *size = u32(SDL_GetIOSize(get_stream(info).get()));
  return NAND_RESULT_OK;
}

s32 NANDTell(NANDFileInfo* info, u32* offset) {
  *offset = u32(SDL_TellIO(get_stream(info).get()));
  return NAND_RESULT_OK;
}

s32 NANDDelete(const char* path) {
  std::filesystem::remove(resolve_path(path));
  return NAND_RESULT_OK;
}

s32 NANDMove(const char* path, const char* destDir) {
  const auto oldPath = resolve_path(path);
  std::error_code ec;
  std::filesystem::rename(oldPath, resolve_path(destDir) / oldPath.filename(), ec);
  return ec ? NAND_RESULT_NOEXISTS : NAND_RESULT_OK;
}

s32 NANDGetStatus(const char* path, NANDStatus* stat) {
  // stub
  return NAND_RESULT_OK;
}

s32 NANDSetStatus(const char* path, const NANDStatus* stat) {
  // stub
  return NAND_RESULT_OK;
}

s32 NANDReadDir(const char* path, char* nameList, u32* num) {
  // stub
  return NAND_RESULT_OK;
}

s32 NANDCreateDir(const char* path, u8 perm, u8 attr) {
  std::filesystem::create_directories(resolve_path(path));
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
