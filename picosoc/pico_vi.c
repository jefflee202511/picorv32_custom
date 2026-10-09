// ========================================================
// pico_vi.c : a very small vi for the PicoSoC shell
//
// Usage: shell.c includes this file and lists the command  vi <name>.
//
// MEMORY
//   The text lives in the 4 KB buffer PFS already owns (pfs_buffer()).
//   There is no screen buffer and no receive FIFO.
//
// SCREEN
//   11 text rows: the cursor line, 5 lines above it and 5 below. Row 12
//   is the status line. Each key press sends as little as possible:
//     left / right            cursor position only
//     up / down               scroll the window one line, draw one line
//     typing, x, backspace    the rest of the cursor line
//     new line, deleted line  scroll part of the window, draw 1-2 lines
//     Ctrl-L                  everything
//   Needs a VT100-style terminal (cursor positioning, erase line, scroll
//   region). Set VI_USE_SCROLL to 0 to redraw the whole window instead
//   of scrolling if the terminal gets the scroll region wrong.
//
// KEYS
//   normal mode   h j k l / arrows   move        0 $   line start / end
//                 i  insert          a  append   o     open line below
//                 x  delete char     dd delete line
//                 :w  :q  :wq  :q!   Ctrl-L redraw
//   insert mode   text, Enter, Backspace, arrows, ESC = back to normal
//
// LIMITS
//   79 characters per line, 4080 bytes per file, no undo, no search, no
//   copy/paste. Pasting text into the terminal loses characters: the
//   UART holds one received byte, and the editor is busy while it draws.
//
// SPEED
//   The firmware runs from the SPI flash, which is slow. All editor code
//   is placed in RAM (VI_RAM) so key handling keeps up with the UART;
//   an arrow key is three bytes sent back to back.
// ========================================================

#ifndef VI_RAM
#define VI_RAM PORT_RAMFUNC
#endif

#ifndef VI_USE_SCROLL
#define VI_USE_SCROLL 1
#endif

// Polls to wait for the rest of an escape sequence after ESC (about 20 ms).
#ifndef VI_ESC_SPINS
#define VI_ESC_SPINS 3000
#endif

#define VI_ROWS   11                // text rows on screen
#define VI_HALF   5                 // lines kept above / below the cursor
#define VI_COLS   79                // longest line
#define VI_CAP    ((int)PFS_DATA_MAX)

#define VI_K_UP    0x100
#define VI_K_DOWN  0x101
#define VI_K_RIGHT 0x102
#define VI_K_LEFT  0x103
#define VI_K_NONE  0x104

// What changed on screen, for vi_update().
#define VI_C_LINE  1                // redraw the cursor line
#define VI_C_TAIL  2                // redraw the cursor line from column 'arg'
#define VI_C_INS   4                // a line was inserted at index 'arg'
#define VI_C_DEL   8                // the line at index 'arg' was deleted
#define VI_C_PREV  16               // also redraw the line above the cursor
#define VI_C_FULL  32               // redraw everything

// The text is vi_len bytes in vi_buf. It always ends with '\n', so every
// line, including the last, is terminated and vi_buf[vi_cur] is valid.
static uint8_t *vi_buf;
static int vi_len;
static int vi_cur;                  // offset of the cursor
static int vi_row, vi_col;          // line and column of the cursor (from 0)
static int vi_nlines;
static int vi_top;                  // first line shown on screen
static int vi_insert;               // 1 = insert mode
static int vi_dirty;                // 1 = unsaved changes
static int vi_full;                 // 1 = a full redraw is pending
static const char *vi_name;
static const char *vi_msg;          // shown once on the status line
static int vi_pending = -1;         // key read ahead after ESC

// status line contents currently on screen
static int vi_st_row = -1, vi_st_nlines, vi_st_insert, vi_st_dirty;

// ---------------- keyboard ----------------

VI_RAM static int vi_getc(void)
{
	int32_t c;

	if (vi_pending >= 0) {
		c = vi_pending;
		vi_pending = -1;
		return c;
	}
	do {
		c = PORT_UART_TRY();
	} while (c == -1);
	return c & 0xff;
}

VI_RAM static int vi_getc_timeout(void)
{
	for (int32_t n = VI_ESC_SPINS; n > 0; n--) {
		int32_t c = PORT_UART_TRY();
		if (c != -1)
			return c & 0xff;
	}
	return -1;
}

