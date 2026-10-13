// ========================================================
// picofs.c : flash storage for PicoSoC firmware, in one file
//
// Sections, bottom layer first:
//   1. hardware port     addresses and register access (replaceable)
//   2. text output       io_puts / io_hex / io_dec / io_getc
//   3. SPI flash driver  flash_read / flash_erase_sector / flash_write ...
//   4. PFS file system   pfs_find / pfs_write / pfs_remove ...
//
// Each section uses only the sections above it. A different file system
// can replace section 4 and keep using the flash_ functions of section 3.
//
// Usage: shell.c includes this file. To use the storage without the
// shell, include this file in firmware.c below putchar().
//
// No libc, no libgcc, no mul/div (works with -nostdlib, rv32ic).
// ========================================================
#ifndef PICOFS_C
#define PICOFS_C

// --------------------------------------------------------
// 1. HARDWARE PORT : everything that depends on the hardware
//
// The shell and the file system reach the board only through the names
// in this file. Every definition can be replaced by defining it before
// this file is included - that is how pico_simfs.c runs the same code on
// a PC.
// --------------------------------------------------------

#include <stdint.h>

#include <stddef.h>

size_t pico_strlen(const char *s) {
    size_t length = 0;

    while (s[length] != '\0') {
        length++;
    }

    return length;
}

// Character output: putchar() from firmware.c ('\n' becomes "\r\n").
#ifndef putchar
void putchar(char c);
#endif

// UART data register: reads -1 while no byte has been received.
#ifndef reg_uart_data
#define reg_uart_data (*(volatile uint32_t*)0x02000008)
#endif

// Read the UART without waiting: the received byte (0..255), or -1 if
// nothing has arrived. A byte that is read is consumed.
#ifndef PORT_UART_TRY
#define PORT_UART_TRY() ((int32_t)reg_uart_data)
#endif

// True if a byte has arrived on the UART. The byte is consumed.
#ifndef PORT_KEY_PRESSED
#define PORT_KEY_PRESSED() (PORT_UART_TRY() != -1)
#endif

#ifndef reg_leds
#define reg_leds (*(volatile uint32_t*)0x03000000)
#endif

// 32-bit memory access for peek / poke / dump.
#ifndef PORT_MEM32
#define PORT_MEM32(addr) (*(volatile uint32_t*)(addr))
#endif

// One byte of the memory-mapped flash.
#ifndef PORT_FLASH_BYTE
#define PORT_FLASH_BYTE(addr) (*(volatile uint8_t*)(addr))
#endif

// spimemio configuration register (0x02000000), accessed byte by byte:
//   byte 0: bit 5 = CS, bit 4 = clock, bits 3..0 = IO3..IO0
//   byte 1: bits 3..0 = output enable for IO3..IO0
//   byte 3: bit 7 = memory-mapped mode (0 = manual mode)
#ifndef PORT_SPI_WR
#define PORT_SPI_WR(off, val) (((volatile uint8_t*)0x02000000)[off] = (val))
#define PORT_SPI_RD()         (((volatile uint8_t*)0x02000000)[0])
#endif

// Restart the firmware: jump to the CPU reset address (PROGADDR_RESET in
// picosoc.v, 1 MB into the flash). start.s then runs again from the top.
#ifndef PORT_REBOOT
#define PORT_REBOOT() (((void (*)(void))0x00100000u)())
#endif

// Marks a function that must run from RAM. The section is part of .data,
// which start.s copies from flash to RAM at boot.
#ifndef PORT_RAMFUNC
#define PORT_RAMFUNC __attribute__((noinline, section(".data.ramfunc")))
#endif

// CPU cycle counter, used for delays. PicoRV32 counts clock cycles in the
// rdcycle register (ENABLE_COUNTERS, on by default; the benchmark in
// firmware.c uses it too). The reader runs from RAM so that timing loops
// that also poll the UART stay fast.
#ifndef PORT_CYCLES
PORT_RAMFUNC static uint32_t port_cycles(void)
{
	uint32_t c;
	__asm__ volatile ("rdcycle %0" : "=r"(c));
	return c;
}
#define PORT_CYCLES() port_cycles()
#endif

