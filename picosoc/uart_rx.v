// ========================================================
// xBASIC UART RX IP (파라미터화 및 메타스테빌리티 방지 버전)
// ========================================================

module uart_rx #(
    parameter integer CLK_FREQ  = 12000000,
    parameter integer BAUD_RATE = 115200
)(
    input  wire       clk,
    input  wire       rst,

    input  wire       rx_pin,      // 외부 UART RX 핀 입력
    
    output reg [7:0]  rx_data,     // 수신 완료된 8비트 데이터
    output reg        rx_valid     // 데이터 수신 완료 플래그 (1클럭 주기 유지)
);

    // 보오레이트 및 샘플링 타이밍 계산
    localparam integer CLKS_PER_BIT      = CLK_FREQ / BAUD_RATE;
    localparam integer CLKS_PER_BIT_HALF = CLKS_PER_BIT / 2;

    // FSM 상태 정의
    localparam [1:0] STATE_IDLE  = 2'b00;
    localparam [1:0] STATE_START = 2'b01;
    localparam [1:0] STATE_DATA  = 2'b10;
    localparam [1:0] STATE_STOP  = 2'b11;

    // 내부 제어 레지스터
    reg [1:0]  rx_state;
    reg [31:0] clk_count;   // CLKS_PER_BIT 스케일에 맞춘 32비트 카운터
    reg [2:0]  bit_index;   // 0 ~ 7 데이터 비트 인덱스
    reg [7:0]  rx_shift;    // 데이터 수신용 시프트 레지스터

    // 메타스테빌리티(Metastability) 방지를 위한 2단 동기화 플립플롭
    reg rx_sync0;
    reg rx_sync1;

    always @(posedge clk) begin
        if (rst) begin
            rx_sync0 <= 1'b1;
            rx_sync1 <= 1'b1;
        end else begin
            rx_sync0 <= rx_pin;
            rx_sync1 <= rx_sync0;
        end
    end

    // UART RX 메인 상태 머신
    always @(posedge clk) begin
        if (rst) begin
            rx_state  <= STATE_IDLE;
            clk_count <= 0;
            bit_index <= 0;
            rx_shift  <= 8'h00;
            rx_data   <= 8'h00;
            rx_valid  <= 1'b0;
        end else begin
            // 기본 상태 설정
            rx_valid <= 1'b0;

            case (rx_state)
                STATE_IDLE: begin
                    clk_count <= 0;
                    bit_index <= 0;
                    // Falling Edge 검출 (Start Bit의 시작)
                    if (rx_sync1 == 1'b0) begin
                        rx_state <= STATE_START;
                    end
                end

                STATE_START: begin
                    // Start 비트의 정중앙(Half Point)까지 카운트하여 노이즈 판별
                    if (clk_count >= CLKS_PER_BIT_HALF - 1) begin
                        clk_count <= 0;
                        if (rx_sync1 == 1'b0) begin
                            rx_state <= STATE_DATA;  // 정상 Start 비트 확인 시 데이터 수신 시작
                        end else begin
                            rx_state <= STATE_IDLE;  // 글리치(노이즈)로 판단 시 IDLE 복귀
                        end
                    end else begin
                        clk_count <= clk_count + 1'b1;
                    end
                end

                STATE_DATA: begin
                    // 1비트 주기마다 데이터 샘플링 진행
                    if (clk_count >= CLKS_PER_BIT - 1) begin
                        clk_count <= 0;
                        rx_shift[bit_index] <= rx_sync1; // LSB부터 순차 수신
                        
                        if (bit_index == 7) begin
                            rx_state <= STATE_STOP;
                        end else begin
                            bit_index <= bit_index + 1'b1;
                        end
                    end else begin
                        clk_count <= clk_count + 1'b1;
                    end
                end

                STATE_STOP: begin
                    // Stop 비트 구간을 온전히 채운 후 완료 플래그 출력 및 데이터 업데이트
                    if (clk_count >= CLKS_PER_BIT - 1) begin
                        clk_count <= 0;
                        rx_data   <= rx_shift;
                        rx_valid  <= 1'b1;       // 의도한 데이터 수신 완료 신호 전송
                        rx_state  <= STATE_IDLE;
                    end else begin
                        clk_count <= clk_count + 1'b1;
                    end
                end

                default: rx_state <= STATE_IDLE;
            endcase
        end
    end

endmodule
