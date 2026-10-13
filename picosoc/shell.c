// ========================================================
// shell.c : command console for PicoSoC firmware, in one file
//
// Usage: in firmware.c, just above main(), add
//     #include "shell.c"
// and call shell_run() from a menu entry. picofs.c, pico_vi.c,
// pico_snake.c and pico_tetris.c must be in the same directory; this
// file includes them.
//
// Sections:
//   1. shell core        line editor, parser, dispatcher
//   2. basic commands    help echo peek poke dump led delay reboot exit
//   3. flash commands    flash fldbg
//   4. file commands     ls save cat rm vi run format
//   5. command list      which command tables are part of the shell
//
// To add a command: write  static int shcmd_xxx(int argc, char **argv)
// and add one line to the table of its section. Return 1 to leave the
// shell, 0 otherwise.
//
// No libc, no libgcc, no mul/div (works with -nostdlib, rv32ic).
// ========================================================
#include "picofs.c"

// --------------------------------------------------------
// 1. SHELL CORE - interface for command sections
//
// A command section has a NULL-terminated table:
//
//     static int shcmd_hello(int argc, char **argv) { ...; return 0; }
//
//     const struct shell_cmd shell_xxx_cmds[] = {
//         { "hello", shcmd_hello, "hello                    say hello" },
//         { 0, 0, 0 }
//     };
//
// The table is then listed in section 5. A command returns 1 to
// leave the shell, 0 otherwise.
// --------------------------------------------------------

#define SHELL_LINE_MAX 64
#define SHELL_ARGS_MAX 8

struct shell_cmd {
	const char *name;
	int (*fn)(int argc, char **argv);
	const char *help;
};

// All command tables, NULL-terminated. Defined in section 5.
extern const struct shell_cmd *const shell_groups[];

void shell_run(void);                           // interactive loop until 'exit'
int  shell_exec(char *line);                    // run one line (modified in place)
void shell_help(void);                          // print every command's help text

// Helpers for commands.
int  shell_readline(char *buf, int max);        // with echo and backspace
int  shell_streq(const char *a, const char *b);
int  shell_parse_num(const char *s, uint32_t *out);   // decimal or 0x hex

// --------------------------------------------------------
// 1. SHELL CORE - line editor, parser and dispatcher
//
// Contains no commands of its own.
// --------------------------------------------------------

int shell_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

// "0x1f" / "0X1F" -> hex, otherwise decimal. Returns 1 on success.
int shell_parse_num(const char *s, uint32_t *out)
{
	uint32_t v = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		if (!*s) return 0;
		for (; *s; s++) {
			uint32_t d;
			if (*s >= '0' && *s <= '9') d = *s - '0';
			else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
			else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
			else return 0;
			v = (v << 4) | d;
		}
	} else {
		if (!*s) return 0;
		for (; *s; s++) {
			if (*s < '0' || *s > '9') return 0;
			v = (v << 3) + (v << 1) + (uint32_t)(*s - '0');   // v * 10
		}
	}
	*out = v;
	return 1;
}

// Read one line with echo and backspace. Returns the length.
int shell_readline(char *buf, int max)
{
	int len = 0;

	for (;;) {
		char c = io_getc();

		if (c == '\r' || c == '\n') {
			putchar('\n');
			break;
		}
		if (c == 0x08 || c == 0x7f) {           // backspace / delete
			if (len > 0) {
				len--;
				io_puts("\b \b");
			}
			continue;
		}
		if (c >= ' ' && c < 0x7f && len < max - 1) {
			buf[len++] = c;
			putchar(c);
		}
	}
	buf[len] = 0;
	return len;
}

// Split the line in place at spaces. Returns argc.
static int shell_split(char *line, char **argv, int max)
{
	int argc = 0;

	while (*line) {
		while (*line == ' ')
			*(line++) = 0;
		if (!*line)
			break;
		if (argc == max)
			break;
		argv[argc++] = line;
		while (*line && *line != ' ')
			line++;
	}
	return argc;
}

void shell_help(void)
{
	for (int g = 0; shell_groups[g]; g++) {
		for (const struct shell_cmd *c = shell_groups[g]; c->name; c++) {
			io_puts("  ");
			io_puts(c->help);
			putchar('\n');
		}
	}
	io_puts("numbers: decimal, or hex with 0x prefix\n");
}

