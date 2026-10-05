/* $XTermId: luit.c,v 1.77 2025/09/12 08:20:14 tom Exp $ */

/*
Copyright 2010-2022,2025 by Thomas E. Dickey
Copyright (c) 2001 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <luit.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <termios.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>

#include <locale.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <signal.h>

#include <version.h>
#include <sys.h>
#include <parser.h>
#include <iso2022.h>

static int pipe_option = 0;
static int p2c_waitpipe[2];
static int c2p_waitpipe[2];

static Iso2022Ptr inputState = NULL, outputState = NULL;

static char *child_argv0 = NULL;
static const char *locale_name = NULL;
/* PATCH(fork, input rejection): where to report rejected input (-notify) */
static const char *notify_path = NULL;
/* PATCH(fork, title): see updateTitle() */
static const char *title_suffix = NULL;
/* PATCH(fork, task command line): see encodeLastArg() */
static int encode_last_arg = 0;
static char *title_area = NULL;
static size_t title_area_len = 0;
/* The pid VS Code knows this terminal by: luit's, or the shell's when the
 * process tree is inverted (see condomInverted) */
static long terminal_pid = 0;
static int exitOnChild = 0;
static int converter = 0;
static int testonly = 0;
static int warnings = 0;

const char *locale_alias = LOCALE_ALIAS_FILE;

int ilog = -1;
int olog = -1;
int verbose = 0;
int ignore_locale = 0;
int fill_fontenc = 0;
int input_unencodable = 0;
unsigned input_unencodable_char = 0;

#ifdef USE_ICONV
/* PATCH(fork, built-in tables only): upstream also looks in the system's
 * ".enc" files (fontenc, first), the C library's iconv, and finally takes
 * bytes as code points (posix). What those give depends on the machine:
 * with X11's font encodings installed, gbk-0 and big5hkscs-0 decoded with
 * their data instead of ours, and musl's iconv, which the distributed
 * binaries have, knows none of CP1253/1254/1256/1257/1258/874. Every
 * charset the supported encodings use has a built-in table (builtin.c or
 * the fork's builtin_ja.c), so only those are used (-prefer can name one
 * other source to use instead). */
UM_MODE lookup_order[] =
{
    umBUILTIN, umNONE
};
#endif

static volatile int sigwinch_queued = 0;
static volatile int sigchld_queued = 0;

static int convert(int, int);
static int condom(int, char **);
static void child(int sfd, char *, char *, char *const *);

void
Message(const char *f, ...)
{
    va_list args;
    va_start(args, f);
    vfprintf(stderr, f, args);
    va_end(args);
}

void
Warning(const char *f, ...)
{
    va_list args;
    va_start(args, f);
    fputs("Warning: ", stderr);
    vfprintf(stderr, f, args);
    va_end(args);
    ++warnings;
}

void
FatalError(const char *f, ...)
{
    va_list args;
    va_start(args, f);
    vfprintf(stderr, f, args);
    va_end(args);
    ExitFailure();
}

static void
help(const char *program, int fatal)
{
#define DATA(name,mark,what) { #mark name, what }
    /* *INDENT-OFF* */
    static const struct {
	const char *name;
	const char *value;
    } options[] = {
	/* sorted per POSIX */
	DATA("V", -, "show version"),
	DATA("alias filename", -, "location of the locale alias file"),
	DATA("argv0 name", -, "set child's name"),
	DATA("c", -, "simple converter stdin/stdout"),
	DATA("encoding encoding", -, "use this encoding rather than current locale's encoding"),
	DATA("fill-fontenc", -, "fill in one-one mapping in -show-fontenc report"),
	DATA("g0 set", -, "set output G0 charset (default ASCII)"),
	DATA("g1 set", -, "set output G1 charset"),
	DATA("g2 set", -, "set output G2 charset"),
	DATA("g3 set", -, "set output G3 charset"),
	DATA("gl gn", -, "set output GL charset"),
	DATA("gr gk", -, "set output GR charset"),
	DATA("h", -, "show this message"),
	DATA("ilog filename", -, "log all input to this file"),
	DATA("k7", -, "generate 7-bit characters for input"),
	DATA("kg0 set", -, "set input G0 charset"),
	DATA("kg1 set", -, "set input G1 charset"),
	DATA("kg2 set", -, "set input G2 charset"),
	DATA("kg3 set", -, "set input G3 charset"),
	DATA("kgl gn", -, "set input GL charset"),
	DATA("kgr gk", -, "set input GR charset"),
	DATA("kls", -, "generate locking shifts SI/SO"),
	DATA("kss", +, "disable generation of single-shifts for input"),
	DATA("kssgr", +, "use GL after single-shift"),
	DATA("list", -, "list encodings recognized by this program"),
	DATA("list-builtin", -, "list built-in encodings"),
	DATA("list-fontenc", -, "list available \".enc\" encoding files"),
	DATA("list-iconv", -, "list iconv-supported encodings"),
	DATA("notify dir", -, "report rejected input to the Unix sockets in this directory"),
	DATA("title-suffix text", -, "name this process after the inner foreground program + text"),
	DATA("encode-last-arg", -, "convert the program's last argument from UTF-8, as if typed"),
	DATA("olog filename", -, "log all output to this file"),
	DATA("ols", +, "disable locking-shifts in output"),
	DATA("osl", +, "disable charset-selection sequences in output"),
	DATA("oss", +, "disable single-shifts in output"),
	DATA("ot", +, "disable interpretation of all sequences in output"),
	DATA("p", -, "do parent/child handshake"),
	DATA("prefer list", -, "override preference between fontenc/iconv lookups"),
	DATA("show-builtin enc", -, "show details of a given built-in encoding"),
	DATA("show-fontenc enc", -, "show details of an \".enc\" encoding file"),
	DATA("show-iconv enc", -, "show iconv encoding in \".enc\" format"),
	DATA("t", -, "testing (initialize locale but no terminal)"),
	DATA("v", -, "verbose (repeat to increase level)"),
	DATA("x", -, "exit as soon as child dies"),
	DATA("-", -, "end of options"),
    };
    /* *INDENT-ON* */

    size_t n;
    size_t col, now;
    FILE *fp = fatal ? stderr : stdout;

    if (fatal) {
	col = 0;
	fprintf(fp, "%s\n", program);
	for (n = 0; n < SizeOf(options); ++n) {
	    if (col == 0) {
		fprintf(fp, " ");
		col = 1;
	    }
	    now = strlen(options[n].name) + 5;
	    col += now;
	    if (col > MAXCOLS) {
		fprintf(fp, "\n ");
		col = now + 1;
	    }
	    fprintf(fp, " [ %s ]", options[n].name);
	}
	if (col)
	    fprintf(fp, "\n");
	fprintf(fp, "  [ program [ args ] ]\n");
    } else {
	fprintf(fp, "Usage: %s [options] [ program [ args ] ]\n", program);
	fprintf(fp, "\n");
	fprintf(fp, "Options:\n");
	col = 0;
	for (n = 0; n < SizeOf(options); ++n) {
	    now = strlen(options[n].name);
	    if (now > col)
		col = now;
	}
	for (n = 0; n < SizeOf(options); ++n) {
	    fprintf(fp, "  %-*s  %s\n", (int) col, options[n].name,
		    options[n].value);
	}
    }
    fflush(fp);

    if (fatal)
	ExitFailure();
}