// Clock cycles per millisecond: 12 MHz on iCEBreaker.
#ifndef PORT_CYCLES_PER_MS
#define PORT_CYCLES_PER_MS 12000u
#endif

// True if a function really was linked into RAM (128 KB on iCEBreaker).
#ifndef PORT_IN_RAM
#define PORT_IN_RAM(fn) ((uint32_t)(fn) < 0x00020000u)
#endif

// --------------------------------------------------------
// 2. TEXT OUTPUT - interface
// --------------------------------------------------------

char io_getc(void);                         // blocks until a byte arrives
void io_puts(const char *s);
void io_hex(uint32_t v, int digits);        // fixed number of hex digits
void io_dec(uint32_t v);                    // decimal, no leading zeros
void io_delay_ms(uint32_t ms);              // wait, timed by the CPU cycle counter

// --------------------------------------------------------
// 2. TEXT OUTPUT - implementation
// --------------------------------------------------------

char io_getc(void)
{
	int32_t c;
	do {
		c = reg_uart_data;      // -1 while no byte has been received
	} while (c == -1);
	return (char)c;
}

void io_puts(const char *s)
{
	int len = pico_strlen(s);
	int count = 0;
	while (*s)
	{
		char temp = *s;
		if (((temp == '\"' || temp == '\'') && count == 0 ) ||
		((temp == '\"' || temp == '\'') &&  count == (len -1) ))
		{
		}else
		{
			putchar(temp);
		}

		s++;
		count ++;

	}
}

void io_hex(uint32_t v, int digits)
{
	for (int shift = (digits - 1) << 2; shift >= 0; shift -= 4)
		putchar("0123456789abcdef"[(v >> shift) & 15]);
}

void io_dec(uint32_t v)
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

// Wait 'ms' milliseconds. Each millisecond is measured against the cycle
// counter from a running target time, so the error does not add up.
void io_delay_ms(uint32_t ms)
{
	uint32_t target = PORT_CYCLES();

	while (ms--) {
		target += PORT_CYCLES_PER_MS;
		while ((int32_t)(PORT_CYCLES() - target) < 0)
			;
	}
}

// --------------------------------------------------------
// 3. SPI FLASH DRIVER - interface
//
// Everything that talks to the flash chip goes through these functions.
// File systems (PFS below, and any later one) use only this interface and
// never touch the SPI pins themselves.
//
// Addresses are flash offsets. On PicoSoC the flash is memory-mapped at
// the same addresses, so offset 0x200000 is also CPU address 0x00200000.
// --------------------------------------------------------

#define FLASH_SECTOR 4096               // smallest erasable unit
#define FLASH_PAGE   256                // largest unit one program command can write

#define FLASH_OK            0
#define FLASH_ERR_TIMEOUT  -101         // flash stayed busy
#define FLASH_ERR_VERIFY   -102         // read-back differs from what was written
#define FLASH_ERR_NOTRAM   -103         // RAM routines were not linked into RAM

extern int flash_debug;                 // 1 = trace every erase / program

// --- reading (memory-mapped, changes nothing) ---
uint8_t flash_read8(uint32_t addr);
void    flash_read(uint32_t addr, uint8_t *buf, uint32_t n);

// --- writing ---
// Erase the 4 KB sector that contains 'addr' (every byte becomes 0xFF).
int flash_erase_sector(uint32_t addr);

// Write 'n' bytes. Any address and length: the data is split at 256-byte
// page boundaries. Writing can only change bits from 1 to 0, so the area
// must have been erased first.
int flash_write(uint32_t addr, const uint8_t *data, uint32_t n);

// Compare the flash contents with 'data'.
int flash_verify(uint32_t addr, const uint8_t *data, uint32_t n);

// --- diagnostics ---
// Reads the JEDEC id (3 bytes) and status register 1. Changes nothing.
int         flash_info(uint8_t id[3], uint8_t *status);
const char *flash_strerror(int err);

