/*
 * ui_stub.c -- headless stand-in for the Atari800 core's built-in user
 *              interface.
 *
 * Atari800_Initialise() calls UI_Initialise() unconditionally, and the core's
 * own UI implementation (src/ui.c plus src/ui_basic.c) is a complete on-screen
 * menu system with its own file browser.  This project replaces all of that
 * with its own OSD menu, so the upstream UI is not compiled in.
 *
 * What remains is this file, which supplies just enough of the UI to let the
 * emulation core link and initialise:
 *
 *   - UI_Initialise() / UI_Exit() succeed and do nothing.
 *   - UI_Run() is never reached in normal operation, but if it ever is, it
 *     presents the current frame rather than pretending to draw a menu.
 *   - UI_driver points at a table of functions that all decline politely.
 *
 * The core only ever calls into the driver from src/ui.c, which is not
 * compiled here, so these entry points exist purely to satisfy the linker and
 * to keep the core's initialisation sequence -- which is the tested upstream
 * one -- intact.
 */

#include "config.h"

#include <string.h>

#include "atari.h"
#include "cartridge.h"
#include "platform.h"
#include "screen.h"
#include "ui.h"
#include "libatari800/libatari800.h"

/* ---------------------------------------------------------------------------
 * Globals the core expects the UI to provide
 *
 * Upstream defines these either in its full UI (src/ui.c) or in its library
 * front end (src/libatari800/main.c).  Neither is compiled here, so they are
 * declared with the values that mean "no user interface is running".
 * ------------------------------------------------------------------------- */

int UI_is_active = FALSE;
int UI_alt_function = 0;
int UI_current_function = 0;

char UI_atari_files_dir[UI_MAX_DIRECTORIES][FILENAME_MAX];
char UI_saved_files_dir[UI_MAX_DIRECTORIES][FILENAME_MAX];
int UI_n_atari_files_dir = 0;
int UI_n_saved_files_dir = 0;
int UI_show_hidden_files = FALSE;

/*
 * The core calls this when a cartridge image of unknown type is inserted.  A
 * full UI would ask the user which type it is; there is no interactive
 * fallback here, so report the condition to the caller instead of guessing.
 */
int UI_SelectCartType(int k)
{
	(void)k;
	return CARTRIDGE_NONE;
}

/* ---------------------------------------------------------------------------
 * Driver entry points
 *
 * Every one of these returns the value that means "the user dismissed this
 * without choosing anything", so that no caller can mistake a stub for a real
 * selection.
 * ------------------------------------------------------------------------- */

static int Stub_Select(const char *title, int flags, int default_item,
                       const UI_tMenuItem *menu, int *seltype)
{
	(void)title;
	(void)flags;
	(void)default_item;
	(void)menu;
	if (seltype != NULL) {
		*seltype = 0;
	}
	return -1;			/* cancelled */
}

static int Stub_SelectInt(int default_value, int min_value, int max_value)
{
	(void)min_value;
	(void)max_value;
	return default_value;		/* unchanged */
}

static int Stub_SelectSlider(const char *title, int start_value, int max_value,
                             void (*label_fun)(char *label, int value, void *user_data),
                             void *user_data)
{
	(void)title;
	(void)max_value;
	(void)label_fun;
	(void)user_data;
	return start_value;		/* unchanged */
}

static int Stub_EditString(const char *title, char *string, int size)
{
	(void)title;
	(void)string;
	(void)size;
	return -1;			/* cancelled */
}

static int Stub_GetSaveFilename(char *filename, char directories[][FILENAME_MAX],
                                int n_directories)
{
	(void)filename;
	(void)directories;
	(void)n_directories;
	return FALSE;
}

static int Stub_GetLoadFilename(char *filename, char directories[][FILENAME_MAX],
                                int n_directories)
{
	(void)filename;
	(void)directories;
	(void)n_directories;
	return FALSE;
}

static int Stub_GetDirectoryPath(char *directory)
{
	(void)directory;
	return FALSE;
}

static void Stub_Message(const char *message, int waitforkey)
{
	(void)message;
	(void)waitforkey;
}

static void Stub_InfoScreen(const char *title, const char *message)
{
	(void)title;
	(void)message;
}

static void Stub_Init(void)
{
}

static UI_tDriver s_stub_driver = {
	Stub_Select,
	Stub_SelectInt,
	Stub_SelectSlider,
	Stub_EditString,
	Stub_GetSaveFilename,
	Stub_GetLoadFilename,
	Stub_GetDirectoryPath,
	Stub_Message,
	Stub_InfoScreen,
	Stub_Init
};

UI_tDriver *UI_driver = &s_stub_driver;