#ifdef USE_ICONV
static void
setLookupOrder(const char *name)
{
    /* *INDENT-OFF* */
    static const struct {
	UM_MODE order;
	const char *name;
    } table[] = {
	{ umBUILTIN,  "builtin" },
	{ umFONTENC,  "fontenc" },
	{ umICONV,    "iconv" },
	{ umPOSIX,    "posix" },
    };
    /* *INDENT-ON* */

    size_t j, k;
    char *toparse = strmalloc(name);
    char *tomatch = toparse;
    char *token;
    UM_MODE new_list[SizeOf(lookup_order)];
    size_t limit = SizeOf(lookup_order) - 1;
    size_t used = 0;

    TRACE(("setLookupOrder(%s)\n", NonNull(name)));
    while ((token = strtok(tomatch, ",")) != NULL) {
	UM_MODE order = umNONE;
	size_t length = strlen(token);

	tomatch = NULL;
	for (j = 0; j < SizeOf(table); ++j) {
	    if (length <= strlen(table[j].name)
		&& !strncmp(token, table[j].name, length)) {
		order = table[j].order;
		break;
	    }
	}
	if (order == umNONE) {
	    FatalError("invalid item in -prefer option: %s\n", token);
	}
	if (used >= limit) {
	    FatalError("too many items in -prefer option: %s\n", name);
	}
	new_list[used++] = order;
    }

    while (used < limit) {
	for (j = 0; j < limit; ++j) {
	    int found = 0;
	    for (k = 0; k < used; ++k) {
		if (lookup_order[j] == new_list[k]) {
		    ++found;
		}
	    }
	    if (!found) {
		new_list[used++] = lookup_order[j];
	    } else if (found > 1) {
		FatalError("repeated keyword in -prefer option: %s\n", name);
	    }
	}
    }

    VERBOSE(1, ("Lookup order: "));
    for (j = 0; j < limit; ++j) {
	lookup_order[j] = new_list[j];
	if (verbose) {
	    for (k = 0; k < SizeOf(table); ++k) {
		if (table[k].order == lookup_order[j]) {
		    if (j)
			VERBOSE(1, (","));
		    VERBOSE(1, ("%s", table[k].name));
		    break;
		}
	    }
	}
    }
    VERBOSE(1, ("\n"));

    free(toparse);
}
#else
static int
needIconvCfg(void)
{
    Message("You need the iconv configuration for this option\n");
    return EXIT_FAILURE;
}

#define reportBuiltinCharsets()  needIconvCfg()
#define reportIconvCharsets()    needIconvCfg()
#define setLookupOrder(name)     needIconvCfg()
#define showBuiltinCharset(name) needIconvCfg()
#define showIconvCharset(name)   needIconvCfg()
#endif

static char *
needParam(int argc, char **argv, int now)
{
    if (now + 1 >= argc)
	FatalError("%s requires an argument\n", argv[now]);
    return (argv[now + 1]);
}
#define getParam(now) needParam(argc, argv, now)