// --------------------------------------------------------
// 3. SPI FLASH DRIVER - implementation for PicoSoC (spimemio)
//
// Reading uses the memory-mapped flash. Erasing and programming switch
// spimemio to manual mode and move the SPI pins from software.
//
// The one rule this file exists to enforce:
//   While the flash is erasing or programming it does not answer read
//   commands, so the CPU cannot fetch code from it. The firmware itself
//   runs from the flash. Therefore the code that sends an erase/program
//   command must run from RAM and must not return until the flash
//   reports that it has finished.
//
// If interrupts are ever enabled, they must be masked around
// flash_ram_cmd() / flash_ram_read() unless the handler is in RAM.
// --------------------------------------------------------

#define FLASH_POLL_MAX 200000           // status polls before giving up

int flash_debug = 1;

static int flash_dbg_status;            // last status byte read by flash_ram_cmd()
static int flash_dbg_polls;             // status polls used by the last flash_ram_cmd()

// ---------------- RAM routines ----------------

// Send one erase or program command and wait until the flash has finished it.
//
// Rules for this function: no calls to other functions, no string
// literals, no switch statements - anything that would touch the flash.
//
// Sequence: manual SPI mode -> write enable (0x06) -> command ->
// poll status (0x05) until BUSY clears -> memory-mapped mode.
// Returns 1 on success, 0 if the flash stayed busy (timeout).
PORT_RAMFUNC static int flash_ram_cmd(const uint8_t *data, int len)
{
	int32_t polls = FLASH_POLL_MAX;
	int status;

	flash_dbg_polls = 0;

	PORT_SPI_WR(1, 0x01);               // IO0 is an output
	PORT_SPI_WR(0, 0x20);               // CS high, clock low
	PORT_SPI_WR(3, 0x00);               // manual (bit-bang) mode

	for (int i = 7; i >= 0; i--) {      // write enable
		int bit = (0x06 >> i) & 1;
		PORT_SPI_WR(0, bit);
		PORT_SPI_WR(0, bit | 0x10);
	}
	PORT_SPI_WR(0, 0x00);
	PORT_SPI_WR(0, 0x20);

	for (int n = 0; n < len; n++) {     // the command itself
		for (int i = 7; i >= 0; i--) {
			int bit = (data[n] >> i) & 1;
			PORT_SPI_WR(0, bit);
			PORT_SPI_WR(0, bit | 0x10);
		}
	}
	PORT_SPI_WR(0, 0x00);
	PORT_SPI_WR(0, 0x20);               // CS high: erase / program starts

	do {                                // read status register 1
		status = 0;
		for (int i = 7; i >= 0; i--) {
			int bit = (0x05 >> i) & 1;
			PORT_SPI_WR(0, bit);
			PORT_SPI_WR(0, bit | 0x10);
		}
		for (int i = 0; i < 8; i++) {
			PORT_SPI_WR(0, 0x00);
			PORT_SPI_WR(0, 0x10);
			status = (status << 1) | ((PORT_SPI_RD() >> 1) & 1);    // IO1 = MISO
		}
		PORT_SPI_WR(0, 0x00);
		PORT_SPI_WR(0, 0x20);
		flash_dbg_polls++;
	} while ((status & 1) && --polls > 0);      // bit 0 = BUSY

	flash_dbg_status = status;
	PORT_SPI_WR(3, 0x80);               // back to memory-mapped mode
	return (status & 1) ? 0 : 1;
}

// Send a one-byte read command and read 'n' reply bytes. Changes nothing in
// the flash. Runs from RAM because manual mode makes the flash unreadable.
PORT_RAMFUNC static void flash_ram_read(int cmd, uint8_t *out, int n)
{
	PORT_SPI_WR(1, 0x01);
	PORT_SPI_WR(0, 0x20);
	PORT_SPI_WR(3, 0x00);

	for (int i = 7; i >= 0; i--) {
		int bit = (cmd >> i) & 1;
		PORT_SPI_WR(0, bit);
		PORT_SPI_WR(0, bit | 0x10);
	}
	for (int k = 0; k < n; k++) {
		int v = 0;
		for (int i = 0; i < 8; i++) {
			PORT_SPI_WR(0, 0x00);
			PORT_SPI_WR(0, 0x10);
			v = (v << 1) | ((PORT_SPI_RD() >> 1) & 1);
		}
		out[k] = v;
	}
	PORT_SPI_WR(0, 0x00);
	PORT_SPI_WR(0, 0x20);
	PORT_SPI_WR(3, 0x80);
}

