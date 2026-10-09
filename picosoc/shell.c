// --------------------------------------------------------
// shell.c : tiny command console + slot file system for PicoSoC firmware
//
// Usage: in firmware.c, below putchar() and the reg_uart_data /
// reg_leds definitions, add
//     #include "shell.c"
// and call shell_run() from a menu entry.
//
// Needs no libc, no libgcc, no mul/div (works with -nostdlib, rv32ic).
// Uses putchar(), reg_uart_data and reg_leds from firmware.c.
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
//   Reading uses the memory-mapped flash. Writing (sector erase 0x20, page
//   program 0x02) is done by fs_ram_cmd(), which runs from RAM and does not
//   return until the flash is ready again.
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

// Code that talks to the flash while it is busy must run from RAM.
#ifndef FS_RAMFUNC
#define FS_RAMFUNC __attribute__((noinline, section(".data.pfs_ram")))
#endif

// spimemio configuration register (0x02000000), accessed byte by byte:
//   byte 0: bit 5 = CS, bit 4 = clock, bits 3..0 = IO3..IO0
//   byte 1: bits 3..0 = output enable for IO3..IO0
//   byte 3: bit 7 = memory-mapped mode (0 = manual mode)
#ifndef FS_SPI_WR
#define FS_SPI_WR(off, val) (((volatile uint8_t*)0x02000000)[off] = (val))
#define FS_SPI_RD()         (((volatile uint8_t*)0x02000000)[0])
#endif

#define FS_POLL_MAX 200000      // status polls before giving up

// Safety check used before every flash command: the RAM routines must really
// have been linked into RAM (addresses below the end of RAM, 128 KB).
#ifndef FS_RAM_OK
#define FS_RAM_OK() ((uint32_t)fs_ram_cmd < 0x00020000u && (uint32_t)fs_ram_read < 0x00020000u)
#endif

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
static int fs_debug = 1;             // trace every flash operation ('fsdbg' toggles)
static int fs_dbg_status;            // last status byte read by fs_ram_cmd()
static int fs_dbg_polls;             // status polls used by the last fs_ram_cmd()
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

// Send one erase or program command and wait until the flash has finished it.
//
// This function runs from RAM (it is placed in .data, which start.s copies
// to RAM at boot). It has to: while the flash is erasing or programming it
// does not answer read commands, so the CPU cannot fetch code from it.
// Returning to flash-resident code before the flash is ready hangs the CPU.
//
// Rules for this function: no calls to other functions, no string
// literals, no switch statements - anything that would touch the flash.
//
// Sequence: manual SPI mode -> write enable (0x06) -> command ->
// poll status (0x05) until BUSY clears -> memory-mapped mode.
// Returns 1 on success, 0 if the flash stayed busy (timeout).
FS_RAMFUNC static int fs_ram_cmd(const uint8_t *data, int len)
{
	int32_t polls = FS_POLL_MAX;
	int status;

	fs_dbg_polls = 0;

	FS_SPI_WR(1, 0x01);                 // IO0 is an output
	FS_SPI_WR(0, 0x20);                 // CS high, clock low
	FS_SPI_WR(3, 0x00);                 // manual (bit-bang) mode

	for (int i = 7; i >= 0; i--) {      // write enable
		int bit = (0x06 >> i) & 1;
		FS_SPI_WR(0, bit);
		FS_SPI_WR(0, bit | 0x10);
	}
	FS_SPI_WR(0, 0x00);
	FS_SPI_WR(0, 0x20);

	for (int n = 0; n < len; n++) {     // the command itself
		for (int i = 7; i >= 0; i--) {
			int bit = (data[n] >> i) & 1;
			FS_SPI_WR(0, bit);
			FS_SPI_WR(0, bit | 0x10);
		}
	}
	FS_SPI_WR(0, 0x00);
	FS_SPI_WR(0, 0x20);                 // CS high: erase / program starts

	do {                                // read status register 1
		status = 0;
		for (int i = 7; i >= 0; i--) {
			int bit = (0x05 >> i) & 1;
			FS_SPI_WR(0, bit);
			FS_SPI_WR(0, bit | 0x10);
		}
		for (int i = 0; i < 8; i++) {
			FS_SPI_WR(0, 0x00);
			FS_SPI_WR(0, 0x10);
			status = (status << 1) | ((FS_SPI_RD() >> 1) & 1);   // IO1 = MISO
		}
		FS_SPI_WR(0, 0x00);
		FS_SPI_WR(0, 0x20);
		fs_dbg_polls++;
	} while ((status & 1) && --polls > 0);      // bit 0 = BUSY

	fs_dbg_status = status;
	FS_SPI_WR(3, 0x80);                 // back to memory-mapped mode
	return (status & 1) ? 0 : 1;
}

// Send a one-byte read command and read 'n' reply bytes. Changes nothing in
// the flash. Runs from RAM for the same reason as fs_ram_cmd().
FS_RAMFUNC static void fs_ram_read(int cmd, uint8_t *out, int n)
{
	FS_SPI_WR(1, 0x01);
	FS_SPI_WR(0, 0x20);
	FS_SPI_WR(3, 0x00);

	for (int i = 7; i >= 0; i--) {
		int bit = (cmd >> i) & 1;
		FS_SPI_WR(0, bit);
		FS_SPI_WR(0, bit | 0x10);
	}
	for (int k = 0; k < n; k++) {
		int v = 0;
		for (int i = 0; i < 8; i++) {
			FS_SPI_WR(0, 0x00);
			FS_SPI_WR(0, 0x10);
			v = (v << 1) | ((FS_SPI_RD() >> 1) & 1);
		}
		out[k] = v;
	}
	FS_SPI_WR(0, 0x00);
	FS_SPI_WR(0, 0x20);
	FS_SPI_WR(3, 0x80);
}