static int
parseOptions(int argc, char **argv)
{
    int i = 1;
    while (i < argc) {
	if (argv[i][0] != '-' && argv[i][0] != '+') {
	    break;
	} else if (!strcmp(argv[i], "--")) {
	    i++;
	    break;
	} else if (!strcmp(argv[i], "-v")) {
	    verbose++;
	    i++;
	} else if (!strcmp(argv[i], "-V")) {
	    printf("%s - %s\n", argv[0], LUIT_VERSION);
	    ExitSuccess();
	} else if (!strcmp(argv[i], "-h")) {
	    help(argv[0], 0);
	    ExitSuccess();
	} else if (!strcmp(argv[i], "-list")) {
	    reportCharsets();
	    ExitSuccess();
	} else if (!strcmp(argv[i], "-fill-fontenc")) {
	    fill_fontenc = 1;
	    i++;
	} else if (!strcmp(argv[i], "-prefer")) {
	    setLookupOrder(getParam(i));
	    i += 2;
	} else if (!strcmp(argv[i], "-show-builtin")) {
	    ExitProgram(showBuiltinCharset(getParam(i)));
	} else if (!strcmp(argv[i], "-show-fontenc")) {
	    ExitProgram(showFontencCharset(getParam(i)));
	} else if (!strcmp(argv[i], "-show-iconv")) {
	    ExitProgram(showIconvCharset(getParam(i)));
	} else if (!strcmp(argv[i], "-list-builtin")) {
	    ExitProgram(reportBuiltinCharsets());
	} else if (!strcmp(argv[i], "-list-fontenc")) {
	    ExitProgram(reportFontencCharsets());
	} else if (!strcmp(argv[i], "-list-iconv")) {
	    ExitProgram(reportIconvCharsets());
	} else if (!strcmp(argv[i], "+oss")) {
	    outputState->outputFlags &= ~OF_SS;
	    i++;
	} else if (!strcmp(argv[i], "+ols")) {
	    outputState->outputFlags &= ~OF_LS;
	    i++;
	} else if (!strcmp(argv[i], "+osl")) {
	    outputState->outputFlags &= ~OF_SELECT;
	    i++;
	} else if (!strcmp(argv[i], "+ot")) {
	    outputState->outputFlags = OF_PASSTHRU;
	    i++;
	} else if (!strcmp(argv[i], "-k7")) {
	    inputState->inputFlags &= ~IF_EIGHTBIT;
	    i++;
	} else if (!strcmp(argv[i], "+kss")) {
	    inputState->inputFlags &= ~IF_SS;
	    i++;
	} else if (!strcmp(argv[1], "+kssgr")) {
	    inputState->inputFlags &= ~IF_SSGR;
	    i++;
	} else if (!strcmp(argv[i], "-kls")) {
	    inputState->inputFlags |= IF_LS;
	    i++;
	} else if (!strcmp(argv[i], "-g0")) {
	    G0(outputState) = getCharsetByName(getParam(i));
	    i += 2;
	} else if (!strcmp(argv[i], "-g1")) {
	    G1(outputState) = getCharsetByName(getParam(i));
	    i += 2;
	} else if (!strcmp(argv[i], "-g2")) {
	    G2(outputState) = getCharsetByName(getParam(i));
	    i += 2;
	} else if (!strcmp(argv[i], "-g3")) {
	    G3(outputState) = getCharsetByName(getParam(i));
	    i += 2;
	} else if (!strcmp(argv[i], "-gl")) {
	    int j;
	    if (strlen(getParam(i)) != 2 ||
		argv[i + 1][0] != 'g')
		j = -1;
	    else
		j = argv[i + 1][1] - '0';
	    if (j < 0 || j > 3)
		FatalError("The argument of -gl "
			   "should be one of g0 through g3,\n"
			   "not %s\n", argv[i + 1]);
	    else
		outputState->glp = &outputState->g[j];
	    i += 2;
	} else if (!strcmp(argv[i], "-gr")) {
	    int j;
	    if (strlen(getParam(i)) != 2 ||
		argv[i + 1][0] != 'g')
		j = -1;
	    else
		j = argv[i + 1][1] - '0';
	    if (j < 0 || j > 3)
		FatalError("The argument of -gl "
			   "should be one of g0 through g3,\n"
			   "not %s\n", argv[i + 1]);
	    else
		outputState->grp = &outputState->g[j];
	    i += 2;
	} else if (!strcmp(argv[i], "-kg0")) {
	    G0(inputState) = getCharsetByName(getParam(i));
	    i += 2;
	} else if (!strcmp(argv[i], "-kg1")) {
	    G1(inputState) = getCharsetByName(getParam(i));
	    i += 2;
	} else if (!strcmp(argv[i], "-kg2")) {
	    G2(inputState) = getCharsetByName(getParam(i));
	    i += 2;
	} else if (!strcmp(argv[i], "-kg3")) {
	    G3(inputState) = getCharsetByName(getParam(i));
	    i += 2;
	} else if (!strcmp(argv[i], "-kgl")) {
	    int j;
	    if (strlen(getParam(i)) != 2 ||
		argv[i + 1][0] != 'g')
		j = -1;
	    else
		j = argv[i + 1][1] - '0';
	    if (j < 0 || j > 3)
		FatalError("The argument of -kgl "
			   "should be one of g0 through g3,\n"
			   "not %s\n", argv[i + 1]);
	    else
		inputState->glp = &inputState->g[j];
	    i += 2;
	} else if (!strcmp(argv[i], "-kgr")) {
	    int j;
	    if (strlen(getParam(i)) != 2 ||
		argv[i + 1][0] != 'g')
		j = -1;
	    else
		j = argv[i + 1][1] - '0';
	    if (j < 0 || j > 3)
		FatalError("The argument of -kgl "
			   "should be one of g0 through g3,\n"
			   "not %s\n", argv[i + 1]);
	    else
		inputState->grp = &inputState->g[j];
	    i += 2;
	} else if (!strcmp(argv[i], "-argv0")) {
	    child_argv0 = getParam(i);
	    i += 2;
	} else if (!strcmp(argv[i], "-x")) {
	    exitOnChild = 1;
	    i++;
	} else if (!strcmp(argv[i], "-c")) {
	    converter = 1;
	    i++;
	} else if (!strcmp(argv[i], "-title-suffix")) {
	    title_suffix = getParam(i);
	    i += 2;
	} else if (!strcmp(argv[i], "-encode-last-arg")) {
	    encode_last_arg = 1;
	    i++;
	} else if (!strcmp(argv[i], "-notify")) {
	    notify_path = getParam(i);
	    i += 2;
	} else if (!strcmp(argv[i], "-ilog")) {
	    if (ilog >= 0)
		close(ilog);
	    ilog = open(getParam(i), O_WRONLY | O_CREAT | O_TRUNC, 0777);
	    if (ilog < 0) {
		perror("Couldn't open input log");
		ExitFailure();
	    }
	    i += 2;
	} else if (!strcmp(argv[i], "-olog")) {
	    if (olog >= 0)
		close(olog);
	    olog = open(getParam(i), O_WRONLY | O_CREAT | O_TRUNC, 0777);
	    if (olog < 0) {
		perror("Couldn't open output log");
		ExitFailure();
	    }
	    i += 2;
	} else if (!strcmp(argv[i], "-alias")) {
	    locale_alias = getParam(i);
	    i += 2;
	} else if (!strcmp(argv[i], "-encoding")) {
	    locale_name = getParam(i);
	    ignore_locale = 1;
	    i += 2;
	} else if (!strcmp(argv[i], "-p")) {
	    pipe_option = 1;
	    i += 1;
	} else if (!strcmp(argv[i], "-t")) {
	    ++testonly;
	    i += 1;
	} else {
	    Message("Unknown option %s\n", argv[i]);
	    help(argv[0], 1);
	}
    }
    return i;
}

