#pragma once

#include <revolution/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAND_RESULT_OK 0
#define NAND_RESULT_ACCESS (-1)
#define NAND_RESULT_ALLOC_FAILED (-2)
#define NAND_RESULT_BUSY (-3)
#define NAND_RESULT_CORRUPT (-4)
#define NAND_RESULT_ECC_CRIT (-5)
#define NAND_RESULT_EXISTS (-6)
#define NAND_RESULT_INVALID (-8)
#define NAND_RESULT_MAXBLOCKS (-9)
#define NAND_RESULT_MAXFD (-10)
#define NAND_RESULT_MAXFILES (-11)
#define NAND_RESULT_NOEXISTS (-12)
#define NAND_RESULT_NOTEMPTY (-13)
#define NAND_RESULT_OPENFD (-14)
#define NAND_RESULT_AUTHENTICATION (-15)
#define NAND_RESULT_MAXDEPTH (-16)
#define NAND_RESULT_UNKNOWN (-64)
#define NAND_RESULT_FATAL_ERROR (-128)

#define NAND_MAX_PATH 64

#define NAND_CHECK_HOME_INSSPACE 0x00000001
#define NAND_CHECK_HOME_INSINODE 0x00000002
#define NAND_CHECK_SYS_INSSPACE 0x00000004
#define NAND_CHECK_SYS_INSINODE 0x00000008

typedef enum { NAND_ACCESS_NONE, NAND_ACCESS_READ, NAND_ACCESS_WRITE, NAND_ACCESS_RW } NANDAccessType;

typedef enum {
    // Read/write by owner
    NAND_PERM_RUSR = (NAND_ACCESS_READ << 4),
    NAND_PERM_WUSR = (NAND_ACCESS_WRITE << 4),
    // Read/write by group
    NAND_PERM_RGRP = (NAND_ACCESS_READ << 2),
    NAND_PERM_WGRP = (NAND_ACCESS_WRITE << 2),
    // Read/write by other
    NAND_PERM_ROTH = (NAND_ACCESS_READ << 0),
    NAND_PERM_WOTH = (NAND_ACCESS_WRITE << 0),
    // Read/write by all
    NAND_PERM_RALL = NAND_PERM_RUSR | NAND_PERM_RGRP | NAND_PERM_ROTH,
    NAND_PERM_WALL = NAND_PERM_WUSR | NAND_PERM_WGRP | NAND_PERM_WOTH,
    NAND_PERM_RWALL = NAND_PERM_RALL | NAND_PERM_WALL
} NANDPermission;

enum {
    NAND_SEEK_SET = 0,
    NAND_SEEK_CUR = 1,
    NAND_SEEK_END = 2,
};

typedef struct NANDFileInfo {
    uintptr_t internal[2];
} NANDFileInfo;

typedef struct NANDStatus {
    u32 ownerId;
    u16 groupId;
    u8 attribute;
    u8 permission;
} NANDStatus;

void NANDInit();

// Info query
s32 NANDGetHomeDir(char *dst);

// Sync file API
s32 NANDCreate(const char *path, u8 perm, u8 attr);
s32 NANDOpen(const char *path, NANDFileInfo *info, u8 accType);
s32 NANDClose(NANDFileInfo *info);
s32 NANDRead(NANDFileInfo *info, void *buf, u32 length);
s32 NANDWrite(NANDFileInfo *info, const void *buf, u32 length);
s32 NANDSeek(NANDFileInfo *info, s32 offset, s32 whence);
s32 NANDGetLength(NANDFileInfo *info, u32 *size);
s32 NANDTell(NANDFileInfo *info, u32 *offset);
s32 NANDDelete(const char *path);
s32 NANDMove(const char *oldName, const char *newName);
s32 NANDGetStatus(const char *path, NANDStatus *stat);
s32 NANDSetStatus(const char *path, const NANDStatus *stat);
s32 NANDReadDir(const char *path, char *nameList, u32 *num);
s32 NANDCreateDir(const char *path, u8 perm, u8 attr);
s32 NANDCheck(u32 fsBlock, u32 inode, u32* answer);

