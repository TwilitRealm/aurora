
#include <revolution/nand.h>
#include <aurora/aurora.h>
#include "../io.hpp"
#include "../internal.hpp"
#include <filesystem>

namespace
{
    std::filesystem::path g_pathBase;
}

void NANDInit()
{
    g_pathBase = aurora::io::fs_path_from_string(aurora::g_config.userPath) / "NAND" / aurora::g_config.appName;
}

// Info query implementation
s32 NANDGetHomeDir(char *dst)
{
    strncpy(dst, "home", NAND_MAX_PATH);
    return NAND_RESULT_OK;
}

// Sync API implementation

struct NANDFileInfoInternal
{
    aurora::io::Stream m_stream;
};
static_assert(sizeof(NANDFileInfoInternal) <= sizeof(NANDFileInfo));

static aurora::io::Stream & get_stream(NANDFileInfo* info)
{
    return ((NANDFileInfoInternal*)info)->m_stream;
}

s32 NANDCreate(const char *path, u8 perm, u8 attr)
{
    auto stream = aurora::io::open_file(g_pathBase / path, "rb+");
    return NAND_RESULT_OK;
}

static char const * get_open_type(u8 accType)
{
    switch (accType)
    {
    case NAND_ACCESS_READ: default: return "rb";
    case NAND_ACCESS_WRITE: return "wb";
    case NAND_ACCESS_RW: return "rb+";
    }
}

s32 NANDOpen(const char *path, NANDFileInfo *info, u8 accType)
{
    NANDFileInfoInternal *internal = ((NANDFileInfoInternal*)info);
    internal->m_stream = aurora::io::open_file(g_pathBase / path, get_open_type(accType));
    return NAND_RESULT_OK;
}

s32 NANDClose(NANDFileInfo *info)
{
    get_stream(info).reset();
    return NAND_RESULT_OK;
}

s32 NANDRead(NANDFileInfo *info, void *buf, u32 length)
{
    return aurora::io::read_exact(get_stream(info).get(), buf, length) ? NAND_RESULT_OK : NAND_RESULT_CORRUPT;
}

s32 NANDWrite(NANDFileInfo *info, const void *buf, u32 length)
{
    return aurora::io::write_exact(get_stream(info).get(), buf, length) ? NAND_RESULT_OK : NAND_RESULT_CORRUPT;
}

static SDL_IOWhence sdl_whence_from_nand_whence(s32 whence)
{
    switch (whence)
    {
    case NAND_SEEK_SET: default: return SDL_IO_SEEK_SET;
    case NAND_SEEK_CUR: return SDL_IO_SEEK_CUR;
    case NAND_SEEK_END: return SDL_IO_SEEK_END;
    }
}

s32 NANDSeek(NANDFileInfo *info, s32 offset, s32 whence)
{
    SDL_SeekIO(get_stream(info).get(), offset, sdl_whence_from_nand_whence(whence));
    return NAND_RESULT_OK;
}

s32 NANDGetLength(NANDFileInfo *info, u32 *size)
{
    *size = u32(SDL_GetIOSize(get_stream(info).get()));
    return NAND_RESULT_OK;
}

s32 NANDTell(NANDFileInfo *info, u32 *offset)
{
    *offset = u32(SDL_TellIO(get_stream(info).get()));
    return NAND_RESULT_OK;
}

s32 NANDDelete(const char *path)
{
    std::filesystem::remove(g_pathBase / path);
    return NAND_RESULT_OK;
}

s32 NANDMove(const char *oldName, const char *newName)
{
    std::filesystem::rename(g_pathBase / oldName, g_pathBase / newName);
    return NAND_RESULT_OK;
}

s32 NANDGetStatus(const char *path, NANDStatus *stat)
{
    // stub
    return NAND_RESULT_OK;
}

s32 NANDSetStatus(const char *path, const NANDStatus *stat)
{
    // stub
    return NAND_RESULT_OK;
}

s32 NANDReadDir(const char *path, char *nameList, u32 *num)
{
    // stub
    return NAND_RESULT_OK;
}

s32 NANDCreateDir(const char *path, u8 perm, u8 attr)
{
    std::filesystem::create_directories(g_pathBase / path);
    return NAND_RESULT_OK;
}