static char *
getShell(void)
{
    const char *shell;
    if ((shell = getenv("SHELL")) == NULL)
	shell = "/bin/sh";
    return strmalloc(shell);
}

static int
isSpecialCommand(int argc, char **argv)
{
    int result = 0;
    if (argc == 1) {
	size_t len = strlen(argv[0]);
	size_t chk = strcspn(argv[0], "~`!$^&*(){}|\\<>?\"' \t");
	if (len != chk) {
	    result = 1;
	}
    }
    return result;
}

static int
parseArgs(int argc, char **argv,
	  char *argv0,
	  char **path_return,
	  char ***argv_return)
{
    char *path = NULL;
    char **child_argv = NULL;

    if (argc <= 0) {
	if ((path = getShell()) == NULL) {
	    goto bail;
	}
	child_argv = malloc(2 * sizeof(char *));
	if (!child_argv)
	    goto bail;
	if (argv0)
	    child_argv[0] = argv0;
	else
	    child_argv[0] = my_basename(path);
	child_argv[1] = NULL;
    } else if (isSpecialCommand(argc, argv)) {
	path = strmalloc("sh");
	child_argv = malloc(4 * sizeof(char *));
	child_argv[0] = argv0 ? argv0 : path;
	child_argv[1] = strmalloc("-c");
	child_argv[2] = argv[0];
	child_argv[3] = NULL;
    } else {
	path = strmalloc(argv[0]);
	if (!path)
	    goto bail;
	child_argv = malloc((unsigned) (argc + 1) * sizeof(char *));
	if (!child_argv) {
	    goto bail;
	}
	if (child_argv0)
	    child_argv[0] = argv0;
	else
	    child_argv[0] = my_basename(argv[0]);
	memcpy(child_argv + 1, argv + 1, (unsigned) (argc - 1) * sizeof(char *));
	child_argv[argc] = NULL;
    }

    *path_return = path;
    *argv_return = child_argv;
    return 0;

  bail:
    free(path);
    return -1;
}
/*
 * PATCH(fork, title): VS Code names a terminal tab after the foreground
 * process of its pty, read from that process's argv[0] (/proc/<pid>/cmdline
 * on Linux). With luit in between that's always luit, so the tab couldn't
 * show "vim" the way a regular terminal does. With -title-suffix, luit
 * names itself after the inner pty's foreground program plus the suffix
 * (the extension passes the encoding, e.g. "vim (EUC-JP)" with a no-break
 * space, since the title is cut at the first ordinary space). It writes
 * into the memory the kernel reports as its command line: the original
 * argument strings, copied elsewhere first so nothing else points there.
 * Linux only: macOS reports the name recorded at exec time.
 */
static void
claimTitleArea(int *argcp, char ***argvp)
{
#ifdef __linux__
    char **copy;
    char *end;
    int k;

    if (*argcp < 1 || (*argvp)[0] == NULL)
	return;
    end = (*argvp)[0];
    for (k = 0; k < *argcp; k++) {
	if ((*argvp)[k] != end)
	    break;		/* only the contiguous part */
	end += strlen((*argvp)[k]) + 1;
    }
    copy = (char **) calloc((size_t) *argcp + 1, sizeof(char *));
    if (copy == NULL)
	return;
    for (k = 0; k < *argcp; k++) {
	copy[k] = strmalloc((*argvp)[k]);
	if (copy[k] == NULL)
	    return;
    }
    title_area = (*argvp)[0];
    title_area_len = (size_t) (end - title_area);
    *argvp = copy;
    /* Started through a link named like the shell (see expandArgsFromEnv),
     * luit's command name would be "bash": `pgrep bash`/`killall bash`
     * would count it. Its real name, for tools that go by that. */
    (void) prctl(PR_SET_NAME, "luit", 0, 0, 0);
#else
    (void) argcp;
    (void) argvp;
#endif
}

#define TITLE_MARKER "[terminal-any-encoding]"

/* Called on every pass of the I/O loop; renames luit when the inner
 * foreground program changes */