// Async API
typedef void (*NANDCallback)(s32 result);
s32 NANDCreateAsync(const char *path, u8 perm, u8 attr, NANDCallback cb);
s32 NANDOpenAsync(const char *path, NANDFileInfo *info, u8 accType, NANDCallback cb);
s32 NANDCloseAsync(NANDFileInfo *info, NANDCallback cb);
s32 NANDReadAsync(NANDFileInfo *info, void *buf, u32 length, NANDCallback cb);
s32 NANDWriteAsync(NANDFileInfo *info, const void *buf, u32 length, NANDCallback cb);
s32 NANDSeekAsync(NANDFileInfo *info, s32 offset, s32 whence, NANDCallback cb);
s32 NANDGetLengthAsync(NANDFileInfo *info, u32 *size, NANDCallback cb);
s32 NANDTellAsync(NANDFileInfo *info, u32 *offset, NANDCallback cb);
s32 NANDReadDirAsync(const char *path, char *nameList, u32 *num, NANDCallback cb);
s32 NANDCreateDirAsync(const char *path, u8 perm, u8 attr, NANDCallback cb);
s32 NANDDeleteAsync(const char *path, NANDCallback cb);
s32 NANDMoveAsync(const char *oldName, const char *newName, NANDCallback cb);
s32 NANDGetStatusAsync(const char *path, NANDStatus *stat, NANDCallback cb);
s32 NANDSetStatusAsync(const char *path, const NANDStatus *stat, NANDCallback cb); 
s32 NANDCheckAsync(u32 fsBlock, u32 inode, u32* answer, NANDCallback cb);

// Banner
#define NAND_BANNER_TEXTURE_SIZE (192 * 64 * 2)
#define NAND_BANNER_ICON_SIZE (48 * 48 * 2)
#define NAND_BANNER_COMMENT_SIZE 32

#define NAND_BANNER_ICON_ANIM_SPEED_END 0
#define NAND_BANNER_ICON_ANIM_SPEED_FAST 1
#define NAND_BANNER_ICON_ANIM_SPEED_NORMAL 2
#define NAND_BANNER_ICON_ANIM_SPEED_SLOW 3
#define NAND_BANNER_ICON_ANIM_SPEED_MASK 3

#define NAND_BANNER_FLAG_NOTCOPY 0x00000001

#define NAND_BANNER_FLAG_ANIM_BOUNCE 0x00000010
#define NAND_BANNER_FLAG_ANIM_LOOP 0x00000000
#define NAND_BANNER_FLAG_ANIM_MASK 0x00000010

#define NAND_BANNER_SIGNATURE 0x5749424E

#define NANDGetIconSpeed(stat, n) (((stat)->iconSpeed >> (2 * (n))) & NAND_BANNER_ICON_ANIM_SPEED_MASK)
#define NANDSetIconSpeed(stat, n, f)                                                                                                                 \
    ((stat)->iconSpeed = (u16)(((stat)->iconSpeed & ~(NAND_BANNER_ICON_ANIM_SPEED_MASK << (2 * (n)))) | ((f) << (2 * (n)))))

#define NAND_BANNER_SIZE(i) (32 + NAND_BANNER_COMMENT_SIZE * sizeof(u16) * 2 + NAND_BANNER_TEXTURE_SIZE + NAND_BANNER_ICON_SIZE * (i))

typedef struct {
    /* 0x0000 */ u32 signature;
    /* 0x0004 */ u32 flag;
    /* 0x0008 */ u16 iconSpeed;
    /* 0x000A */ u8 reserved[22];
    /* 0x0020 */ u16 comment[2][NAND_BANNER_COMMENT_SIZE];
    /* 0x0060 */ u8 bannerTexture[NAND_BANNER_TEXTURE_SIZE];
    /* 0x6060 */ u8 iconTexture[8][NAND_BANNER_ICON_SIZE];
} NANDBanner;

void NANDInitBanner(NANDBanner*, u32, const u16*, const u16*);

#ifdef __cplusplus
}
#endif