s32 NANDCheck(u32 fsBlock, u32 inode, u32* answer)
{
    *answer = 0;
    return NAND_RESULT_OK;
}

// Async API.
// Just calls the sync API and calls the callback immediately.

s32 NANDCreateAsync(const char *path, u8 perm, u8 attr, NANDCallback cb) { s32 ret = NANDCreate(path, perm, attr); cb(ret); return NAND_RESULT_OK; }
s32 NANDOpenAsync(const char *path, NANDFileInfo *info, u8 accType, NANDCallback cb) { s32 ret = NANDOpen(path, info, accType); cb(ret); return NAND_RESULT_OK; }
s32 NANDCloseAsync(NANDFileInfo *info, NANDCallback cb) { s32 ret = NANDClose(info); cb(ret); return NAND_RESULT_OK; }
s32 NANDReadAsync(NANDFileInfo *info, void *buf, u32 length, NANDCallback cb) { s32 ret = NANDRead(info, buf, length); cb(ret); return NAND_RESULT_OK; }
s32 NANDWriteAsync(NANDFileInfo *info, const void *buf, u32 length, NANDCallback cb) { s32 ret = NANDWrite(info, buf, length); cb(ret); return NAND_RESULT_OK; }
s32 NANDSeekAsync(NANDFileInfo *info, s32 offset, s32 whence, NANDCallback cb) { s32 ret = NANDSeek(info, offset, whence); cb(ret); return NAND_RESULT_OK; }
s32 NANDGetLengthAsync(NANDFileInfo *info, u32 *size, NANDCallback cb) { s32 ret = NANDGetLength(info, size); cb(ret); return NAND_RESULT_OK; }
s32 NANDTellAsync(NANDFileInfo *info, u32 *offset, NANDCallback cb) { s32 ret = NANDTell(info, offset); cb(ret); return NAND_RESULT_OK; }
s32 NANDReadDirAsync(const char *path, char *nameList, u32 *num, NANDCallback cb) { s32 ret = NANDReadDir(path, nameList, num); cb(ret); return NAND_RESULT_OK; }
s32 NANDCreateDirAsync(const char *path, u8 perm, u8 attr, NANDCallback cb) { s32 ret = NANDCreateDir(path, perm, attr); cb(ret); return NAND_RESULT_OK; }
s32 NANDDeleteAsync(const char *path, NANDCallback cb) { s32 ret = NANDDelete(path); cb(ret); return NAND_RESULT_OK; }
s32 NANDMoveAsync(const char *oldName, const char *newName, NANDCallback cb) { s32 ret = NANDMove(oldName, newName); cb(ret); return NAND_RESULT_OK; }
s32 NANDGetStatusAsync(const char *path, NANDStatus *stat, NANDCallback cb) { s32 ret = NANDGetStatus(path, stat); cb(ret); return NAND_RESULT_OK; }
s32 NANDSetStatusAsync(const char *path, const NANDStatus *stat, NANDCallback cb) { s32 ret = NANDSetStatus(path, stat); cb(ret); return NAND_RESULT_OK; }
s32 NANDCheckAsync(u32 fsBlock, u32 inode, u32* answer, NANDCallback cb) { s32 ret = NANDCheck(fsBlock, inode, answer); cb(ret); return NAND_RESULT_OK; }

// Banner API.

void NANDInitBanner(NANDBanner* bnr, u32 const flag, const u16* title, const u16* comment)
{
    memset(bnr, 0, sizeof(NANDBanner));
    bnr->signature = 0x5749424E;
    bnr->flag = flag;

    if (wcscmp((wchar_t*)title, L"") == 0)
        wcsncpy((wchar_t*)bnr->comment[0], L" ", 32);
    else
        wcsncpy((wchar_t*)bnr->comment[0], (wchar_t*)title, 32);

    if (wcscmp((wchar_t*)comment, L"") == 0)
        wcsncpy((wchar_t*)bnr->comment[1], L" ", 32);
    else
        wcsncpy((wchar_t*)bnr->comment[1], (wchar_t*)comment, 32);
}