static void
updateTitle(int pty)
{
#ifdef __linux__
    char path[64];
    char cmd[256];
    const char *name;
    ssize_t got;
    int fd;
    pid_t fg;

    if (title_suffix == NULL || title_area == NULL || title_area_len < 2)
	return;
    /* The name, not just the process group: `exec vim` keeps the pid */
    fg = tcgetpgrp(pty);
    if (fg <= 0)
	return;
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long) fg);
    fd = open(path, O_RDONLY);
    if (fd < 0)
	return;
    got = read(fd, cmd, sizeof(cmd) - 1);
    close(fd);
    if (got <= 0)
	return;
    cmd[got] = '\0';		/* argv[0] ends at the first NUL */
    name = strrchr(cmd, '/');
    name = name ? name + 1 : cmd;
    if (*name == '-')		/* login shells: "-bash" */
	name++;
    {
	char title[256];
	snprintf(title, sizeof(title), "%s%s", name, title_suffix);
	if (strncmp(title, title_area, title_area_len - 1) == 0)
	    return;
	memset(title_area, 0, title_area_len);
	snprintf(title_area, title_area_len, "%s", title);
	/* What `ps` shows after the title: says what this process really
	 * is, so nobody mistakes it for the program it's named after. */
	{
	    size_t used = strlen(title_area) + 1;
	    if (used + sizeof(TITLE_MARKER) <= title_area_len)
		memcpy(title_area + used, TITLE_MARKER, sizeof(TITLE_MARKER));
	}
    }
#else
    (void) pty;
#endif
}

/*
 * PATCH(fork, shell shim): options can also come from the environment.
 * The VS Code extension starts luit through a link named like the inner
 * shell (e.g. ".../bash"), so VS Code treats it as that shell: it injects
 * shell integration exactly as for its own terminals, which means it
 * replaces the arguments. luit's own options, the shell and its leading
 * arguments therefore come from TERMINAL_ANY_ENCODING_ARGS (one per line),
 * and the command line's arguments (VS Code's injected ones, or the
 * profile's) are appended after them, i.e. passed on to the shell. The
 * variable is removed so the shell doesn't inherit it.
 */
#define ARGS_ENV "TERMINAL_ANY_ENCODING_ARGS"

/*
 * PATCH(fork, copies): the extension keeps one copy of luit per version, in
 * a directory of its own, and removes copies no terminal has started from
 * for a long time. Started by the extension (through the shell-named link
 * or directly), luit marks its directory as used, so a terminal VS Code
 * keeps restoring keeps its copy.
 */
static void
markCopyUsed(const char *argv0)
{
    char *real;
    char *slash;

    if (argv0 == NULL || strchr(argv0, '/') == NULL)
	return;
    real = realpath(argv0, NULL);
    if (real == NULL)
	return;
    slash = strrchr(real, '/');
    if (slash != NULL && slash != real) {
	*slash = '\0';
	(void) utimes(real, NULL);
    }
    free(real);
}

static void
expandArgsFromEnv(int *argcp, char ***argvp)
{
    const char *value = getenv(ARGS_ENV);
    char *copy;
    char *p;
    char **args;
    int count = 0;
    int n = 0;
    int k;

    if (value == NULL)
	return;
    copy = strmalloc(value);
    unsetenv(ARGS_ENV);
    if (copy == NULL)
	return;
    for (p = copy; *p; p++)
	if (*p == '\n')
	    count++;
    count++;
    args = (char **) calloc((size_t) (count + *argcp + 1), sizeof(char *));
    if (args == NULL)
	return;
    args[n++] = (*argvp)[0];
    for (p = copy; p != NULL;) {
	char *next = strchr(p, '\n');
	if (next != NULL)
	    *next++ = '\0';
	if (*p != '\0')
	    args[n++] = p;
	p = next;
    }
    for (k = 1; k < *argcp; k++)
	args[n++] = (*argvp)[k];
    args[n] = NULL;
    markCopyUsed((*argvp)[0]);
    *argcp = n;
    *argvp = args;
}


/*
 * PATCH(fork, task command line): with -encode-last-arg, converts the last
 * argument -- the command line of a VS Code task, which VS Code passes in
 * UTF-8 -- into the encoding the way typed input is converted (copyIn(),
 * on the input state nothing has been typed into yet), so the shell gets
 * what it would have if the line had been typed in this terminal. Earlier
 * arguments (the profile's, VS Code's shell-integration script paths) name
 * files and stay as they are. If the line has a character the encoding
 * can't represent, nothing runs: as with rejected input, a substitute could
 * change the command (`rm <emoji>*` becoming `rm ?*`).
 */
static void
encodeLastArg(int argc, char **argv)
{
    unsigned char *arg;
    size_t len, done;
    FILE *tmp;
    int fd;
    off_t size;
    char *encoded;

    if (argc < 2)		/* the program alone, no argument */
	return;
    arg = (unsigned char *) argv[argc - 1];
    len = strlen((char *) arg);
    tmp = tmpfile();
    if (tmp == NULL) {
	perror("Couldn't convert the command line");
	ExitFailure();
    }
    fd = fileno(tmp);
    for (done = 0; done < len;) {
	size_t n = len - done < BUFFER_SIZE ? len - done : BUFFER_SIZE;
	if (copyIn(inputState, fd, arg + done, (int) n, 0)) {
	    Message("luit: the command line wasn't run: %s can't represent"
		    " U+%04X\n", locale_name, input_unencodable_char);
	    ExitFailure();
	}
	done += n;
    }
    size = lseek(fd, 0, SEEK_END);
    encoded = (size >= 0) ? malloc((size_t) size + 1) : NULL;
    if (encoded == NULL
	|| pread(fd, encoded, (size_t) size, 0) != (ssize_t) size) {
	perror("Couldn't convert the command line");
	ExitFailure();
    }
    encoded[size] = '\0';
    fclose(tmp);
    argv[argc - 1] = encoded;
}

