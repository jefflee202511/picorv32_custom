// =====================================================================================
// PicoSoC simpleuart-compatible wrapper (uses xBASIC uart_rx / uart_tx)
//
//  - Ports are identical to PicoSoC's simpleuart -> use in place of simpleuart.v
//  - Baud rate is fixed by the CLK_FREQ / BAUD_RATE parameters
//    (the reg_div register is kept for firmware compatibility; it can be
//     read and written but has no effect on the actual baud rate)
// =====================================================================================

module simpleuart #(
	parameter integer DEFAULT_DIV = 1,          // compatibility only (does not affect baud rate)
	parameter integer CLK_FREQ    = 12000000,
	parameter integer BAUD_RATE   = 115200
) (
	input clk,
	input resetn,

	output ser_tx,
	input  ser_rx,

	input   [3:0] reg_div_we,
	input  [31:0] reg_div_di,
	output [31:0] reg_div_do,

	input         reg_dat_we,
	input         reg_dat_re,
	input  [31:0] reg_dat_di,
	output [31:0] reg_dat_do,
	output        reg_dat_wait
);
	wire rst = !resetn;

	// ----------------------------------------------------
	// Divider register (dummy, for firmware compatibility)
	// ----------------------------------------------------
	reg [31:0] cfg_divider;
	assign reg_div_do = cfg_divider;

	always @(posedge clk) begin
		if (!resetn) begin
			cfg_divider <= DEFAULT_DIV;
		end else begin
			if (reg_div_we[0]) cfg_divider[ 7: 0] <= reg_div_di[ 7: 0];
			if (reg_div_we[1]) cfg_divider[15: 8] <= reg_div_di[15: 8];
			if (reg_div_we[2]) cfg_divider[23:16] <= reg_div_di[23:16];
			if (reg_div_we[3]) cfg_divider[31:24] <= reg_div_di[31:24];
		end
	end

	// ----------------------------------------------------
	// RX: latch the 1-clock rx_valid pulse until the CPU reads it
	// ----------------------------------------------------
	wire [7:0] rx_data;
	wire       rx_valid;

	reg  [7:0] recv_buf_data;
	reg        recv_buf_valid;

	uart_rx #(
		.CLK_FREQ (CLK_FREQ),
		.BAUD_RATE(BAUD_RATE)
	) u_rx (
		.clk     (clk),
		.rst     (rst),
		.rx_pin  (ser_rx),
		.rx_data (rx_data),
		.rx_valid(rx_valid)
	);

	always @(posedge clk) begin
		if (!resetn) begin
			recv_buf_data  <= 8'h00;
			recv_buf_valid <= 1'b0;
		end else begin
			if (reg_dat_re)
				recv_buf_valid <= 1'b0;
			// If a read and a receive-complete occur in the same clock, new data wins
			if (rx_valid) begin
				recv_buf_data  <= rx_data;
				recv_buf_valid <= 1'b1;
			end
		end
	end

	// Returns 0xFFFFFFFF (-1) when no data is available (PicoSoC firmware getchar convention)
	assign reg_dat_do = recv_buf_valid ? {24'h000000, recv_buf_data} : ~32'h0;

	// ----------------------------------------------------
	// TX: stall the CPU while busy, issue a 1-clock start when idle
	// ----------------------------------------------------
	wire tx_busy;
	wire tx_start = reg_dat_we && !tx_busy;

	assign reg_dat_wait = reg_dat_we && tx_busy;

	uart_tx #(
		.CLK_FREQ (CLK_FREQ),
		.BAUD_RATE(BAUD_RATE)
	) u_tx (
		.clk  (clk),
		.rst  (rst),
		.start(tx_start),
		.data (reg_dat_di[7:0]),
		.tx   (ser_tx),
		.busy (tx_busy)
	);

endmodule
