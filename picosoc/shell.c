// --------------------------------------------------------
// shell.c : tiny command console + slot file system for PicoSoC firmware
//
// desc: AI generated code by claude code 2026. 10.10
// Usage: in firmware.c, below putchar() and the reg_uart_data /
// reg_leds definitions, add
//     #include "shell.c"
// and call shell_run() from a menu entry.
//
// Needs no libc, no libgcc, no mul/div (works with -nostdlib, rv32ic).
// Uses putchar(), reg_uart_data, reg_leds and flashio() from firmware.c.
//
// To add a command: write   static int shcmd_xxx(int argc, char **argv)
// and add one line to shell_cmds[] below. Return 1 to leave the shell.
//
// FILE SYSTEM
//   FS_SLOTS fixed slots, one 4 KB flash sector each, starting at flash
//   offset FS_BASE. A slot is:
//     bytes 0..3   magic "PFS1"   (anything else = free slot)
//     bytes 4..7   data length
//     bytes 8..15  file name, NUL terminated (max 7 characters)
//     bytes 16..   data (max 4080 bytes)
//   Reading uses the memory-mapped flash. Writing goes through flashio()
//   (sector erase 0x20, page program 0x02), which runs from RAM.
//
//   !! FS_BASE must not overlap the bitstream (flash offset 0) or the
//   !! firmware (flash offset 1 MB on iCEBreaker).
// --------------------------------------------------------

#include <stdint.h>

#define SHELL_LINE_MAX 64
#define SHELL_ARGS_MAX 8

#define FS_BASE      0x00200000u      // flash offset 2 MB
#define FS_SLOTS     8
#define FS_SECTOR    4096
#define FS_HDR       16
#define FS_NAME_MAX  8                // including the terminating NUL
#define FS_DATA_MAX  (FS_SECTOR - FS_HDR)

#ifndef SHELL_MEM
#define SHELL_MEM(addr) (*(volatile uint32_t*)(addr))
#endif

#ifndef FS_FLASH_BYTE
#define FS_FLASH_BYTE(addr) (*(volatile uint8_t*)(addr))
#endif

void flashio(uint8_t *data, int len, uint8_t wrencmd);

static int shell_exec(char *line);

// ---------------- I/O helpers ----------------

static char shell_getc(void)
{
	int32_t c;
	do {
		c = reg_uart_data;      // -1 while no byte has been received
	} while (c == -1);
	return (char)c;
}

static void shell_puts(const char *s)
{
	while (*s)
		putchar(*(s++));
}

static void shell_hex(uint32_t v, int digits)
{
	for (int shift = (digits - 1) << 2; shift >= 0; shift -= 4)
		putchar("0123456789abcdef"[(v >> shift) & 15]);
}

static void shell_dec(uint32_t v)
{
	static const uint32_t pow10[10] = {
		1000000000u, 100000000u, 10000000u, 1000000u, 100000u,
		10000u, 1000u, 100u, 10u, 1u
	};
	int started = 0;

	for (int i = 0; i < 10; i++) {
		char d = '0';
		while (v >= pow10[i]) {
			v -= pow10[i];
			d++;
		}
		if (d != '0' || started || i == 9) {
			putchar(d);
			started = 1;
		}
	}
}

static int shell_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