int
main(int argc, char **argv)
{
    int rc;
    int i;
    char *l;

    claimTitleArea(&argc, &argv);
    expandArgsFromEnv(&argc, &argv);

#ifdef HAVE_PUTENV
    if ((l = strmalloc("NCURSES_NO_UTF8_ACS=1")) != NULL)
	putenv(l);
#endif

    l = setlocale(LC_ALL, "");
    if (!l)
	Warning("couldn't set locale.\n");
    TRACE(("setlocale ->%s\n", NonNull(l)));

    inputState = allocIso2022();
    if (!inputState)
	FatalError("Couldn't create input state\n");

    outputState = allocIso2022();
    if (!outputState)
	FatalError("Couldn't create output state\n");

    if (l) {
	locale_name = setlocale(LC_CTYPE, NULL);
    } else {
	locale_name = getenv("LC_ALL");
	if (locale_name == NULL) {
	    locale_name = getenv("LC_CTYPE");
	    if (locale_name == NULL) {
		locale_name = getenv("LANG");
	    }
	}
    }

    if (locale_name == NULL) {
	Message("Couldn't get locale name -- using C\n");
	locale_name = "C";
    }

    i = parseOptions(argc, argv);
    if (i < 0)
	FatalError("Couldn't parse options\n");

    rc = initIso2022(locale_name, NULL, outputState);
    if (rc < 0)
	FatalError("Couldn't init output state\n");

    rc = mergeIso2022(inputState, outputState);
    if (verbose) {
	reportIso2022("Input", inputState);
    }
    if (rc < 0)
	FatalError("Couldn't init input state\n");

    if (testonly) {
	if (testonly > 1) {
	    rc += warnings;
	}
    } else {
	if (converter)
	    rc = convert(STDIN_FILENO, STDOUT_FILENO);
	else {
	    if (encode_last_arg)
		encodeLastArg(argc - i, argv + i);
	    rc = condom(argc - i, argv + i);
	}
    }

#ifdef NO_LEAKS
    ExitProgram(rc);
#endif
    return rc;
}

static int
convert(int ifd, int ofd)
{
    int rc, i;
    unsigned char buf[BUFFER_SIZE];

    rc = droppriv();
    if (rc < 0) {
	perror("Couldn't drop privileges");
	ExitFailure();
    }

    while (1) {
	i = (int) read(ifd, buf, (size_t) BUFFER_SIZE);
	if (i <= 0) {
	    if (i < 0) {
		perror("Read error");
		ExitFailure();
	    }
	    break;
	}
	copyOut(outputState, ofd, buf, (unsigned) i);
    }
    return 0;
}

#ifdef SIGWINCH
static void
sigwinchHandler(int sig GCC_UNUSED)
{
    sigwinch_queued = 1;
}
#endif

static void
sigchldHandler(int sig GCC_UNUSED)
{
    sigchld_queued = 1;
}

static int
setup_io(int sfd, int pty)
{
    int rc;
    int val;

    TRACE(("setup_io pty %d (isatty:%d)\n", pty, isatty(pty)));
#ifdef SIGWINCH
    installHandler(SIGWINCH, sigwinchHandler);
#endif
    installHandler(SIGCHLD, sigchldHandler);

    rc = copyTermios(sfd, pty);
    if (rc < 0)
	FatalError("Couldn't copy terminal settings\n");

    rc = setRawTermios(sfd);
    if (rc < 0)
	FatalError("Couldn't set terminal to raw\n");

    val = fcntl(sfd, F_GETFL, 0);
    if (val >= 0) {
	(void) fcntl(sfd, F_SETFL, val | O_NONBLOCK);
    }
    val = fcntl(pty, F_GETFL, 0);
    if (val >= 0) {
	(void) fcntl(pty, F_SETFL, val | O_NONBLOCK);
    }

    setWindowSize(sfd, pty);

    return rc;
}

static void
cleanup_io(int sfd, int pty)
{
    int val;

#ifdef SIGWINCH
    installHandler(SIGWINCH, SIG_DFL);
#endif
    installHandler(SIGCHLD, SIG_DFL);

    val = fcntl(sfd, F_GETFL, 0);
    if (val >= 0) {
	(void) fcntl(sfd, F_SETFL, val & ~O_NONBLOCK);
    }
    val = fcntl(pty, F_GETFL, 0);
    if (val >= 0) {
	(void) fcntl(pty, F_SETFL, val & ~O_NONBLOCK);
    }
}

static void
close_waitpipe(int which)
{
    close(p2c_waitpipe[which]);
    close(c2p_waitpipe[!which]);
}

static void
write_waitpipe(int fds[2])
{
    IGNORE_RC(write(fds[1], "1", (size_t) 1));
}

static void
read_waitpipe(int fds[2])
{
    char tmp[10];
    IGNORE_RC(read(fds[0], tmp, (size_t) 1));
}

/* Upstream tells the parent luit, which is waiting on this child, that
 * the child failed. PATCH(fork, inverted tree): with the inverted tree there
 * is no such parent: the parent is whatever started luit (VS Code), which
 * must not get a SIGABRT. */
static void
abortParent(void)
{
    if (terminal_pid == 0)
	kill(getppid(), SIGABRT);
}