// Returns a character, or a VI_K_ code for an arrow key (ESC [ A..D).
VI_RAM static int vi_getkey(void)
{
	int c = vi_getc();

	if (c != 0x1b)
		return c;
	c = vi_getc_timeout();
	if (c < 0)
		return 0x1b;                // ESC on its own
	if (c != '[') {
		vi_pending = c;             // ESC followed by an ordinary key
		return 0x1b;
	}
	c = vi_getc_timeout();
	if (c == 'A') return VI_K_UP;
	if (c == 'B') return VI_K_DOWN;
	if (c == 'C') return VI_K_RIGHT;
	if (c == 'D') return VI_K_LEFT;
	return VI_K_NONE;
}

// ---------------- text buffer ----------------

// Offset of the start of the line containing 'pos'.
VI_RAM static int vi_bol(int pos)
{
	while (pos > 0 && vi_buf[pos - 1] != '\n')
		pos--;
	return pos;
}

// Offset of the '\n' that ends the line containing 'pos'.
VI_RAM static int vi_eol(int pos)
{
	while (vi_buf[pos] != '\n')
		pos++;
	return pos;
}

// Offset of the start of line 'n', found by walking from the cursor line.
VI_RAM static int vi_line_off(int n)
{
	int pos = vi_cur - vi_col;
	int r = vi_row;

	while (r < n) {
		pos = vi_eol(pos) + 1;
		r++;
	}
	while (r > n) {
		pos = vi_bol(pos - 1);
		r--;
	}
	return pos;
}

// Insert one byte at 'pos'. Returns 0 if the file is full.
VI_RAM static int vi_put(int pos, int c)
{
	if (vi_len >= VI_CAP) {
		vi_msg = "file full";
		return 0;
	}
	for (int i = vi_len; i > pos; i--)
		vi_buf[i] = vi_buf[i - 1];
	vi_buf[pos] = (uint8_t)c;
	vi_len++;
	vi_dirty = 1;
	return 1;
}

// Remove 'n' bytes starting at 'pos'.
VI_RAM static void vi_cut(int pos, int n)
{
	if (n <= 0)
		return;
	for (int i = pos; i + n < vi_len; i++)
		vi_buf[i] = vi_buf[i + n];
	vi_len -= n;
	vi_dirty = 1;
}

// ---------------- screen output ----------------

VI_RAM static void vi_esc(const char *s)
{
	putchar(0x1b);
	io_puts(s);
}

VI_RAM static void vi_goto(int r, int c)
{
	vi_esc("[");
	io_dec((uint32_t)r);
	putchar(';');
	io_dec((uint32_t)c);
	putchar('H');
}

VI_RAM static void vi_region(int a, int b)
{
	vi_esc("[");
	io_dec((uint32_t)a);
	putchar(';');
	io_dec((uint32_t)b);
	putchar('r');
}

// Scroll screen rows a..b by one line. up = 1: contents move up and row b
// becomes free. up = 0: contents move down and row a becomes free.
VI_RAM static void vi_scroll(int a, int b, int up)
{
	if (!VI_USE_SCROLL) {
		vi_full = 1;
		return;
	}
	if (a >= b)
		return;
	vi_region(a, b);
	if (up) {
		vi_goto(b, 1);
		vi_esc("D");                // index: scrolls up at the bottom margin
	} else {
		vi_goto(a, 1);
		vi_esc("M");                // reverse index: scrolls down at the top margin
	}
	vi_region(1, VI_ROWS);
}

VI_RAM static void vi_put_text(int pos)
{
	while (vi_buf[pos] != '\n') {
		int c = vi_buf[pos++];
		putchar(c >= 0x20 && c < 0x7f ? c : '?');
	}
	vi_esc("[K");                   // erase to end of line
}

// Draw screen row 's' (1..VI_ROWS).
VI_RAM static void vi_draw_row(int s)
{
	int n = vi_top + s - 1;

	if (s < 1 || s > VI_ROWS)
		return;
	vi_goto(s, 1);
	if (n < vi_nlines) {
		vi_put_text(vi_line_off(n));
	} else {
		putchar('~');
		vi_esc("[K");
	}
}

VI_RAM static void vi_draw_status(void)
{
	vi_goto(VI_ROWS + 1, 1);
	io_puts(vi_insert ? "-- INSERT -- " : "-- NORMAL -- ");
	io_puts(vi_name);
	if (vi_dirty)
		io_puts(" [+]");
	io_puts("  L");
	io_dec((uint32_t)(vi_row + 1));
	putchar('/');
	io_dec((uint32_t)vi_nlines);
	if (vi_msg) {
		io_puts("  ");
		io_puts(vi_msg);
	}
	vi_esc("[K");
	vi_st_row = vi_row;
	vi_st_nlines = vi_nlines;
	vi_st_insert = vi_insert;
	vi_st_dirty = vi_dirty;
	vi_msg = 0;
}