// "0x1f" / "0X1F" -> hex, otherwise decimal. Returns 1 on success.
static int shell_parse_num(const char *s, uint32_t *out)
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
static int shell_readline(char *buf, int max)
{
	int len = 0;

	for (;;) {
		char c = shell_getc();

		if (c == '\r' || c == '\n') {
			putchar('\n');
			break;
		}
		if (c == 0x08 || c == 0x7f) {           // backspace / delete
			if (len > 0) {
				len--;
				shell_puts("\b \b");
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

// ---------------- flash file system ----------------

static uint8_t fs_buf[FS_SECTOR];    // RAM image of one slot (header + data)
static int fs_running;               // set while 'run' executes a file

static uint32_t fs_slot_addr(int slot)
{
	return FS_BASE + ((uint32_t)slot << 12);
}

static uint32_t fs_rd32(uint32_t addr)
{
	return (uint32_t)FS_FLASH_BYTE(addr) |
		((uint32_t)FS_FLASH_BYTE(addr + 1) << 8) |
		((uint32_t)FS_FLASH_BYTE(addr + 2) << 16) |
		((uint32_t)FS_FLASH_BYTE(addr + 3) << 24);
}

static int fs_slot_used(int slot)
{
	uint32_t a = fs_slot_addr(slot);

	return FS_FLASH_BYTE(a) == 'P' && FS_FLASH_BYTE(a + 1) == 'F' &&
		FS_FLASH_BYTE(a + 2) == 'S' && FS_FLASH_BYTE(a + 3) == '1' &&
		fs_rd32(a + 4) <= FS_DATA_MAX;
}

static int fs_name_ok(const char *name)
{
	int n = 0;

	while (name[n])
		n++;
	return n > 0 && n < FS_NAME_MAX;
}

// Returns the slot holding 'name', or -1.
static int fs_find(const char *name)
{
	for (int slot = 0; slot < FS_SLOTS; slot++) {
		uint32_t a = fs_slot_addr(slot) + 8;
		int i = 0;

		if (!fs_slot_used(slot))
			continue;
		while (i < FS_NAME_MAX && name[i] && name[i] == (char)FS_FLASH_BYTE(a + i))
			i++;
		if (i < FS_NAME_MAX && !name[i] && !FS_FLASH_BYTE(a + i))
			return slot;
	}
	return -1;
}

static int fs_find_free(void)
{
	for (int slot = 0; slot < FS_SLOTS; slot++)
		if (!fs_slot_used(slot))
			return slot;
	return -1;
}

// Wait until the flash has finished an erase or program operation.
static void fs_wait(void)
{
	uint8_t b[2];

	do {
		b[0] = 0x05;            // read status register 1
		b[1] = 0;
		flashio(b, 2, 0);
	} while (b[1] & 1);         // bit 0 = BUSY
}

static void fs_erase(uint32_t addr)
{
	uint8_t b[4];

	b[0] = 0x20;                // 4 KB sector erase
	b[1] = addr >> 16;
	b[2] = addr >> 8;
	b[3] = addr;
	flashio(b, 4, 0x06);        // 0x06 = write enable
	fs_wait();
}

// Program up to 256 bytes; must not cross a 256-byte page boundary.
static void fs_program(uint32_t addr, const uint8_t *src, int n)
{
	uint8_t b[4 + 256];

	b[0] = 0x02;                // page program
	b[1] = addr >> 16;
	b[2] = addr >> 8;
	b[3] = addr;
	for (int i = 0; i < n; i++)
		b[4 + i] = src[i];
	flashio(b, 4 + n, 0x06);
	fs_wait();
}

// Write fs_buf (data already at fs_buf + FS_HDR) to a slot. Returns 1 if verified.
static int fs_store(int slot, const char *name, uint32_t len)
{
	uint32_t base = fs_slot_addr(slot);
	uint32_t total = FS_HDR + len;
	int i;

	fs_buf[0] = 'P'; fs_buf[1] = 'F'; fs_buf[2] = 'S'; fs_buf[3] = '1';
	fs_buf[4] = len; fs_buf[5] = len >> 8; fs_buf[6] = len >> 16; fs_buf[7] = len >> 24;
	for (i = 0; i < FS_NAME_MAX && name[i]; i++)
		fs_buf[8 + i] = name[i];
	for (; i < FS_NAME_MAX; i++)
		fs_buf[8 + i] = 0;

	fs_erase(base);
	for (uint32_t off = 0; off < total; off += 256) {
		uint32_t n = total - off;
		if (n > 256) n = 256;
		fs_program(base + off, fs_buf + off, n);
	}

	for (uint32_t off = 0; off < total; off++)
		if (FS_FLASH_BYTE(base + off) != fs_buf[off])
			return 0;
	return 1;
}

static int shcmd_ls(int argc, char **argv)
{
	int count = 0;

	for (int slot = 0; slot < FS_SLOTS; slot++) {
		uint32_t a = fs_slot_addr(slot);

		if (!fs_slot_used(slot))
			continue;
		shell_puts("  ");
		for (int i = 0; i < FS_NAME_MAX; i++) {
			char c = FS_FLASH_BYTE(a + 8 + i);
			putchar(c ? c : ' ');
		}
		putchar(' ');
		shell_dec(fs_rd32(a + 4));
		shell_puts(" bytes\n");
		count++;
	}
	shell_dec(count);
	shell_puts(" of ");
	shell_dec(FS_SLOTS);
	shell_puts(" slots used\n");
	return 0;
}

static int shcmd_save(int argc, char **argv)
{
	char line[SHELL_LINE_MAX];
	uint32_t len = 0;
	int slot;

	if (argc != 2 || !fs_name_ok(argv[1])) {
		shell_puts("usage: save <name>   (name: 1-7 characters)\n");
		return 0;
	}
	slot = fs_find(argv[1]);
	if (slot < 0)
		slot = fs_find_free();
	if (slot < 0) {
		shell_puts("no free slot\n");
		return 0;
	}

	shell_puts("enter text, finish with a line containing only '.'\n");
	for (;;) {
		int n = shell_readline(line, SHELL_LINE_MAX);

		if (n == 1 && line[0] == '.')
			break;
		if (len + n + 1 > FS_DATA_MAX) {
			shell_puts("file too big, not saved\n");
			return 0;
		}
		for (int i = 0; i < n; i++)
			fs_buf[FS_HDR + len++] = line[i];
		fs_buf[FS_HDR + len++] = '\n';
	}

	if (fs_store(slot, argv[1], len)) {
		shell_puts("saved ");
		shell_dec(len);
		shell_puts(" bytes\n");
	} else {
		shell_puts("verify failed\n");
	}
	return 0;
}

static int shcmd_cat(int argc, char **argv)
{
	int slot;
	uint32_t a, len;

	if (argc != 2 || (slot = fs_find(argv[1])) < 0) {
		shell_puts(argc != 2 ? "usage: cat <name>\n" : "no such file\n");
		return 0;
	}
	a = fs_slot_addr(slot);
	len = fs_rd32(a + 4);
	for (uint32_t i = 0; i < len; i++)
		putchar(FS_FLASH_BYTE(a + FS_HDR + i));
	return 0;
}

static int shcmd_rm(int argc, char **argv)
{
	int slot;

	if (argc != 2 || (slot = fs_find(argv[1])) < 0) {
		shell_puts(argc != 2 ? "usage: rm <name>\n" : "no such file\n");
		return 0;
	}
	fs_erase(fs_slot_addr(slot));
	return 0;
}

// Erase every slot after asking for confirmation.
static int shcmd_format(int argc, char **argv)
{
	char line[SHELL_LINE_MAX];

	shell_puts("erase all files? type 'yes': ");
	shell_readline(line, SHELL_LINE_MAX);
	if (!shell_streq(line, "yes")) {
		shell_puts("cancelled\n");
		return 0;
	}
	for (int slot = 0; slot < FS_SLOTS; slot++)
		fs_erase(fs_slot_addr(slot));
	shell_puts("formatted\n");
	return 0;
}

// Execute a file line by line as shell commands.
static int shcmd_run(int argc, char **argv)
{
	char line[SHELL_LINE_MAX];
	int slot, n = 0;
	uint32_t a, len;

	if (argc != 2 || (slot = fs_find(argv[1])) < 0) {
		shell_puts(argc != 2 ? "usage: run <name>\n" : "no such file\n");
		return 0;
	}
	if (fs_running) {
		shell_puts("run: cannot be nested\n");
		return 0;
	}
	fs_running = 1;
	a = fs_slot_addr(slot);
	len = fs_rd32(a + 4);

	for (uint32_t i = 0; i <= len; i++) {
		char c = i < len ? (char)FS_FLASH_BYTE(a + FS_HDR + i) : '\n';

		if (c != '\n') {
			if (n < SHELL_LINE_MAX - 1)
				line[n++] = c;
			continue;
		}
		line[n] = 0;
		n = 0;
		if (!line[0])
			continue;
		shell_puts("> ");
		shell_puts(line);
		putchar('\n');
		if (shell_exec(line))
			break;                  // 'exit' inside a file stops the file
	}
	fs_running = 0;
	return 0;
}

// ---------------- other commands ----------------

static int shcmd_help(int argc, char **argv);

static int shcmd_echo(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (i > 1) putchar(' ');
		shell_puts(argv[i]);
	}
	putchar('\n');
	return 0;
}

static int shcmd_peek(int argc, char **argv)
{
	uint32_t addr;

	if (argc != 2 || !shell_parse_num(argv[1], &addr)) {
		shell_puts("usage: peek <addr>\n");
		return 0;
	}
	addr &= ~3u;                                // word aligned access only
	shell_hex(addr, 8);
	shell_puts(": ");
	shell_hex(SHELL_MEM(addr), 8);
	putchar('\n');
	return 0;
}

static int shcmd_poke(int argc, char **argv)
{
	uint32_t addr, val;

	if (argc != 3 || !shell_parse_num(argv[1], &addr) || !shell_parse_num(argv[2], &val)) {
		shell_puts("usage: poke <addr> <value>\n");
		return 0;
	}
	addr &= ~3u;
	SHELL_MEM(addr) = val;
	return 0;
}

static int shcmd_dump(int argc, char **argv)
{
	uint32_t addr, words = 16;

	if (argc < 2 || argc > 3 || !shell_parse_num(argv[1], &addr) ||
			(argc == 3 && !shell_parse_num(argv[2], &words))) {
		shell_puts("usage: dump <addr> [words]\n");
		return 0;
	}
	if (words > 256) words = 256;
	addr &= ~3u;

	for (uint32_t i = 0; i < words; i++) {
		if ((i & 3) == 0) {
			shell_hex(addr, 8);
			putchar(':');
		}
		putchar(' ');
		shell_hex(SHELL_MEM(addr), 8);
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
		shell_puts("usage: led <value>\n");
		return 0;
	}
	reg_leds = val;
	return 0;
}

static int shcmd_exit(int argc, char **argv)
{
	return 1;
}

struct shell_cmd {
	const char *name;
	int (*fn)(int argc, char **argv);
	const char *help;
};

static const struct shell_cmd shell_cmds[] = {
	{ "help", shcmd_help, "help                     list commands" },
	{ "echo", shcmd_echo, "echo <text...>           print the arguments" },
	{ "peek", shcmd_peek, "peek <addr>              read a 32-bit word" },
	{ "poke", shcmd_poke, "poke <addr> <value>      write a 32-bit word" },
	{ "dump", shcmd_dump, "dump <addr> [words]      hex dump (default 16 words)" },
	{ "led",  shcmd_led,  "led <value>              write the LED register" },
	{ "ls",   shcmd_ls,   "ls                       list files in flash" },
	{ "save", shcmd_save, "save <name>              type a text file into flash" },
	{ "cat",  shcmd_cat,  "cat <name>               print a file" },
	{ "rm",   shcmd_rm,   "rm <name>                delete a file" },
	{ "run",  shcmd_run,  "run <name>               execute a file as commands" },
	{ "format", shcmd_format, "format                   erase all files" },
	{ "exit", shcmd_exit, "exit                     return to the menu" },
};

#define SHELL_NUM_CMDS ((int)(sizeof(shell_cmds) / sizeof(shell_cmds[0])))

static int shcmd_help(int argc, char **argv)
{
	for (int i = 0; i < SHELL_NUM_CMDS; i++) {
		shell_puts("  ");
		shell_puts(shell_cmds[i].help);
		putchar('\n');
	}
	shell_puts("numbers: decimal, or hex with 0x prefix\n");
	return 0;
}

// ---------------- main loop ----------------

// Run one command line (modified in place). Returns 1 for 'exit'.
static int shell_exec(char *line)
{
	char *argv[SHELL_ARGS_MAX];
	int argc = shell_split(line, argv, SHELL_ARGS_MAX);
	int i;

	if (argc == 0)
		return 0;

	for (i = 0; i < SHELL_NUM_CMDS; i++)
		if (shell_streq(argv[0], shell_cmds[i].name))
			return shell_cmds[i].fn(argc, argv);

	shell_puts("unknown command: ");
	shell_puts(argv[0]);
	putchar('\n');
	return 0;
}

void shell_run(void)
{
	char line[SHELL_LINE_MAX];

	shell_puts("\npico shell - type 'help'\n");

	for (;;) {
		shell_puts("pico> ");
		shell_readline(line, SHELL_LINE_MAX);
		if (shell_exec(line))
			break;
	}
}