static void
child(int sfd, char *line, char *path, char *const argv[])
{
    int tty;
    int pgrp;

    TRACE(("child %s\n", NonNull(path)));
    if (path == NULL)
	ExitFailure();

    /* PATCH(fork, inverted tree): in condomInverted() this is the process
     * VS Code started, already a session leader that just gave up its
     * controlling terminal, where setsid() would fail. */
    if (getsid(0) != getpid()) {
	pgrp = setsid();
	if (pgrp < 0) {
	    abortParent();
	    ExitFailure();
	}
    }

    tty = openTty(line);
    if (tty < 0) {
	abortParent();
	ExitFailure();
    }

    if (pipe_option) {
	write_waitpipe(c2p_waitpipe);
    }

    setWindowSize(sfd, tty);
    close(STDIN_FILENO);
    if (tty != STDIN_FILENO)
	dup2(tty, STDIN_FILENO);

    close(STDOUT_FILENO);
    if (tty != STDOUT_FILENO)
	dup2(tty, STDOUT_FILENO);

    close(STDERR_FILENO);
    if (tty != STDERR_FILENO)
	dup2(tty, STDERR_FILENO);

    if (tty > 2)
	close(tty);

    if (pipe_option) {
	read_waitpipe(p2c_waitpipe);
	close_waitpipe(0);
    }

    execvp(path, argv);
    perror(path);
    ExitFailure();
}

/*
 * PATCH(fork, input rejection): tells the VS Code extension that input was
 * rejected, so it can say why: the bell alone is silent with VS Code's
 * default settings. -notify names a directory where each VS Code window's
 * extension listens on its own Unix socket ("*.sock"); every one gets the
 * line "unencodable <encoding> <pid> <hex code point>", and the window that owns this
 * terminal (its process id is luit's) shows the notification. Broadcasting
 * keeps this working for terminals revived after a window reload, whose
 * extension host (and socket) is new. Best effort and non-blocking:
 * sockets that don't accept right away are skipped.
 */
static void
notifyRejected(void)
{
    char line[256];
    int len;
    DIR *dir;
    struct dirent *entry;
    int flags = 0;

    if (notify_path == NULL)
	return;
    len = snprintf(line, sizeof(line), "unencodable %s %ld %X\n",
		   locale_name ? locale_name : "",
		   terminal_pid ? terminal_pid : (long) getpid(),
		   input_unencodable_char);
    if (len <= 0 || len >= (int) sizeof(line))
	return;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    dir = opendir(notify_path);
    if (dir == NULL)
	return;
    while ((entry = readdir(dir)) != NULL) {
	struct sockaddr_un addr;
	size_t nlen = strlen(entry->d_name);
	int fd;

	if (nlen < 6 || strcmp(entry->d_name + nlen - 5, ".sock") != 0)
	    continue;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s",
		     notify_path, entry->d_name) >= (int) sizeof(addr.sun_path))
	    continue;
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
	    continue;
	(void) fcntl(fd, F_SETFD, FD_CLOEXEC);
	(void) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
	{
	    int one = 1;
	    (void) setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	}
#endif
	if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0)
	    IGNORE_RC(send(fd, line, (size_t) len, flags));
	close(fd);
    }
    closedir(dir);
}

/* PATCH(fork, input rejection): how long input keeps being dropped after a
 * rejection, measured from the last dropped read. Long enough to cover a
 * paste arriving in several reads, short enough not to eat the next
 * keystroke typed by hand. */
#define REJECT_QUIET_MILLIS 50.0

static double
monotonicMillis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static void
parent(int sfd, int pty)
{
    unsigned char buf[BUFFER_SIZE];
    int i;
    int rc;
    double reject_until = 0.0;

    if (pipe_option) {
	read_waitpipe(c2p_waitpipe);
    }

    if (verbose) {
	reportIso2022("Output", outputState);
    }
    setup_io(sfd, pty);

    if (pipe_option) {
	write_waitpipe(p2c_waitpipe);
	close_waitpipe(1);
    }

    for (;;) {
	/* PATCH(fork, title): VS Code re-reads the tab title every 200 ms,
	 * output or not, so keep up with the inner foreground program at
	 * the same pace (e.g. a silent `sleep`). */
	rc = waitForInput(sfd, pty, title_suffix != NULL ? 200 : -1);
	updateTitle(pty);

	if (sigwinch_queued) {
	    sigwinch_queued = 0;
	    setWindowSize(sfd, pty);
	}

	if (sigchld_queued && exitOnChild)
	    break;

	if (rc > 0) {
	    if (rc & IO_Closed) {
		break;
	    }
	    if (rc & IO_CanWrite) {
		i = (int) read(pty, buf, (size_t) BUFFER_SIZE);
		if ((i == 0) || ((i < 0) && (errno != EAGAIN)))
		    break;
		if (i > 0)
		    copyOut(outputState, sfd, buf, (unsigned) i);
	    }
	    if (rc & IO_CanRead) {
		i = (int) read(sfd, buf, (size_t) BUFFER_SIZE);
		if ((i == 0) || ((i < 0) && (errno != EAGAIN)))
		    break;
		if (i > 0) {
		    /* PATCH(fork, input rejection): input with an unencodable
		     * character is rejected as a whole (see copyIn) and the
		     * user gets a bell. Input arriving right after that is
		     * dropped too: a paste comes in several reads, and
		     * forwarding only its tail could run a different command
		     * than the one pasted. */
		    double now = monotonicMillis();
		    int discard = now < reject_until;
		    if (copyIn(inputState, pty, buf, i, discard) && !discard) {
			IGNORE_RC(write(sfd, "\a", (size_t) 1));
			notifyRejected();
		    }
		    if (discard || input_unencodable)
			reject_until = now + REJECT_QUIET_MILLIS;
		}
	    }
	}
    }

    restoreTermios(sfd);
    cleanup_io(sfd, pty);
}

/*
 * PATCH(fork, exit status): exits with the child's status, so whatever runs
 * luit (VS Code's "terminal process exited with code N" alert) sees the
 * shell's exit code instead of always 0. A child killed by a signal maps to
 * 128 + signal, as shells report it. By the time parent() returns the pty
 * is closed, so the child has exited or got SIGHUP; one that ignores it
 * isn't waited for forever.
 */