// Returns 1 if the RAM routines are usable; prints why not otherwise.
static int fs_ram_check(void)
{
	if (FS_RAM_OK())
		return 1;
	shell_puts("[fs] flash routines are not in RAM - refusing to touch the flash\n");
	return 0;
}

// Debug trace. Each line is printed in two halves: the first half before
// the flash operation, the second after it. If the board hangs, the last
// thing on the terminal shows which operation it was in.
static void fs_trace_begin(const char *what, uint32_t addr, uint32_t n)
{
	if (!fs_debug)
		return;
	shell_puts("[fs] ");
	shell_puts(what);
	shell_puts(" 0x");
	shell_hex(addr, 6);
	if (n) {
		shell_puts(" +");
		shell_dec(n);
	}
	shell_puts(" ... ");
}

static void fs_trace_end(int ok)
{
	if (!fs_debug)
		return;
	shell_puts(ok ? "ok" : "TIMEOUT");
	shell_puts(" (status=");
	shell_hex(fs_dbg_status, 2);
	shell_puts(" polls=");
	shell_dec(fs_dbg_polls);
	shell_puts(")\n");
}

static int fs_erase(uint32_t addr)
{
	uint8_t b[4];
	int ok;

	b[0] = 0x20;                // 4 KB sector erase
	b[1] = addr >> 16;
	b[2] = addr >> 8;
	b[3] = addr;
	if (!fs_ram_check())
		return 0;
	fs_trace_begin("erase", addr, 0);
	ok = fs_ram_cmd(b, 4);
	fs_trace_end(ok);
	return ok;
}

// Program up to 256 bytes; must not cross a 256-byte page boundary.
static int fs_program(uint32_t addr, const uint8_t *src, int n)
{
	uint8_t b[4 + 256];
	int ok;

	b[0] = 0x02;                // page program
	b[1] = addr >> 16;
	b[2] = addr >> 8;
	b[3] = addr;
	for (int i = 0; i < n; i++)
		b[4 + i] = src[i];
	if (!fs_ram_check())
		return 0;
	fs_trace_begin("program", addr, n);
	ok = fs_ram_cmd(b, 4 + n);
	fs_trace_end(ok);
	return ok;
}

// Write fs_buf (data already at fs_buf + FS_HDR) to a slot.
// Returns 1 if verified, 0 if the read-back differs, -1 on flash timeout.
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

	if (!fs_erase(base))
		return -1;
	for (uint32_t off = 0; off < total; off += 256) {
		uint32_t n = total - off;
		if (n > 256) n = 256;
		if (!fs_program(base + off, fs_buf + off, n))
			return -1;
	}

	for (uint32_t off = 0; off < total; off++) {
		if (FS_FLASH_BYTE(base + off) != fs_buf[off]) {
			if (fs_debug) {
				shell_puts("[fs] verify: mismatch at 0x");
				shell_hex(base + off, 6);
				shell_puts(" wrote ");
				shell_hex(fs_buf[off], 2);
				shell_puts(" read ");
				shell_hex(FS_FLASH_BYTE(base + off), 2);
				putchar('\n');
			}
			return 0;
		}
	}
	if (fs_debug) {
		shell_puts("[fs] verify ");
		shell_dec(total);
		shell_puts(" bytes ok\n");
	}
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

	slot = fs_store(slot, argv[1], len);
	if (slot > 0) {
		shell_puts("saved ");
		shell_dec(len);
		shell_puts(" bytes\n");
	} else {
		shell_puts(slot < 0 ? "flash timeout\n" : "verify failed\n");
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
	if (!fs_erase(fs_slot_addr(slot)))
		shell_puts("flash timeout\n");
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
	for (int slot = 0; slot < FS_SLOTS; slot++) {
		if (!fs_erase(fs_slot_addr(slot))) {
			shell_puts("flash timeout\n");
			return 0;
		}
	}
	shell_puts("formatted\n");
	return 0;
}

// Read the flash ID and status through the RAM routine. Changes nothing:
// use it to check the RAM round trip before trying a write.
static int shcmd_flash(int argc, char **argv)
{
	uint8_t id[3];
	uint8_t st[1];

	if (!fs_ram_check())
		return 0;
	shell_puts("reading id ... ");
	fs_ram_read(0x9f, id, 3);
	shell_hex(id[0], 2); putchar(' ');
	shell_hex(id[1], 2); putchar(' ');
	shell_hex(id[2], 2); putchar('\n');
	shell_puts("reading status ... ");
	fs_ram_read(0x05, st, 1);
	shell_hex(st[0], 2);
	putchar('\n');
	return 0;
}

static int shcmd_fsdbg(int argc, char **argv)
{
	fs_debug = !fs_debug;
	shell_puts(fs_debug ? "flash trace on\n" : "flash trace off\n");
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
	uint32_t pad;       // keeps the entry at 16 bytes: indexing needs no multiply
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
	{ "flash", shcmd_flash, "flash                    read flash id and status (no write)" },
	{ "fsdbg", shcmd_fsdbg, "fsdbg                    flash trace on/off" },
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