// Run one command line (modified in place). Returns 1 for 'exit'.
int shell_exec(char *line)
{
	char *argv[SHELL_ARGS_MAX];
	int argc = shell_split(line, argv, SHELL_ARGS_MAX);

	if (argc == 0)
		return 0;

	for (int g = 0; shell_groups[g]; g++)
		for (const struct shell_cmd *c = shell_groups[g]; c->name; c++)
			if (shell_streq(argv[0], c->name))
				return c->fn(argc, argv);

	io_puts("unknown command: ");
	io_puts(argv[0]);
	putchar('\n');
	return 0;
}

void shell_run(void)
{
	char line[SHELL_LINE_MAX];

	io_puts("\npico shell - type 'help'\n");

	for (;;) {
		io_puts("pico> ");
		shell_readline(line, SHELL_LINE_MAX);
		if (shell_exec(line))
			break;
	}
}

// --------------------------------------------------------
// 2. BASIC COMMANDS : help, echo, peek, poke, dump, led, delay, reboot, exit
// --------------------------------------------------------

static int shcmd_help(int argc, char **argv)
{
	shell_help();
	return 0;
}

static int shcmd_echo(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) 
	{
		if ( i > 1 ) 
			putchar(' ');
		
		io_puts(argv[i]);
	}

	putchar('\n');
	return 0;
}

static int shcmd_peek(int argc, char **argv)
{
	uint32_t addr;

	if (argc != 2 || !shell_parse_num(argv[1], &addr)) {
		io_puts("usage: peek <addr>\n");
		return 0;
	}
	addr &= ~3u;                                // word aligned access only
	io_hex(addr, 8);
	io_puts(": ");
	io_hex(PORT_MEM32(addr), 8);
	putchar('\n');
	return 0;
}

static int shcmd_poke(int argc, char **argv)
{
	uint32_t addr, val;

	if (argc != 3 || !shell_parse_num(argv[1], &addr) || !shell_parse_num(argv[2], &val)) {
		io_puts("usage: poke <addr> <value>\n");
		return 0;
	}
	addr &= ~3u;
	PORT_MEM32(addr) = val;
	return 0;
}

static int shcmd_dump(int argc, char **argv)
{
	uint32_t addr, words = 16;

	if (argc < 2 || argc > 3 || !shell_parse_num(argv[1], &addr) ||
			(argc == 3 && !shell_parse_num(argv[2], &words))) {
		io_puts("usage: dump <addr> [words]\n");
		return 0;
	}
	if (words > 256) words = 256;
	addr &= ~3u;

	for (uint32_t i = 0; i < words; i++) {
		if ((i & 3) == 0) {
			io_hex(addr, 8);
			putchar(':');
		}
		putchar(' ');
		io_hex(PORT_MEM32(addr), 8);
		addr += 4;
		if ((i & 3) == 3 || i == words - 1)
			putchar('\n');
	}
	return 0;
}

static int shcmd_led(int argc, char **argv)
{
	uint32_t val;

	if (argc != 2 || !shell_parse_num(argv[1], &val)) {
		io_puts("usage: led <value>\n");
		return 0;
	}
	reg_leds = val;
	return 0;
}

static int shcmd_delay(int argc, char **argv)
{
	uint32_t ms;

	if (argc != 2 || !shell_parse_num(argv[1], &ms) || ms > 60000) {
		io_puts("usage: delay <ms>   (0-60000)\n");
		return 0;
	}
	io_delay_ms(ms);
	return 0;
}

// Software restart. The CPU registers, RAM variables and stack are set up
// again by start.s; hardware registers (LEDs, UART divider, SPI mode) keep
// their values until the firmware rewrites them. Does not return.
static int shcmd_reboot(int argc, char **argv)
{
	io_puts("rebooting...\n");
	PORT_REBOOT();
	return 1;
}

static int shcmd_exit(int argc, char **argv)
{
	return 1;
}

const struct shell_cmd shell_base_cmds[] = {
	{ "help", shcmd_help, "help                     list commands" },
	{ "echo", shcmd_echo, "echo <text...>           print the arguments" },
	{ "peek", shcmd_peek, "peek <addr>              read a 32-bit word" },
	{ "poke", shcmd_poke, "poke <addr> <value>      write a 32-bit word" },
	{ "dump", shcmd_dump, "dump <addr> [words]      hex dump (default 16 words)" },
	{ "led",  shcmd_led,  "led <value>              write the LED register" },
	{ "delay", shcmd_delay, "delay <ms>               wait (0-60000 ms)" },
	{ "reboot", shcmd_reboot, "reboot                   restart the firmware" },
	{ "exit", shcmd_exit, "exit                     return to the menu" },
	{ 0, 0, 0 }
};

// --------------------------------------------------------
// 3. FLASH COMMANDS : flash, fldbg
//
// Independent of any file system: uses only the flash_ functions.
// --------------------------------------------------------