static int flash_ram_ok(void)
{
	return PORT_IN_RAM(flash_ram_cmd) && PORT_IN_RAM(flash_ram_read);
}

// ---------------- debug trace ----------------

// Each line is printed in two halves: the first before the flash
// operation, the second after it. If the board hangs, the last thing on
// the terminal shows which operation it was in.
static void flash_trace_begin(const char *what, uint32_t addr, uint32_t n)
{
	if (!flash_debug)
		return;
	io_puts("[flash] ");
	io_puts(what);
	io_puts(" 0x");
	io_hex(addr, 6);
	if (n) {
		io_puts(" +");
		io_dec(n);
	}
	io_puts(" ... ");
}

static void flash_trace_end(int ok)
{
	if (!flash_debug)
		return;
	io_puts(ok ? "ok" : "TIMEOUT");
	io_puts(" (status=");
	io_hex(flash_dbg_status, 2);
	io_puts(" polls=");
	io_dec(flash_dbg_polls);
	io_puts(")\n");
}

// ---------------- reading ----------------

uint8_t flash_read8(uint32_t addr)
{
	return PORT_FLASH_BYTE(addr);
}

void flash_read(uint32_t addr, uint8_t *buf, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		buf[i] = PORT_FLASH_BYTE(addr + i);
}

// ---------------- writing ----------------

int flash_erase_sector(uint32_t addr)
{
	uint8_t b[4];
	int ok;

	addr &= ~(uint32_t)(FLASH_SECTOR - 1);
	b[0] = 0x20;                // 4 KB sector erase
	b[1] = addr >> 16;
	b[2] = addr >> 8;
	b[3] = addr;
	if (!flash_ram_ok())
		return FLASH_ERR_NOTRAM;
	flash_trace_begin("erase", addr, 0);
	ok = flash_ram_cmd(b, 4);
	flash_trace_end(ok);
	return ok ? FLASH_OK : FLASH_ERR_TIMEOUT;
}

// One page program command: at most 256 bytes, inside one page.
static int flash_program_page(uint32_t addr, const uint8_t *src, uint32_t n)
{
	uint8_t b[4 + FLASH_PAGE];  // on the stack = in RAM, as flash_ram_cmd() needs
	int ok;

	b[0] = 0x02;                // page program
	b[1] = addr >> 16;
	b[2] = addr >> 8;
	b[3] = addr;
	for (uint32_t i = 0; i < n; i++)
		b[4 + i] = src[i];
	flash_trace_begin("program", addr, n);
	ok = flash_ram_cmd(b, 4 + (int)n);
	flash_trace_end(ok);
	return ok ? FLASH_OK : FLASH_ERR_TIMEOUT;
}

int flash_write(uint32_t addr, const uint8_t *data, uint32_t n)
{
	if (!flash_ram_ok())
		return FLASH_ERR_NOTRAM;

	while (n > 0) {
		uint32_t room = FLASH_PAGE - (addr & (FLASH_PAGE - 1));    // bytes left in this page
		uint32_t chunk = n < room ? n : room;
		int err = flash_program_page(addr, data, chunk);

		if (err)
			return err;
		addr += chunk;
		data += chunk;
		n -= chunk;
	}
	return FLASH_OK;
}

