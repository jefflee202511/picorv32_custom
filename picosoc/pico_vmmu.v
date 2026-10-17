module vmmu #(parameter NTHREADS = 8) (
    input clk,
    input user_mode,
    input [$clog2(NTHREADS)-1:0] cur_tid,
    input cfg_we,
    input [$clog2(NTHREADS)-1:0] cfg_tid,
    input [31:0] cfg_base,
    input [31:0] cfg_limit,
    input mem_valid,
    input [31:0] mem_addr,
    output reg phys_valid,
    output reg [31:0] phys_addr,
    output reg fault
);

reg [31:0] base_reg [0:NTHREADS-1];
reg [31:0] limit_reg [0:NTHREADS-1];
reg is_ok;

always @(posedge clk) begin
    if (cfg_we == 1'b1) begin
        base_reg[cfg_tid] <= cfg_base;
        limit_reg[cfg_tid] <= cfg_limit;
    end
end

always @(*) begin
    if (user_mode == 1'b1) begin
        phys_addr = base_reg[cur_tid] + mem_addr;
        if (mem_addr < limit_reg[cur_tid]) is_ok = 1'b1;
        else is_ok = 1'b0;
    end
    else begin
        phys_addr = mem_addr;
        is_ok = 1'b1;
    end
    phys_valid = mem_valid & is_ok;
    fault = mem_valid & ~is_ok;
end

endmodule