// Read the flash ID and status through the RAM routine. Changes nothing:
// use it to check the RAM round trip before trying a write.
static int shcmd_flash(int argc, char **argv)
{
	uint8_t id[3];
	uint8_t status;
	int err = flash_info(id, &status);

	if (err) {
		io_puts(flash_strerror(err));
		putchar('\n');
		return 0;
	}
	io_puts("id ");
	io_hex(id[0], 2); putchar(' ');
	io_hex(id[1], 2); putchar(' ');
	io_hex(id[2], 2);
	io_puts("  status ");
	io_hex(status, 2);
	putchar('\n');
	return 0;
}

static int shcmd_fldbg(int argc, char **argv)
{
	flash_debug = !flash_debug;
	io_puts(flash_debug ? "flash trace on\n" : "flash trace off\n");
	return 0;
}

const struct shell_cmd shell_flash_cmds[] = {
	{ "flash", shcmd_flash, "flash                    read flash id and status (no write)" },
	{ "fldbg", shcmd_fldbg, "fldbg                    flash trace on/off" },
	{ 0, 0, 0 }
};

// --------------------------------------------------------
// 4. FILE COMMANDS : ls, save, cat, rm, run, format
//
// The only section that connects the shell to PFS. It uses nothing but
// the pfs_ functions, so another file system with the same interface can
// take its place.
// --------------------------------------------------------

static int fs_running;              // set while 'run' executes a file

static void fs_report(int err)
{
	io_puts(pfs_strerror(err));
	putchar('\n');
}

static int shcmd_ls(int argc, char **argv)
{
	char name[PFS_NAME_MAX];
	int count = 0;

	for (int slot = 0; slot < PFS_SLOTS; slot++) {
		if (!pfs_used(slot))
			continue;
		pfs_name(slot, name);
		io_puts("  ");
		for (int i = 0, end = 0; i < PFS_NAME_MAX; i++) {
			if (!name[i]) end = 1;
			putchar(end ? ' ' : name[i]);
		}
		putchar(' ');
		io_dec(pfs_size(slot));
		io_puts(" bytes\n");
		count++;
	}
	io_dec(count);
	io_puts(" of ");
	io_dec(PFS_SLOTS);
	io_puts(" slots used\n");
	return 0;
}

static int shcmd_save(int argc, char **argv)
{
	char line[SHELL_LINE_MAX];
	uint8_t *buf = pfs_buffer();
	uint32_t len = 0;
	int err;

	if (argc != 2 || !pfs_name_valid(argv[1])) {
		io_puts("usage: save <name>   (name: 1-7 characters)\n");
		return 0;
	}

	io_puts("enter text, finish with a line containing only '.'\n");
	for (;;) {
		int n = shell_readline(line, SHELL_LINE_MAX);

		if (n == 1 && line[0] == '.')
			break;
		if (len + n + 1 > PFS_DATA_MAX) {
			io_puts("file too big, not saved\n");
			return 0;
		}
		for (int i = 0; i < n; i++)
			buf[len++] = line[i];
		buf[len++] = '\n';
	}

	err = pfs_write(argv[1], len);
	if (err) {
		fs_report(err);
		return 0;
	}
	io_puts("saved ");
	io_dec(len);
	io_puts(" bytes\n");
	return 0;
}

static int shcmd_cat(int argc, char **argv)
{
	int slot;
	uint32_t len;

	if (argc != 2) {
		io_puts("usage: cat <name>\n");
		return 0;
	}
	slot = pfs_find(argv[1]);
	if (slot < 0) {
		fs_report(PFS_ERR_NOTFOUND);
		return 0;
	}
	len = pfs_size(slot);
	for (uint32_t i = 0; i < len; i++)
		putchar(pfs_byte(slot, i));
	return 0;
}

static int shcmd_rm(int argc, char **argv)
{
	int err;

	if (argc != 2) {
		io_puts("usage: rm <name>\n");
		return 0;
	}
	err = pfs_remove(argv[1]);
	if (err)
		fs_report(err);
	return 0;
}

// Erase every slot after asking for confirmation.
static int shcmd_format(int argc, char **argv)
{
	char line[SHELL_LINE_MAX];
	int err;

	io_puts("erase all files? type 'yes': ");
	shell_readline(line, SHELL_LINE_MAX);
	if (!shell_streq(line, "yes")) {
		io_puts("cancelled\n");
		return 0;
	}
	err = pfs_format();
	if (err)
		fs_report(err);
	else
		io_puts("formatted\n");
	return 0;
}