static int
childExitCode(int pid)
{
    int status;
    int tries;

    for (tries = 0; tries < 200; tries++) {
	pid_t done = waitpid((pid_t) pid, &status, WNOHANG);
	if (done == (pid_t) pid) {
	    if (WIFEXITED(status))
		return WEXITSTATUS(status);
	    if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	    return EXIT_FAILURE;
	}
	if (done < 0)
	    return EXIT_FAILURE;
	usleep(10000);
    }
    return EXIT_FAILURE;
}

/*
 * PATCH(fork, inverted tree): runs the shell as the process that started
 * luit, with the converter as a detached helper, instead of luit being the
 * shell's parent. VS Code (like any terminal) looks at the process it
 * started and its children: with luit in between, the shell itself counted
 * as a running child (so closing an editor terminal always asked for
 * confirmation), and the exit code and working directory were luit's. Now
 * they're the shell's, exactly as in a regular terminal.
 *
 * Only for a session leader with the outer terminal as its controlling
 * terminal (how VS Code, and terminals in general, start a shell);
 * otherwise condom() keeps the classic layout. The steps:
 *   1. Fork the converter and detach it (double fork, so it's no child of
 *      the shell); it calls setsid() to leave our process group.
 *   2. We give up the outer terminal (TIOCNOTTY; ignoring the SIGHUP this
 *      sends our own group) and tell the converter, which takes it as its
 *      controlling terminal: it then gets SIGWINCH on resize and SIGHUP
 *      when the terminal closes, as luit always did.
 *   3. We take the inner pty as controlling terminal and exec the shell.
 * The converter exits when the inner pty closes, i.e. when the shell and
 * everything it started are gone.
 */
static int
canInvert(int sfd)
{
#if defined(TIOCNOTTY) && defined(TIOCSCTTY)
    return !pipe_option
	&& getsid(0) == getpid()
	&& isatty(sfd)
	&& tcgetsid(sfd) == getpid();
#else
    (void) sfd;
    return 0;
#endif
}

#if defined(TIOCNOTTY) && defined(TIOCSCTTY)
/*
 * Gives up the outer terminal, so that this process can later take the inner
 * pty as its controlling terminal. Done before anything is forked: if the
 * system doesn't allow it, nothing has changed yet and condom() keeps the
 * classic layout. On macOS it fails (ENOTTY, as started by a terminal).
 * TIOCNOTTY also sends SIGHUP to our own process group, i.e. only us.
 */
static int
releaseOuterTerminal(int sfd)
{
    int rc;
    void (*old) (int) = signal(SIGHUP, SIG_IGN);

    rc = ioctl(sfd, TIOCNOTTY, (char *) 0);
    if (rc < 0)
	VERBOSE(1, ("keeping the classic layout: TIOCNOTTY: %s\n",
		    strerror(errno)));
    signal(SIGHUP, old == SIG_ERR ? SIG_DFL : old);
    return rc == 0;
}

static int
condomInverted(int sfd, int pty, char *line, char *path, char **child_argv)
{
    pid_t helper;

    terminal_pid = (long) getpid();

    helper = fork();
    if (helper < 0) {
	perror("Couldn't fork");
	ExitFailure();
    }
    if (helper == 0) {
	pid_t converter = fork();
	if (converter != 0)
	    _exit(converter < 0 ? EXIT_FAILURE : EXIT_SUCCESS);
	/* the converter, now an orphan: takes the outer terminal, which
	 * nobody has now, to get its SIGWINCH and SIGHUP */
	(void) setsid();
	(void) ioctl(sfd, TIOCSCTTY, (char *) 0);
	closeParentTty();
	free(child_argv);
	free(path);
	free(line);
	parent(sfd, pty);
	return EXIT_SUCCESS;
    }

    /* the process the outer terminal started: becomes the shell */
    (void) waitpid(helper, NULL, 0);
    close(pty);
    child(sfd, line, path, child_argv);
    return EXIT_FAILURE;	/* child() doesn't return */
}
#endif

static int
condom(int argc, char **argv)
{
    int pty;
    int pid;
    char *line;
    char *path = NULL;
    char **child_argv = NULL;
    int rc;
    int sfd = STDIN_FILENO;

    rc = parseArgs(argc, argv, child_argv0,
		   &path, &child_argv);
    if (rc < 0)
	FatalError("Couldn't parse arguments\n");

    rc = allocatePty(&pty, &line);
    if (rc < 0) {
	perror("Couldn't allocate pty");
	ExitFailure();
    }

    rc = droppriv();
    if (rc < 0) {
	perror("Couldn't drop privileges");
	ExitFailure();
    }

    if (pipe_option) {
	IGNORE_RC(pipe(p2c_waitpipe));
	IGNORE_RC(pipe(c2p_waitpipe));
    }

#if defined(TIOCNOTTY) && defined(TIOCSCTTY)
    if (canInvert(sfd) && releaseOuterTerminal(sfd))
	return condomInverted(sfd, pty, line, path, child_argv);
#endif

    TRACE(("...forking to run %s(%s)\n", NonNull(path), NonNull(child_argv[0])));
    pid = fork();
    if (pid < 0) {
	perror("Couldn't fork");
	ExitFailure();
    }

    if (pid == 0) {
	close(pty);
	if (pipe_option) {
	    close_waitpipe(1);
	}
	child(sfd, line, path, child_argv);
    } else {
	if (pipe_option) {
	    close_waitpipe(0);
	}
	closeParentTty();
	free(child_argv);
	free(path);
	free(line);
	parent(sfd, pty);
	return childExitCode(pid);
    }

    return 0;
}

#ifdef NO_LEAKS
void
luit_leaks(void)
{
    destroyIso2022(inputState);
    destroyIso2022(outputState);
}
#endif
