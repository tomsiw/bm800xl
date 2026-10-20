/*
 * vfs.c -- the emulation core's file system, implemented on top of FatFs.
 *
 * See vfs.h for the contract, and for why the handles here are `FILE *`.
 *
 * FatFs itself comes from Circle's addon/fatfs, whose diskio layer routes the
 * volume names below to the SD card driver or to a USB mass storage device:
 *
 *     SD:    the SD card (device "emmc1")
 *     USB1:  first USB mass storage device ("umsd1")
 *     USB2:  second ("umsd2")
 *     USB3:  third ("umsd3")
 *
 * The emulation core keeps at most three files open at a time -- a disk image,
 * a cassette image and a cartridge image -- plus, briefly, a file being scanned
 * for ROM images, so a small fixed table is enough and avoids any dependency on
 * a heap.
 */

#include "vfs.h"

/*
 * For ATARI_RuntimeLog: every read is traced so that a read which never
 * returns is identifiable from the log.  See the budget notes below.
 */
#include "atari_port.h"

#include "ff.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------- */

/* Concurrently open files. */
#define ATARI_VFS_MAX_FILES 8

/* Concurrently open directories. */
#define ATARI_VFS_MAX_DIRS 2

/* ---------------------------------------------------------------------------
 * Internal types
 * ------------------------------------------------------------------------- */

struct atari_vfs_file {
	int used;
	int error;
	int eof;
	FIL fil;
};

struct atari_vfs_dir {
	int used;
	DIR dir;
	FILINFO info;
	struct dirent entry;
};

static struct atari_vfs_file s_files[ATARI_VFS_MAX_FILES];

/*
 * Bounded trace of file operations, for following an ATR image through this
 * layer while a machine boots from it.  Declared up here because the open/close
 * paths near the top of the file use it, and the routine that arms it lives with
 * the read statistics further down.
 */
#define FILE_TRACE_MAX	40

static unsigned s_file_trace_left;
static struct atari_vfs_dir s_dirs[ATARI_VFS_MAX_DIRS];

/*
 * The handle the caller sees is the address of the slot.  These pointers are
 * never dereferenced as a C library stream: every stdio call in the emulation
 * core is redirected to the functions in this file, and this file never passes
 * a handle on to newlib.
 */
#define HANDLE_OF(slot)  ((FILE *)(void *)(slot))

static struct atari_vfs_file *FileFromHandle(FILE *handle)
{
	int i;