// ---- run: execute a file line by line as shell commands ----
//
// Settings may be placed at the top of the file, one per line, before the
// first command:
//     loop=on     repeat the file until a key is pressed
//     echo=on     print each command before it runs (default: off)
// Nothing is printed for the commands themselves unless echo=on, and the
// flash trace is switched off while the file runs.

// Copy the line starting at 'pos' into 'line' and return the position
// after it. Lines longer than the buffer are cut.
static uint32_t fs_read_line(int slot, uint32_t pos, uint32_t len, char *line)
{
	int n = 0;

	while (pos < len) {
		char c = (char)pfs_byte(slot, pos++);

		if (c == '\n')
			break;
		if (n < SHELL_LINE_MAX - 1)
			line[n++] = c;
	}
	line[n] = 0;
	return pos;
}

// If 'line' is "<name>=on" or "<name>=off", store 1 or 0 in *value and
// return 1. Return 0 if the line is not this setting, -1 if the value is
// neither on nor off.
static int fs_setting(const char *line, const char *name, int *value)
{
	while (*name && *line == *name) {
		line++;
		name++;
	}
	if (*name || *line != '=')
		return 0;
	line++;
	if (shell_streq(line, "on")||shell_streq(line, "ON") ) {
		*value = 1;
		return 1;
	}
	if (shell_streq(line, "off")||shell_streq(line, "OFF")) {
		*value = 0;
		return 1;
	}
	return -1;
}

static int shcmd_run(int argc, char **argv)
{
	char line[SHELL_LINE_MAX];
	int slot, loop = 0, echo = 0, stop = 0, saved_debug;
	uint32_t len, pos = 0, body;

	if (argc != 2) {
		io_puts("usage: run <name>\n");
		return 0;
	}
	slot = pfs_find(argv[1]);
	if (slot < 0) {
		fs_report(PFS_ERR_NOTFOUND);
		return 0;
	}
	if (fs_running) {
		io_puts("run: cannot be nested\n");
		return 0;
	}
	len = pfs_size(slot);

	// Settings at the top of the file.
	for (;;) {
		uint32_t next = fs_read_line(slot, pos, len, line);
		int r;

		if (line[0]) {
			r = fs_setting(line, "loop", &loop);
			if (!r)
				r = fs_setting(line, "echo", &echo);
			if (r < 0) {
				io_puts("run: use on or off: ");
				io_puts(line);
				putchar('\n');
				return 0;
			}
			if (!r)
				break;              // first command: settings end here
		}
		pos = next;
		if (pos >= len)
			break;
	}
	body = pos;

	fs_running = 1;
	saved_debug = flash_debug;
	flash_debug = 0;
	if (loop)
		io_puts("looping - press any key to stop\n");

	do {
		for (pos = body; pos < len && !stop; ) {
			pos = fs_read_line(slot, pos, len, line);
			if (!line[0])
				continue;
			if (echo) {
				io_puts("> ");
				io_puts(line);
				putchar('\n');
			}
			if (shell_exec(line))
				stop = 1;           // 'exit' inside a file stops the file
			else if (loop && PORT_KEY_PRESSED())
				stop = 2;
		}
		if (loop && !stop && PORT_KEY_PRESSED())
			stop = 2;
	} while (loop && !stop);

	if (stop == 2)
		io_puts("stopped\n");
	flash_debug = saved_debug;
	fs_running = 0;
	return 0;
}

// The editor: defines shcmd_vi().
#include "pico_vi.c"

const struct shell_cmd shell_fs_cmds[] = {
	{ "ls",     shcmd_ls,     "ls                       list files in flash" },
	{ "save",   shcmd_save,   "save <name>              type a text file into flash" },
	{ "cat",    shcmd_cat,    "cat <name>               print a file" },
	{ "rm",     shcmd_rm,     "rm <name>                delete a file" },
	{ "vi",     shcmd_vi,     "vi <name>                edit a file (pico_vi.c)" },
	{ "run",    shcmd_run,    "run <name>               execute a file as commands" },
	{ "format", shcmd_format, "format                   erase all files" },
	{ 0, 0, 0 }
};

// The games: define shell_snake_cmds[] and shell_tetris_cmds[].
#include "pico_snake.c"
#include "pico_tetris.c"

// --------------------------------------------------------
// 5. COMMAND LIST : which command tables are part of the shell
//
// To add a table: add it to shell_groups[]. To leave one out: remove its line.
// --------------------------------------------------------

const struct shell_cmd *const shell_groups[] = {
	shell_base_cmds,
	shell_flash_cmds,
	shell_fs_cmds,
	shell_snake_cmds,
	shell_tetris_cmds,
	0
};