// First line the window should show: the cursor line in the middle, but
// never past either end of the file.
VI_RAM static int vi_want_top(void)
{
	int t = vi_row - VI_HALF;

	if (t > vi_nlines - VI_ROWS)
		t = vi_nlines - VI_ROWS;
	if (t < 0)
		t = 0;
	return t;
}

// Bring the screen up to date after a key. 'change' says what happened to
// the text (VI_C_ flags), 'arg' is the column or line index it refers to.
VI_RAM static void vi_update(int change, int arg)
{
	int nt = vi_want_top();
	int d = nt - vi_top;
	int s;

	if (change & VI_C_FULL)
		vi_full = 1;

	if (vi_full) {
		// nothing to scroll
	} else if (change & VI_C_INS) {
		if (d == 0) {
			s = arg - vi_top + 1;
			vi_scroll(s, VI_ROWS, 0);
			if (!vi_full) vi_draw_row(s);
		} else if (d == 1) {
			s = arg - nt + 1;
			vi_scroll(1, s, 1);
			vi_top = nt;
			if (!vi_full) vi_draw_row(s);
		} else {
			vi_full = 1;
		}
	} else if (change & VI_C_DEL) {
		s = arg - vi_top + 1;
		if (s > VI_ROWS)
			s = VI_ROWS;
		if (d == 0) {
			vi_scroll(s, VI_ROWS, 1);
			if (!vi_full) vi_draw_row(VI_ROWS);
		} else if (d == -1) {
			vi_scroll(1, s, 0);
			vi_top = nt;
			if (!vi_full) vi_draw_row(1);
		} else {
			vi_full = 1;
		}
	} else if (d == 1) {
		vi_scroll(1, VI_ROWS, 1);
		vi_top = nt;
		if (!vi_full) vi_draw_row(VI_ROWS);
	} else if (d == -1) {
		vi_scroll(1, VI_ROWS, 0);
		vi_top = nt;
		if (!vi_full) vi_draw_row(1);
	} else if (d != 0) {
		vi_full = 1;
	}

	if (vi_full) {
		vi_top = nt;
		for (s = 1; s <= VI_ROWS; s++)
			vi_draw_row(s);
		vi_full = 0;
		vi_st_row = -1;
	} else {
		s = vi_row - vi_top + 1;
		if (change & VI_C_PREV)
			vi_draw_row(s - 1);
		if (change & VI_C_LINE) {
			vi_draw_row(s);
		} else if (change & VI_C_TAIL) {
			vi_goto(s, arg + 1);
			vi_put_text(vi_cur - vi_col + arg);
		}
	}

	if (vi_msg || vi_st_row != vi_row || vi_st_nlines != vi_nlines ||
			vi_st_insert != vi_insert || vi_st_dirty != vi_dirty)
		vi_draw_status();
	vi_goto(vi_row - vi_top + 1, vi_col + 1);
}

// ---------------- cursor movement ----------------

VI_RAM static void vi_left(void)
{
	if (vi_col > 0) {
		vi_cur--;
		vi_col--;
	}
}

// In normal mode the cursor sits on a character, so it stops one short of
// the end of the line; in insert mode it may stand after the last one.
VI_RAM static void vi_right(void)
{
	if (vi_buf[vi_cur] == '\n')
		return;
	if (!vi_insert && vi_buf[vi_cur + 1] == '\n')
		return;
	vi_cur++;
	vi_col++;
}

VI_RAM static void vi_clamp(void)
{
	if (!vi_insert && vi_col > 0 && vi_buf[vi_cur] == '\n') {
		vi_cur--;
		vi_col--;
	}
}

// Go to column 'want' of the line starting at 'start' (or its end).
VI_RAM static void vi_set_col(int start, int want)
{
	vi_cur = start;
	vi_col = 0;
	while (vi_col < want && vi_buf[vi_cur] != '\n') {
		vi_cur++;
		vi_col++;
	}
	vi_clamp();
}

VI_RAM static void vi_down(void)
{
	if (vi_row < vi_nlines - 1) {
		int want = vi_col;
		int start = vi_eol(vi_cur) + 1;

		vi_row++;
		vi_set_col(start, want);
	}
}

VI_RAM static void vi_up(void)
{
	if (vi_row > 0) {
		int want = vi_col;
		int start = vi_bol(vi_cur - vi_col - 1);

		vi_row--;
		vi_set_col(start, want);
	}
}

// ---------------- editing ----------------