	for (i = 0; i < ATARI_VFS_MAX_FILES; ++i) {
		if (HANDLE_OF(&s_files[i]) == handle) {
			return s_files[i].used ? &s_files[i] : 0;
		}
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * Volume name translation
 *
 * FatFs on Circle is configured with FF_STR_VOLUME_ID == 1 and
 * FF_VOLUME_STRS "SD","SD2","USB","USB2","USB3","FD","NVME": a logical drive is
 * named by the text before a colon, and a colon is mandatory.  Circle's diskio
 * layer then maps those drive numbers onto devices:
 *
 *     index 0 "SD:"    the SD card            ("emmc1")
 *     index 1 "SD2:"   a second SD interface  ("emmc2")
 *     index 2 "USB:"   first USB drive        ("umsd1")
 *     index 3 "USB2:"  second USB drive       ("umsd2")
 *     index 4 "USB3:"  third USB drive        ("umsd3")
 *     index 6 "NVME:"  NVMe device            ("nvme1")
 *
 * This project only uses the SD card and the first USB drive, but the others
 * are accepted so that paths written into a settings file by another tool still
 * resolve.
 *
 * A path with no recognised prefix is taken to be on the SD card.
 * ------------------------------------------------------------------------- */

typedef struct {
	const char *alias;	/* prefix as the emulator writes it */
	const char *volume;	/* volume name FatFs knows, colon included */
} volume_map_t;

static const volume_map_t s_volumes[] = {
	{ "SD:",   "SD:"   },
	{ "USB:",  "USB:"  },	/* plain "USB:" means the first USB drive */
	{ "USB1:", "USB:"  },
	{ "USB2:", "USB2:" },
	{ "USB3:", "USB3:" },
	{ "USB4:", "USB3:" },	/* only three USB mass storage volumes exist */
};

#define ATARI_VFS_VOLUME_COUNT (sizeof s_volumes / sizeof s_volumes[0])

/*
 * Translates ALIAS_PATH into TARGET.  Returns 0 on success, -1 if the path does
 * not fit.
 */
static int TranslatePath(const char *alias_path, char *target, size_t target_size)
{
	size_t i;

	if (alias_path == 0 || alias_path[0] == '\0') {
		ATARI_RuntimeLog("path error - empty or null path");
		return -1;
	}

	for (i = 0; i < ATARI_VFS_VOLUME_COUNT; ++i) {
		size_t alias_len = strlen(s_volumes[i].alias);
		if (strncmp(alias_path, s_volumes[i].alias, alias_len) == 0) {
			const char *rest = alias_path + alias_len;
			size_t volume_len = strlen(s_volumes[i].volume);
			size_t rest_len = strlen(rest);

			if (volume_len + rest_len + 1 > target_size) {
				ATARI_RuntimeLog("path error - %s too long for the "
						 "translated buffer", alias_path);
				return -1;
			}
			memcpy(target, s_volumes[i].volume, volume_len);
			memcpy(target + volume_len, rest, rest_len + 1);
			return 0;
		}
	}

	/* No recognised volume: treat the path as being on the SD card. */
	{
		size_t len = strlen(alias_path);
		if (len + 4 > target_size) {	/* "SD:" + path + NUL */
			ATARI_RuntimeLog("path error - %s too long to prefix "
					 "with the SD volume", alias_path);
			return -1;
		}
		if (alias_path[0] == '/') {
			memcpy(target, "SD:", 3);
			memcpy(target + 3, alias_path, len + 1);
		}
		else {
			memcpy(target, "SD:/", 4);
			memcpy(target + 4, alias_path, len + 1);
		}
		return 0;
	}
}

/* ---------------------------------------------------------------------------
 * Handle allocation
 * ------------------------------------------------------------------------- */

static struct atari_vfs_file *AllocFile(void)
{
	int i;
	for (i = 0; i < ATARI_VFS_MAX_FILES; ++i) {
		if (!s_files[i].used) {
			memset(&s_files[i], 0, sizeof s_files[i]);
			s_files[i].used = 1;
			return &s_files[i];
		}
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * Files
 * ------------------------------------------------------------------------- */

FILE *ATARI_VFS_Open(const char *path, const char *mode)
{
	char fat_path[ATARI_VFS_PATH_MAX];
	BYTE fat_mode = 0;
	int want_read = 0;
	int want_write = 0;
	int create = 0;
	int truncate_to_zero = 0;
	int append = 0;
	struct atari_vfs_file *file;

	if (path == 0 || mode == 0) {
		return 0;
	}
	if (TranslatePath(path, fat_path, sizeof fat_path) != 0) {
		return 0;
	}

	/* Decode the C mode string. */
	switch (mode[0]) {
	case 'r':
		want_read = 1;
		break;
	case 'w':
		want_write = 1;
		create = 1;
		truncate_to_zero = 1;
		break;
	case 'a':
		want_write = 1;
		create = 1;
		append = 1;
		break;
	default:
		return 0;
	}

	if (strchr(mode, '+') != 0 && !append) {
		want_read = 1;
	}

	if (want_read) {
		fat_mode |= FA_READ;
	}
	if (want_write) {
		fat_mode |= FA_WRITE;
	}

	if (append) {
		/* FA_OPEN_APPEND creates the file if needed and seeks to its end. */
		fat_mode |= FA_OPEN_APPEND;
	}
	else if (create) {
		if (truncate_to_zero) {
			fat_mode |= FA_CREATE_ALWAYS;
		}
		else {
			fat_mode |= FA_OPEN_ALWAYS;
		}
	}
	else {
		fat_mode |= FA_OPEN_EXISTING;
	}

	file = AllocFile();
	if (file == 0) {
		return 0;
	}

	if (f_open(&file->fil, fat_path, fat_mode) != FR_OK) {
		file->used = 0;

		if (s_file_trace_left > 0) {
			s_file_trace_left--;
			ATARI_RuntimeLog("file open %s -> FAILED", path);
		}

		return 0;
	}

	/* A "w" open must end up empty even if the file already existed. */
	if (truncate_to_zero) {
		f_lseek(&file->fil, 0);
		f_truncate(&file->fil);
	}

	/*
	 * Logged on success as well as failure, and before the handle is returned.
	 *
	 * The earlier trace only logged closes, which was useless for a disk image:
	 * it is opened once and held for the whole session, so the one operation
	 * that was traced never happened.  Opens and seeks are what the mount and
	 * the boot path actually perform.
	 */
	if (s_file_trace_left > 0) {
		s_file_trace_left--;
		ATARI_RuntimeLog("file open %s -> ok, size %lu", path,
				 (unsigned long) f_size(&file->fil));
	}

	return HANDLE_OF(file);
}

int ATARI_VFS_Close(FILE *handle)
{
	struct atari_vfs_file *file = FileFromHandle(handle);
	FRESULT result;

	if (file == 0) {
		ATARI_RuntimeLog("file close error - invalid handle");
		return -1;
	}
	result = f_close(&file->fil);
	file->used = 0;

	if (s_file_trace_left > 0) {
		s_file_trace_left--;
		ATARI_RuntimeLog("file close -> %d", (int) result);
	}

	return result == FR_OK ? 0 : -1;
}

/*
 * Cumulative I/O counters.
 *
 * Cheap enough to keep always on, and reported by the periodic statistics line.
 * They replace an earlier per-read trace, which cost a flushed card write and a
 * timestamp per sector and buried every other line in the log.
 */
static unsigned long s_read_count;
static unsigned long long s_read_bytes;
static unsigned long s_read_errors;

void ATARI_VFS_BeginFileTrace(void)
{
	/*
	 * Effectively uncapped.
	 *
	 * The original 40-operation budget was a rate limiter copied from the read
	 * trace, but file opens, seeks and closes happen a handful of times during a
	 * boot, not thousands.  Capping them saved nothing and threw away exactly
	 * the detail being looked for.  Reads keep their own separate limit because
	 * those really can run into the thousands.
	 */
	s_file_trace_left = 1000000;
	ATARI_RuntimeLog("file trace: armed, file operations from here are logged");
}

/*
 * Bounded trace of the reads made while a machine is booting.
 *
 * A machine that boots from a disk image reads a recognisable sequence of
 * sectors, so the trace shows how far the boot got and which read was the last
 * one issued.  It is opened by ATARI_VFS_BeginLoadTrace() -- from the cold reset
 * path -- and closes itself after a fixed number of reads, so a program that
 * reads continuously cannot flood the card.
 */
#define LOAD_TRACE_READS	448

static unsigned s_load_trace_left;
static unsigned s_load_trace_total;

void ATARI_VFS_BeginLoadTrace(void)
{
	s_load_trace_left = LOAD_TRACE_READS;
	s_load_trace_total = 0;
	ATARI_RuntimeLog("load trace: open, first %d reads will be logged",
	                 LOAD_TRACE_READS);
}

void ATARI_VFS_GetHandleStats(unsigned *pUsed, unsigned *pTotal)
{
	unsigned used = 0;

	for (unsigned i = 0; i < ATARI_VFS_MAX_FILES; i++) {
		if (s_files[i].used) {
			used++;
		}
	}

	if (pUsed != 0) {
		*pUsed = used;
	}
	if (pTotal != 0) {
		*pTotal = ATARI_VFS_MAX_FILES;
	}
}

void ATARI_VFS_GetStats(unsigned long *pReads, unsigned long long *pBytes,
			unsigned long *pErrors)
{
	if (pReads != 0) {
		*pReads = s_read_count;
	}
	if (pBytes != 0) {
		*pBytes = s_read_bytes;
	}
	if (pErrors != 0) {
		*pErrors = s_read_errors;
	}
}

size_t ATARI_VFS_Read(void *buffer, size_t size, size_t count, FILE *handle)
{
	struct atari_vfs_file *file = FileFromHandle(handle);
	UINT requested;
	UINT got = 0;

	if (file == 0 || size == 0 || count == 0) {
		return 0;
	}

	/* FatFs counts in bytes, so clamp to what a single UINT can express. */
	if (count > (size_t)0xFFFFFFFFu / size) {
		count = (size_t)0xFFFFFFFFu / size;
	}
	requested = (UINT)(size * count);

	/*
	 * A per-read log was tried here and removed: the emulated machine issues
	 * thousands of them, each line costs a flushed card write and a timestamp,
	 * and it buried the lines that mattered.  What is worth knowing about the
	 * disk is when it is attached and how far the machine got, and both are
	 * recorded elsewhere -- see ATARI_MountDisk() and the frame trace.
	 *
	 * The cumulative counters below are what a periodic summary reports, so the
	 * cost is a couple of additions per read rather than a card write.
	 */
	if (s_load_trace_left > 0) {
		s_load_trace_left--;
		s_load_trace_total++;
		ATARI_RuntimeLog("load read %u: off %lu len %lu",
		                 s_load_trace_total,
		                 (unsigned long)f_tell(&file->fil),
		                 (unsigned long)requested);
	}

	if (f_read(&file->fil, buffer, requested, &got) != FR_OK) {
		file->error = 1;
		s_read_errors++;
		return 0;
	}

	if (s_load_trace_left == 0 && s_load_trace_total == LOAD_TRACE_READS) {
		/* One closing line, so the log says why the trace stopped. */
		s_load_trace_total++;
		ATARI_RuntimeLog("load trace: closed after %d reads",
		                 LOAD_TRACE_READS);
	}

	s_read_count++;
	s_read_bytes += got;

	if (got < requested) {
		file->eof = 1;
	}
	return got / size;
}

size_t ATARI_VFS_Write(const void *buffer, size_t size, size_t count, FILE *handle)
{
	struct atari_vfs_file *file = FileFromHandle(handle);
	UINT requested;
	UINT put = 0;

	if (file == 0 || size == 0 || count == 0) {
		return 0;
	}

	if (count > (size_t)0xFFFFFFFFu / size) {
		count = (size_t)0xFFFFFFFFu / size;
	}
	requested = (UINT)(size * count);

	if (f_write(&file->fil, buffer, requested, &put) != FR_OK) {
		file->error = 1;
		return 0;
	}
	return put / size;
}

int ATARI_VFS_Seek(FILE *handle, long offset, int whence)
{
	if (s_file_trace_left > 0) {
		s_file_trace_left--;
		ATARI_RuntimeLog("file seek off %ld whence %d", offset, whence);
	}
	struct atari_vfs_file *file = FileFromHandle(handle);
	FSIZE_t target;

	if (file == 0) {
		ATARI_RuntimeLog("file seek error - invalid handle");
		return -1;
	}

	switch (whence) {
	case SEEK_SET:
		target = (FSIZE_t)(offset < 0 ? 0 : (unsigned long)offset);
		break;
	case SEEK_CUR:
		{
			FSIZE_t current = f_tell(&file->fil);
			if (offset < 0) {
				unsigned long back = (unsigned long)(-offset);
				target = back > current ? 0 : current - back;
			}
			else {
				target = current + (unsigned long)offset;
			}
		}
		break;
	case SEEK_END:
		{
			FSIZE_t size = f_size(&file->fil);
			if (offset < 0) {
				unsigned long back = (unsigned long)(-offset);
				target = back > size ? 0 : size - back;
			}
			else {
				target = size + (unsigned long)offset;
			}
		}
		break;
	default:
		ATARI_RuntimeLog("file seek error - invalid mode %d", whence);
		return -1;
	}

	if (f_lseek(&file->fil, target) != FR_OK) {
		file->error = 1;
		ATARI_RuntimeLog("file seek error - failed to seek");
		return -1;
	}

	/* Seeking clears the end-of-file indication, as in C. */
	file->eof = 0;
	return 0;
}

long ATARI_VFS_Tell(FILE *handle)
{
	struct atari_vfs_file *file = FileFromHandle(handle);

	if (file == 0) {
		ATARI_RuntimeLog("file tell error - invalid handle");
		return -1;
	}
	return (long)f_tell(&file->fil);
}

void ATARI_VFS_Rewind(FILE *handle)
{
	struct atari_vfs_file *file = FileFromHandle(handle);

	if (file != 0) {
		(void)ATARI_VFS_Seek(handle, 0, SEEK_SET);
	}
}

int ATARI_VFS_Flush(FILE *handle)
{
	struct atari_vfs_file *file = FileFromHandle(handle);

	if (file == 0) {
		ATARI_RuntimeLog("file flush error - invalid handle");
		return -1;
	}
	if (f_sync(&file->fil) != FR_OK) {
		ATARI_RuntimeLog("file flush error - f_sync failed");
		return -1;
	}
	return 0;
}

int ATARI_VFS_Eof(FILE *handle)
{
	struct atari_vfs_file *file = FileFromHandle(handle);

	if (file == 0) {
		return 1;
	}
	/*
	 * FatFs reports end-of-file through the read count, so track it here and
	 * also treat "position at or past size" as end-of-file.
	 */
	if (file->eof) {
		return 1;
	}
	return f_tell(&file->fil) >= f_size(&file->fil) ? 1 : 0;
}

int ATARI_VFS_Error(FILE *handle)
{
	struct atari_vfs_file *file = FileFromHandle(handle);

	if (file == 0) {
		return 1;
	}
	return file->error;
}

int ATARI_VFS_Truncate(FILE *handle)
{
	struct atari_vfs_file *file = FileFromHandle(handle);

	if (file == 0) {
		ATARI_RuntimeLog("file truncate error - invalid handle");
		return -1;
	}
	if (f_truncate(&file->fil) != FR_OK) {
		ATARI_RuntimeLog("file truncate error - f_truncate failed");
		return -1;
	}
	return 0;
}

long ATARI_VFS_Size(FILE *handle)
{
	struct atari_vfs_file *file = FileFromHandle(handle);

	if (file == 0) {
		ATARI_RuntimeLog("file size error - invalid handle");
		return -1;
	}
	return (long)f_size(&file->fil);
}

/* ---------------------------------------------------------------------------
 * Directories
 * ------------------------------------------------------------------------- */

static struct atari_vfs_dir *AllocDir(void)
{
	int i;
	for (i = 0; i < ATARI_VFS_MAX_DIRS; ++i) {
		if (!s_dirs[i].used) {
			memset(&s_dirs[i], 0, sizeof s_dirs[i]);
			s_dirs[i].used = 1;
			return &s_dirs[i];
		}
	}
	return 0;
}

atari_vfs_dir_t *ATARI_VFS_OpenDir(const char *path)
{
	char fat_path[ATARI_VFS_PATH_MAX];
	struct atari_vfs_dir *dir;

	if (path == 0 || TranslatePath(path, fat_path, sizeof fat_path) != 0) {
		return 0;
	}

	dir = AllocDir();
	if (dir == 0) {
		return 0;
	}

	if (f_opendir(&dir->dir, fat_path) != FR_OK) {
		ATARI_RuntimeLog("dir open FAILED for %s", path);
		dir->used = 0;
		return 0;
	}
	return dir;
}

struct dirent *ATARI_VFS_ReadDir(atari_vfs_dir_t *dir)
{
	if (dir == 0 || !dir->used) {
		return 0;
	}

	for (;;) {
		if (f_readdir(&dir->dir, &dir->info) != FR_OK) {
			ATARI_RuntimeLog("dir read FAILED (f_readdir error)");
			return 0;
		}
		/* An empty name marks the end of the directory. */
		if (dir->info.fname[0] == '\0') {
			return 0;
		}
		/* Skip directories: the emulation core only looks at files. */
		if (dir->info.fattrib & AM_DIR) {
			continue;
		}

		{
			size_t len = strlen(dir->info.fname);
			if (len >= sizeof dir->entry.d_name) {
				len = sizeof dir->entry.d_name - 1;
			}
			memcpy(dir->entry.d_name, dir->info.fname, len);
			dir->entry.d_name[len] = '\0';
		}
		return &dir->entry;
	}
}

int ATARI_VFS_CloseDir(atari_vfs_dir_t *dir)
{
	if (dir == 0 || !dir->used) {
		ATARI_RuntimeLog("dir close error - invalid handle");
		return -1;
	}
	f_closedir(&dir->dir);
	dir->used = 0;
	return 0;
}
