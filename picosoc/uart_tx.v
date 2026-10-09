// ========================================================
// xBASIC UART TX IP (보안 및 타이밍 교정 버전)
// ========================================================

module uart_tx #(
    parameter integer CLK_FREQ  = 12000000,
    parameter integer BAUD_RATE = 115200
)(
    input  wire       clk,
    input  wire       rst,

    input  wire       start,
    input  wire [7:0] data,

    output reg        tx,
    output reg        busy
);

    localparam integer CLKS_PER_BIT = CLK_FREQ / BAUD_RATE;

    reg [31:0] clk_count;
    reg [3:0]  bit_index;
    reg [9:0]  tx_shift; // Start(0) + Data(8) + Stop(1) = 10비트 버퍼

    always @(posedge clk) begin
        if (rst) begin
            tx        <= 1'b1;
            busy      <= 1'b0;
            clk_count <= 0;
            bit_index <= 0;
            tx_shift  <= 10'b1111111111;
        end else begin
            
            if (!busy) begin
                tx <= 1'b1; // Idle 상태 항상 High 유지
                
                if (start) begin
                    // 버퍼 구성: [9]=Stop비트(1), [8:1]=데이터(8비트), [0]=Start비트(0)
                    tx_shift  <= {1'b1, data, 1'b0};
                    busy      <= 1'b1;
                    clk_count <= 0;
                    bit_index <= 0;
                    tx        <= 1'b0; // 첫 Start 비트 즉시 출력
                end
            end else begin
                // 보오레이트 타이밍 카운터 1비트 주기 유지
                if (clk_count >= CLKS_PER_BIT - 1) begin
                    clk_count <= 0;

                    // ⚠️ 핵심 보완 (Stop 비트 완료 검증):
                    // bit_index 0~8까지는 Start+Data 전송, index 9는 Stop 비트 전송 구간임
                    if (bit_index == 9) begin
                        busy <= 1'b0; // 9번째 비트(Stop)의 1비트 타임이 온전히 끝난 '직후'에 busy 해제
                        tx   <= 1'b1; // 완전히 IDLE 복귀
                    end else begin
                        // 다음 보낼 비트 인덱스 증가 및 안전한 비트 매핑 출력
                        // (동작 클록 사이클과 시프트 인덱스 정밀 동기화)
                        tx        <= tx_shift[bit_index + 1];
                        bit_index <= bit_index + 1'b1;
                    end
                end else begin
                    clk_count <= clk_count + 1'b1;
                end
            end
        end
    end

endmodule