int flash_verify(uint32_t addr, const uint8_t *data, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		uint8_t got = PORT_FLASH_BYTE(addr + i);

		if (got != data[i]) {
			if (flash_debug) {
				io_puts("[flash] verify: mismatch at 0x");
				io_hex(addr + i, 6);
				io_puts(" wrote ");
				io_hex(data[i], 2);
				io_puts(" read ");
				io_hex(got, 2);
				putchar('\n');
			}
			return FLASH_ERR_VERIFY;
		}
	}
	if (flash_debug) {
		io_puts("[flash] verify 0x");
		io_hex(addr, 6);
		io_puts(" +");
		io_dec(n);
		io_puts(" ok\n");
	}
	return FLASH_OK;
}

// ---------------- diagnostics ----------------

int flash_info(uint8_t id[3], uint8_t *status)
{
	if (!flash_ram_ok())
		return FLASH_ERR_NOTRAM;
	flash_ram_read(0x9f, id, 3);
	flash_ram_read(0x05, status, 1);
	return FLASH_OK;
}

const char *flash_strerror(int err)
{
	if (err == FLASH_OK)          return "ok";
	if (err == FLASH_ERR_TIMEOUT) return "flash timeout";
	if (err == FLASH_ERR_VERIFY)  return "verify failed";
	if (err == FLASH_ERR_NOTRAM)  return "flash routines are not in RAM - flash not touched";
	return "unknown flash error";
}

// --------------------------------------------------------
// 4. PFS (Pico Flash Slots) - file system interface
//
// PFS_SLOTS fixed slots, one 4 KB flash sector each, starting at flash
// offset PFS_BASE. A slot is:
//     bytes 0..3   magic "PFS1"   (anything else = free slot)
//     bytes 4..7   data length, little endian
//     bytes 8..15  file name, NUL terminated (max 7 characters)
//     bytes 16..   data (max 4080 bytes)
//
// !! PFS_BASE must not overlap the bitstream (flash offset 0) or the
// !! firmware (flash offset 1 MB on iCEBreaker).
// --------------------------------------------------------

#define PFS_BASE      0x00200000u       // flash offset 2 MB
#define PFS_SLOTS     8
#define PFS_SECTOR    4096
#define PFS_HDR       16
#define PFS_NAME_MAX  8                 // including the terminating NUL
#define PFS_DATA_MAX  (PFS_SECTOR - PFS_HDR)

// Result codes. Functions that write may also return a FLASH_ERR_ code
// from the flash driver; pfs_strerror() understands both.
#define PFS_OK            0
#define PFS_ERR_NAME     -1             // name empty or longer than 7 characters
#define PFS_ERR_NOTFOUND -2
#define PFS_ERR_FULL     -3             // no free slot
#define PFS_ERR_TOOBIG   -4

// --- reading (changes nothing) ---
int      pfs_name_valid(const char *name);      // 1 if usable as a file name
int      pfs_used(int slot);                    // 1 if the slot holds a file
int      pfs_find(const char *name);            // slot number, or -1
uint32_t pfs_size(int slot);                    // data length in bytes
void     pfs_name(int slot, char *out);         // copies PFS_NAME_MAX bytes
uint8_t  pfs_byte(int slot, uint32_t offset);   // one data byte

// --- writing ---
// Fill pfs_buffer() with up to PFS_DATA_MAX bytes, then call pfs_write().
uint8_t *pfs_buffer(void);
int      pfs_write(const char *name, uint32_t len);
int      pfs_remove(const char *name);
int      pfs_format(void);                      // erases every slot

const char *pfs_strerror(int err);

// --------------------------------------------------------
// 4. PFS (Pico Flash Slots) - implementation
//
// Only the layout of files in the flash lives here. All access to the
// flash chip goes through the flash_ functions of section 3; this section
// knows nothing about SPI and nothing about the shell.
// --------------------------------------------------------

static uint8_t pfs_buf[PFS_SECTOR];     // RAM image of one slot (header + data)

static uint32_t pfs_slot_addr(int slot)
{
	return PFS_BASE + ((uint32_t)slot << 12);
}

static uint32_t pfs_rd32(uint32_t addr)
{
	return (uint32_t)flash_read8(addr) |
		((uint32_t)flash_read8(addr + 1) << 8) |
		((uint32_t)flash_read8(addr + 2) << 16) |
		((uint32_t)flash_read8(addr + 3) << 24);
}

