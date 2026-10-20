/*
 * vfs.h -- the emulation core's file system.
 *
 * The Atari800 core accesses files only through a handful of stdio-like
 * functions.  On a bare-metal target there is no operating system to provide
 * them, so this project supplies its own small implementation, backed directly
 * by FatFs (which in turn talks to the SD card and to USB mass storage devices
 * through the Circle drivers).
 *
 * The core's `config.h` redirects the stdio names `fopen`, `fread`, ... onto
 * the ATARI_VFS_* functions declared here.  No call site in the emulation core
 * was modified, and no part of the C library is replaced.
 *
 * ---------------------------------------------------------------------------
 * Why these functions take and return `FILE *`
 * ---------------------------------------------------------------------------
 * The core declares its own handles as `FILE *` -- for example
 * `FILE *fp = fopen(...)`.  If this layer used a distinct handle type, every
 * one of those declarations would be a type error, and the alternative,
 * redefining the `FILE` typedef, collides with the C library's own definition
 * of it in <stdio.h>.
 *
 * So the handle type stays `FILE *`, and the pointers this layer hands out are
 * its own: the `FILE` structure is a private C library type that the emulation
 * core never dereferences, only passes back.  A handle returned from here is
 * therefore never given to a real stdio function -- all of them are redirected
 * -- and the redirection is total, so no synthetic handle can reach newlib.
 * `DIR` needs no such treatment: it is only ever produced by the redirected
 * opendir, so it is declared here as its own type.
 *
 * ---------------------------------------------------------------------------
 * Paths
 * ---------------------------------------------------------------------------
 * Paths are FatFs paths with an explicit volume, for example:
 *
 *     SD:/ROMS/ATARIXL.ROM          the SD card, next to the kernel image
 *     USB:/disks/BOUNTYBOB.ATR      the external USB drive
 *
 * Only plain files are supported; there are no symbolic links and no file
 * owners, and there is no notion of a current directory -- every path is
 * absolute and starts with a volume name.
 */

#ifndef ATARI_PI_VFS_H
#define ATARI_PI_VFS_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest path this layer accepts, including the terminating NUL. */
#ifndef ATARI_VFS_PATH_MAX
#define ATARI_VFS_PATH_MAX 256
#endif

/* ---------------------------------------------------------------------------
 * Files
 * ------------------------------------------------------------------------- */

/*
 * Opens a file.  MODE is a standard C mode string:
 *
 *   "r"  / "rb"   existing file, read only        -- NULL if it does not exist
 *   "w"  / "wb"   create or truncate, write only
 *   "r+"/ "rb+"   existing file, read and write
 *   "w+"/ "wb+"   create or truncate, read and write
 *   "a"  / "ab"   create if absent, append
 *   "a+"/ "ab+"   create if absent, append and read
 *
 * Returns NULL on failure.  There is a small fixed number of handles, which is
 * what the emulation core needs: it keeps a disk image, a cassette image and a
 * cartridge image open at once, and briefly a file while scanning for ROMs.
 */
FILE *ATARI_VFS_Open(const char *path, const char *mode);

/* Closes a file.  Returns 0 on success, non-zero on failure. */
int ATARI_VFS_Close(FILE *file);

/* Reads up to SIZE * COUNT bytes; returns the number of items read. */
size_t ATARI_VFS_Read(void *buffer, size_t size, size_t count, FILE *file);

/* Writes up to SIZE * COUNT bytes; returns the number of items written. */
size_t ATARI_VFS_Write(const void *buffer, size_t size, size_t count, FILE *file);

/* Sets the file position, returning 0 on success. */
int ATARI_VFS_Seek(FILE *file, long offset, int whence);

/* Returns the current file position, or -1 on failure. */
long ATARI_VFS_Tell(FILE *file);

/* Rewinds to the beginning. */
void ATARI_VFS_Rewind(FILE *file);

/* Flushes buffered data to the medium.  Returns 0 on success. */
int ATARI_VFS_Flush(FILE *file);

/* Returns non-zero once the position is at or past the end of the file. */
int ATARI_VFS_Eof(FILE *file);

/* Returns non-zero if an operation on the file failed. */
int ATARI_VFS_Error(FILE *file);

/*
 * Truncates the file to the current position.  Returns 0 on success.  Used when
 * a disk image is written back and ended up shorter than it was.
 */
int ATARI_VFS_Truncate(FILE *file);

/* Returns the size of an open file in bytes, or -1 on failure. */
long ATARI_VFS_Size(FILE *file);

/*
 * Cumulative read statistics: calls, bytes delivered, and failures.
 *
 * Reported by the periodic statistics line.  Kept as counters rather than a log
 * of every read because the emulated machine issues thousands of them, and the
 * cost of writing each one to the card was far higher than its value.
 */
void ATARI_VFS_GetStats(unsigned long *pReads, unsigned long long *pBytes,
			unsigned long *pErrors);

/*
 * Opens a short trace of the next reads, for watching a machine boot from a
 * disk image.  Called from the cold reset path; it closes itself after a fixed
 * number of reads so that continuous I/O cannot flood the log.
 */
void ATARI_VFS_BeginLoadTrace(void);

/*
 * How many entries of the fixed file-handle table are in use, and how many
 * exist.  A mounted disk image holds one for the whole session, so this says
 * whether a machine with a disk is close to exhausting the table -- which would
 * turn a later open into a null handle.
 */
void ATARI_VFS_GetHandleStats(unsigned *pUsed, unsigned *pTotal);

/*
 * Opens a bounded trace of file open/close/seek, for following an ATR image
 * through the file layer while a machine boots from it.
 */
void ATARI_VFS_BeginFileTrace(void);

/* ---------------------------------------------------------------------------
 * Directory iteration
 *
 * A deliberately small subset: these directories are flat and are only ever
 * iterated from the start, which is all the ROM-discovery code needs.
 * ------------------------------------------------------------------------- */

/* The directory handle type used by the redirected opendir/readdir/closedir. */
typedef struct atari_vfs_dir atari_vfs_dir_t;

struct dirent {
	char d_name[ATARI_VFS_PATH_MAX];
};

/* Opens a directory for iteration.  Returns NULL if it cannot be opened. */
atari_vfs_dir_t *ATARI_VFS_OpenDir(const char *path);

/* Returns the next entry, or NULL when the directory is exhausted.  The
 * returned pointer stays valid until the next call. */
struct dirent *ATARI_VFS_ReadDir(atari_vfs_dir_t *dir);

/* Closes a directory.  Returns 0 on success. */
int ATARI_VFS_CloseDir(atari_vfs_dir_t *dir);

#ifdef __cplusplus
}
#endif

#endif /* ATARI_PI_VFS_H */