// x : delete the character under the cursor
VI_RAM static void vi_del_char(void)
{
	int c0 = vi_col;

	if (vi_buf[vi_cur] == '\n')
		return;
	vi_cut(vi_cur, 1);
	vi_clamp();
	vi_update(VI_C_TAIL, c0);
}

// dd : delete the cursor line
VI_RAM static void vi_del_line(void)
{
	int start = vi_cur - vi_col;
	int end = vi_eol(start);
	int at = vi_row;

	if (vi_nlines == 1) {
		vi_cut(start, end - start);         // keep the final '\n'
		vi_cur = start;
		vi_col = 0;
		vi_update(VI_C_LINE, 0);
		return;
	}
	vi_cut(start, end - start + 1);
	vi_nlines--;
	if (vi_row == vi_nlines) {              // it was the last line
		vi_row--;
		vi_cur = vi_bol(start - 1);
	} else {
		vi_cur = start;
	}
	vi_col = 0;
	vi_update(VI_C_DEL, at);
}

// o : open an empty line below the cursor line and start inserting
VI_RAM static void vi_open_line(void)
{
	int end = vi_eol(vi_cur);

	if (!vi_put(end + 1, '\n')) {
		vi_update(0, 0);
		return;
	}
	vi_cur = end + 1;
	vi_row++;
	vi_col = 0;
	vi_nlines++;
	vi_insert = 1;
	vi_update(VI_C_INS, vi_row);
}

VI_RAM static void vi_type_char(int c)
{
	int linelen = vi_col + (vi_eol(vi_cur) - vi_cur);

	if (linelen >= VI_COLS) {
		vi_msg = "line full";
		vi_update(0, 0);
		return;
	}
	if (!vi_put(vi_cur, c)) {
		vi_update(0, 0);
		return;
	}
	vi_cur++;
	vi_col++;
	vi_update(VI_C_TAIL, vi_col - 1);
}

VI_RAM static void vi_type_enter(void)
{
	int split = vi_buf[vi_cur] != '\n';     // text moves to the new line

	if (!vi_put(vi_cur, '\n')) {
		vi_update(0, 0);
		return;
	}
	vi_cur++;
	vi_row++;
	vi_col = 0;
	vi_nlines++;
	vi_update(VI_C_INS | (split ? VI_C_PREV : 0), vi_row);
}

VI_RAM static void vi_type_backspace(void)
{
	if (vi_col > 0) {
		vi_cut(vi_cur - 1, 1);
		vi_cur--;
		vi_col--;
		vi_update(VI_C_TAIL, vi_col);
	} else if (vi_row > 0) {                // join with the line above
		int prev = vi_bol(vi_cur - 1);
		int prevlen = vi_cur - 1 - prev;
		int curlen = vi_eol(vi_cur) - vi_cur;

		if (prevlen + curlen > VI_COLS) {
			vi_msg = "line full";
			vi_update(0, 0);
			return;
		}
		vi_cut(vi_cur - 1, 1);
		vi_cur--;
		vi_row--;
		vi_col = prevlen;
		vi_nlines--;
		vi_update(VI_C_DEL | VI_C_LINE, vi_row + 1);
	}
}

// ---------------- file ----------------

// Load the file (or start an empty one). Returns 0 if it cannot be edited.
VI_RAM static int vi_load(const char *name)
{
	int slot = pfs_find(name);
	int linelen = 0;

	vi_buf = pfs_buffer();
	vi_len = 0;
	if (slot >= 0) {
		vi_len = (int)pfs_size(slot);
		for (int i = 0; i < vi_len; i++)
			vi_buf[i] = pfs_byte(slot, (uint32_t)i);
	}
	if (vi_len == 0 || vi_buf[vi_len - 1] != '\n') {
		if (vi_len >= VI_CAP) {
			io_puts("vi: file too big to edit\n");
			return 0;
		}
		vi_buf[vi_len++] = '\n';
	}

	vi_nlines = 0;
	for (int i = 0; i < vi_len; i++) {
		if (vi_buf[i] == '\n') {
			vi_nlines++;
			linelen = 0;
		} else if (++linelen > VI_COLS) {
			io_puts("vi: a line is longer than 79 characters\n");
			return 0;
		}
	}

	vi_name = name;
	vi_cur = vi_row = vi_col = vi_top = 0;
	vi_insert = 0;
	vi_dirty = 0;
	vi_pending = -1;
	vi_msg = slot >= 0 ? 0 : "new file";
	return 1;
}

VI_RAM static int vi_save(void)
{
	int err = pfs_write(vi_name, (uint32_t)vi_len);

	if (err) {
		vi_msg = pfs_strerror(err);
		return 0;
	}
	vi_dirty = 0;
	vi_msg = "written";
	return 1;
}