int pfs_name_valid(const char *name)
{
	int n = 0;

	while (name[n])
		n++;
	return n > 0 && n < PFS_NAME_MAX;
}

static int pfs_find_free(void)
{
	for (int slot = 0; slot < PFS_SLOTS; slot++)
		if (!pfs_used(slot))
			return slot;
	return -1;
}

// ---------------- reading ----------------

int pfs_used(int slot)
{
	uint32_t a = pfs_slot_addr(slot);

	return flash_read8(a) == 'P' && flash_read8(a + 1) == 'F' &&
		flash_read8(a + 2) == 'S' && flash_read8(a + 3) == '1' &&
		pfs_rd32(a + 4) <= PFS_DATA_MAX;
}

int pfs_find(const char *name)
{
	for (int slot = 0; slot < PFS_SLOTS; slot++) {
		uint32_t a = pfs_slot_addr(slot) + 8;
		int i = 0;

		if (!pfs_used(slot))
			continue;
		while (i < PFS_NAME_MAX && name[i] && name[i] == (char)flash_read8(a + i))
			i++;
		if (i < PFS_NAME_MAX && !name[i] && !flash_read8(a + i))
			return slot;
	}
	return -1;
}

uint32_t pfs_size(int slot)
{
	return pfs_rd32(pfs_slot_addr(slot) + 4);
}

void pfs_name(int slot, char *out)
{
	flash_read(pfs_slot_addr(slot) + 8, (uint8_t *)out, PFS_NAME_MAX);
	out[PFS_NAME_MAX - 1] = 0;
}

uint8_t pfs_byte(int slot, uint32_t offset)
{
	return flash_read8(pfs_slot_addr(slot) + PFS_HDR + offset);
}

// ---------------- writing ----------------

uint8_t *pfs_buffer(void)
{
	return pfs_buf + PFS_HDR;
}

int pfs_write(const char *name, uint32_t len)
{
	uint32_t base, total = PFS_HDR + len;
	int slot, i, err;

	if (!pfs_name_valid(name))
		return PFS_ERR_NAME;
	if (len > PFS_DATA_MAX)
		return PFS_ERR_TOOBIG;
	slot = pfs_find(name);
	if (slot < 0)
		slot = pfs_find_free();
	if (slot < 0)
		return PFS_ERR_FULL;
	base = pfs_slot_addr(slot);

	pfs_buf[0] = 'P'; pfs_buf[1] = 'F'; pfs_buf[2] = 'S'; pfs_buf[3] = '1';
	pfs_buf[4] = len; pfs_buf[5] = len >> 8; pfs_buf[6] = len >> 16; pfs_buf[7] = len >> 24;
	for (i = 0; i < PFS_NAME_MAX && name[i]; i++)
		pfs_buf[8 + i] = name[i];
	for (; i < PFS_NAME_MAX; i++)
		pfs_buf[8 + i] = 0;

	err = flash_erase_sector(base);
	if (!err)
		err = flash_write(base, pfs_buf, total);
	if (!err)
		err = flash_verify(base, pfs_buf, total);
	return err;
}

int pfs_remove(const char *name)
{
	int slot = pfs_find(name);

	if (slot < 0)
		return PFS_ERR_NOTFOUND;
	return flash_erase_sector(pfs_slot_addr(slot));
}

int pfs_format(void)
{
	for (int slot = 0; slot < PFS_SLOTS; slot++) {
		int err = flash_erase_sector(pfs_slot_addr(slot));
		if (err)
			return err;
	}
	return PFS_OK;
}

const char *pfs_strerror(int err)
{
	if (err == PFS_OK)           return "ok";
	if (err == PFS_ERR_NAME)     return "bad name (1-7 characters)";
	if (err == PFS_ERR_NOTFOUND) return "no such file";
	if (err == PFS_ERR_FULL)     return "no free slot";
	if (err == PFS_ERR_TOOBIG)   return "file too big";
	return flash_strerror(err);
}

#endif // PICOFS_C