/* ---------------------------------------------------------------------------
 * Core lifecycle
 * ------------------------------------------------------------------------- */

int UI_Initialise(int *argc, char *argv[])
{
	(void)argc;
	(void)argv;
	return TRUE;
}

void UI_Exit(void)
{
}

void UI_Run(void)
{
	/*
	 * The OSD menu is implemented by this project and runs from the main
	 * loop, so the core's UI is never asked to run.  If some path does reach
	 * here, the least surprising behaviour is to show what the emulated
	 * machine is displaying and return immediately.
	 */
	PLATFORM_DisplayScreen();
}

/* ---------------------------------------------------------------------------
 * Small pieces the core expects from the library front end
 * ------------------------------------------------------------------------- */

/*
 * SIO calls this when a disk sector is read or written, so that a front end
 * can show drive activity.  This project has its own indicator in the OSD, and
 * nothing here needs to know, so it stays NULL and the core tests for that.
 */
void (*disk_activity_callback)(int drive, int operation) = NULL;

/*
 * The core tears the monitor down during shutdown; it is not compiled in here,
 * so this is the no-op that keeps Atari800_Exit() self-contained.
 */
void MONITOR_Exit(void)
{
}

/*
 * The core's own front end calls this once per frame to pace the emulation.
 * It is not compiled in here because frame pacing belongs to this project's
 * platform layer (see ATARI_RunFrame), so reaching it would mean a frame is
 * being driven from somewhere unexpected.  Doing nothing is the safe answer:
 * the pace is still governed by ATARI_RunFrame().
 */
void Atari800_Sync(void)
{
}

/* ===========================================================================
 * Symbols the core expects the embedding library front end to provide
 *
 * Upstream's libatari800 defines these in src/libatari800/api.c.  That file is
 * not compiled here -- this project drives the core directly instead -- so the
 * few globals and functions the core still refers to are supplied here.
 * ======================================================================== */

/*
 * Whether a BRK instruction should resume emulation rather than abandon the
 * frame.
 *
 * One -- the value here, and the emulator's default -- runs BRK as the 6502
 * does: push the return address, set I, and vector through $FFFE, which is how
 * a debugger such as DDT takes control.  The settings file asks for the other
 * behaviour with CONTINUE_ON_BRK=0, applied through ATARI_SetContinueOnBrk().
 */
int libatari800_continue_on_brk = 1;

/*
 * The core's public setter for the flag above.
 *
 * Upstream defines this in src/libatari800/api.c, which is not compiled here --
 * this project drives the core directly -- so the definition belongs with the
 * variable.  It is declared in libatari800/libatari800.h.
 */
void libatari800_continue_emulation_on_brk(int cont)
{
	libatari800_continue_on_brk = cont ? 1 : 0;
}

/*
 * The result of the last frame, and a human readable description of it.
 *
 * The core's own save-state backend also reports through these, which is why
 * they exist at all; the kernel reads them through ATARI_LastError().
 */
int libatari800_error_code = 0;

static const char *const s_error_messages[] = {
	"no error",
	"an unidentified cartridge type was inserted",
	"the emulated CPU stopped responding",
	"the emulated machine executed a BRK instruction",
	"the display list is invalid",
	"the emulated machine entered self-test",
	"the emulated machine entered Memo Pad",
	"the emulated machine executed an invalid escape opcode",
};

const char *libatari800_error_message(void)
{
	unsigned int index = (unsigned int)libatari800_error_code;

	if (index >= sizeof s_error_messages / sizeof s_error_messages[0]) {
		return "unknown emulation error";
	}
	return s_error_messages[index];
}

/* ===========================================================================
 * Symbols newlib expects the platform to provide
 *
 * Linking newlib's libc gives the emulation core the string, conversion and
 * formatted-output routines it needs.  newlib's own start-up and system-call
 * glue then expects a handful of symbols that a bare-metal platform has to
 * supply.  None of them is reached in this program:
 *
 *   - _fini is called by __libc_fini_array during exit(), and the emulator never
 *     exits normally;
 *   - end is what newlib's _sbrk grows the heap from, and it is never used
 *     because Circle installs its own allocator for malloc and new;
 *   - _exit is reached only from abort() and exit().
 *
 * They exist so that the link succeeds, and so that a mistake which did reach
 * them halts visibly rather than jumping into nothing.
 * ======================================================================== */

/* Marks the end of the kernel's static data.  See the note above. */
char end;
char _end;

void _fini(void)
{
}

void _exit(int status)
{
	(void)status;
	for (;;) {
		/* A deliberate dead end: there is nothing to return to. */
	}
}