// ':' command line on the status row. Returns 1 to leave the editor.
VI_RAM static int vi_command(void)
{
	char cmd[8];
	int n = 0, quit = 0;

	vi_goto(VI_ROWS + 1, 1);
	putchar(':');
	vi_esc("[K");
	vi_st_row = -1;                         // status line must be redrawn

	for (;;) {
		int c = vi_getkey();

		if (c == '\r' || c == '\n')
			break;
		if (c == 0x1b) {
			vi_update(0, 0);
			return 0;
		}
		if (c == 0x08 || c == 0x7f) {
			if (n > 0) {
				n--;
				io_puts("\b \b");
			}
			continue;
		}
		if (c >= 0x20 && c < 0x7f && n < (int)sizeof(cmd) - 1) {
			cmd[n++] = (char)c;
			putchar(c);
		}
	}
	cmd[n] = 0;

	if (shell_streq(cmd, "w")) {
		vi_save();
	} else if (shell_streq(cmd, "wq")) {
		quit = vi_save();
	} else if (shell_streq(cmd, "q")) {
		if (vi_dirty)
			vi_msg = "unsaved changes (:q! discards them)";
		else
			quit = 1;
	} else if (shell_streq(cmd, "q!")) {
		quit = 1;
	} else if (n > 0) {
		vi_msg = "unknown command";
	}
	if (!quit)
		vi_update(0, 0);
	return quit;
}

// ---------------- main loop ----------------

VI_RAM static void vi_normal_key(int c, int *quit)
{
	if (c == 'h' || c == VI_K_LEFT) {
		vi_left();
		vi_update(0, 0);
	} else if (c == 'l' || c == VI_K_RIGHT) {
		vi_right();
		vi_update(0, 0);
	} else if (c == 'j' || c == VI_K_DOWN) {
		vi_down();
		vi_update(0, 0);
	} else if (c == 'k' || c == VI_K_UP) {
		vi_up();
		vi_update(0, 0);
	} else if (c == '0') {
		vi_cur -= vi_col;
		vi_col = 0;
		vi_update(0, 0);
	} else if (c == '$') {
		vi_set_col(vi_cur - vi_col, VI_COLS);
		vi_update(0, 0);
	} else if (c == 'i') {
		vi_insert = 1;
		vi_update(0, 0);
	} else if (c == 'a') {
		vi_insert = 1;
		vi_right();
		vi_update(0, 0);
	} else if (c == 'o') {
		vi_open_line();
	} else if (c == 'x') {
		vi_del_char();
	} else if (c == 'd') {
		if (vi_getkey() == 'd')
			vi_del_line();
	} else if (c == ':') {
		*quit = vi_command();
	} else if (c == 0x0c) {                 // Ctrl-L
		vi_esc("[2J");
		vi_update(VI_C_FULL, 0);
	}
}

VI_RAM static void vi_insert_key(int c)
{
	if (c == 0x1b) {
		vi_insert = 0;
		vi_left();
		vi_update(0, 0);
	} else if (c == '\r' || c == '\n') {
		vi_type_enter();
	} else if (c == 0x08 || c == 0x7f) {
		vi_type_backspace();
	} else if (c == VI_K_LEFT) {
		vi_left();
		vi_update(0, 0);
	} else if (c == VI_K_RIGHT) {
		vi_right();
		vi_update(0, 0);
	} else if (c == VI_K_DOWN) {
		vi_down();
		vi_update(0, 0);
	} else if (c == VI_K_UP) {
		vi_up();
		vi_update(0, 0);
	} else if (c >= 0x20 && c < 0x7f) {
		vi_type_char(c);
	}
}

VI_RAM static int shcmd_vi(int argc, char **argv)
{
	int quit = 0, saved_debug;

	if (argc != 2 || !pfs_name_valid(argv[1])) {
		io_puts("usage: vi <name>   (name: 1-7 characters)\n");
		return 0;
	}
	if (!vi_load(argv[1]))
		return 0;

	saved_debug = flash_debug;
	flash_debug = 0;                        // the trace would spoil the screen

	vi_esc("[2J");
	vi_region(1, VI_ROWS);
	vi_update(VI_C_FULL, 0);

	while (!quit) {
		int c = vi_getkey();

		if (vi_insert)
			vi_insert_key(c);
		else
			vi_normal_key(c, &quit);
	}

	vi_esc("[r");                           // scroll region back to the whole screen
	vi_goto(VI_ROWS + 2, 1);
	putchar('\n');
	flash_debug = saved_debug;
	return 0;
}
