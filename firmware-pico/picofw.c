#include <stdio.h> // sscanf
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "vga_rgb.pio"
#include "font8x8.h"

#define WIDTH 320
#define HEIGHT 240
#define UART_ID uart0

typedef unsigned int uint; // makes it easier

#define HSYNC_GPIO 16
#define VSYNC_GPIO 17
#define RGB_BASE 18

uint8_t framebuffer[HEIGHT][WIDTH];

PIO pio = pio0;
uint sm_rgb = 0;

int dma_chan; 
volatile int current_line = 0;

#define CMD_QUEUE_SIZE 16
#define CMD_BUF_SIZE 64

char command_queue[CMD_QUEUE_SIZE][CMD_BUF_SIZE];
volatile int cmd_head = 0;
volatile int cmd_tail = 0;

char cmd_buffer[CMD_BUF_SIZE];
volatile int cmd_pos = 0;

volatile absolute_time_t vsync_end_time;
volatile bool vsync_active = false;

// HSYNC

void hsync_pulse() {
    gpio_put(HSYNC_GPIO, 0);
    busy_wait_us_32(4);   // OK outside IRQ
    gpio_put(HSYNC_GPIO, 1);
}

// COMMAND QUEUE

void enqueue_command(char *cmd) {
    int next = (cmd_head + 1) % CMD_QUEUE_SIZE;

    if (next != cmd_tail) { // not full
        strncpy(command_queue[cmd_head], cmd, CMD_BUF_SIZE - 1);
        command_queue[cmd_head][CMD_BUF_SIZE - 1] = 0;
        cmd_head = next;
    }
}

// DRAWING

// Proper 8-bit to 2-bit RGB conversion
static inline uint8_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 6) << 4) |
           ((g >> 6) << 2) |
           (b >> 6);
}

static inline void put_pixel(int x, int y, uint8_t color) {
    if ((unsigned)x >= WIDTH) return;
    if ((unsigned)y >= HEIGHT) return;
    framebuffer[y][x] = color;
}

static inline void fill_rect(int x, int y, int w, int h, uint8_t color) {

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }

    if (x >= WIDTH || y >= HEIGHT) return;
    if (x + w > WIDTH) w = WIDTH - x;
    if (y + h > HEIGHT) h = HEIGHT - y;

    if (w <= 0 || h <= 0) return;

    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            framebuffer[y + j][x + i] = color;
}

void clear_screen(uint8_t color) {
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++)
            framebuffer[y][x] = color;
}

void draw_char(int x, int y, char c, uint8_t color) {

    const uint8_t *bitmap = NULL;

    if (c == ' ')
        bitmap = font8x8_letters[0];
    else if (c >= 'A' && c <= 'Z')
        bitmap = font8x8_letters[1 + (c - 'A')];
    else if (c >= 'a' && c <= 'z')
        bitmap = font8x8_letters[27 + (c - 'a')];
    else if (c >= '0' && c <= '9')
        bitmap = font8x8_numbers[c - '0'];
    else if (c >= 33 && c <= 64)
        bitmap = font8x8_symbols[c - 33];

    if (!bitmap) return;

    for (int row = 0; row < 8; row++) {
        uint8_t bits = bitmap[row];

        for (int col = 0; col < 8; col++) {
            if (bits & (1 << (7 - col)))
                put_pixel(x + col, y + row, color);
        }
    }
}

void draw_text(int x, int y, char *text, uint8_t color) {
    while (*text) {
        draw_char(x, y, *text++, color);
        x += 8;
    }
}

// DMA HANDLER (NO SLEEP INSIDE IRQ)

void dma_handler() {

    dma_hw->ints0 = 1u << dma_chan;

    // Set next line FIRST
    dma_channel_set_read_addr(
        dma_chan,
        framebuffer[current_line],
        true
    );

    current_line++;

    if (current_line >= HEIGHT) {
        current_line = 0;

        // Start VSYNC pulse (non-blocking)
        gpio_put(VSYNC_GPIO, 0);
        vsync_end_time = make_timeout_time_us(64);
        vsync_active = true;
    }

    hsync_pulse();
}

// VGA INIT

void vga_init() {

    for (int i = 0; i < 6; i++) {
        gpio_init(RGB_BASE + i);
        gpio_set_dir(RGB_BASE + i, GPIO_OUT);
    }

    gpio_init(HSYNC_GPIO);
    gpio_set_dir(HSYNC_GPIO, GPIO_OUT);
    gpio_put(HSYNC_GPIO, 1);

    gpio_init(VSYNC_GPIO);
    gpio_set_dir(VSYNC_GPIO, GPIO_OUT);
    gpio_put(VSYNC_GPIO, 1);

    uint offset = pio_add_program(pio, &vga_rgb_program);
    pio_sm_config c = vga_rgb_program_get_default_config(offset);

    sm_config_set_out_pins(&c, RGB_BASE, 6);
    sm_config_set_out_shift(&c, true, true, 8);

    float div = (float)clock_get_hz(clk_sys) / 25000000.0f;
    sm_config_set_clkdiv(&c, div);

    pio_sm_init(pio, sm_rgb, offset, &c);
    pio_sm_set_consecutive_pindirs(pio, sm_rgb, RGB_BASE, 6, true);
    pio_sm_set_enabled(pio, sm_rgb, true);

    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dma_c = dma_channel_get_default_config(dma_chan);

    channel_config_set_transfer_data_size(&dma_c, DMA_SIZE_8);
    channel_config_set_read_increment(&dma_c, true);
    channel_config_set_write_increment(&dma_c, false);
    channel_config_set_dreq(&dma_c,
        pio_get_dreq(pio, sm_rgb, true));

    dma_channel_configure(
        dma_chan,
        &dma_c,
        &pio->txf[sm_rgb],
        framebuffer[0],
        WIDTH,
        false
    );

    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_set_irq0_enabled(dma_chan, true);

    dma_channel_start(dma_chan);
}

// UART

void poll_uart() {

    while (uart_is_readable(UART_ID)) {

        char c = uart_getc(UART_ID);

        if (c == '\n') {
            cmd_buffer[cmd_pos] = 0;
            enqueue_command(cmd_buffer);
            cmd_pos = 0;
        }
        else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buffer[cmd_pos++] = c;
        }
        else {
            cmd_pos = 0; // prevent overflow lock
        }
    }
}

void execute_command(char *cmd) {

    if (strncmp(cmd, "PPX", 3) == 0) {
        int x,y,c;
        if (sscanf(cmd, "PPX %d %d %d", &x, &y, &c) == 3)
            put_pixel(x,y,c);
    }

    else if (strncmp(cmd, "FRECT", 5) == 0) {
        int x,y,w,h,c;
        if (sscanf(cmd, "FRECT %d %d %d %d %d",
                   &x,&y,&w,&h,&c) == 5)
            fill_rect(x,y,w,h,c);
    }

    else if (strncmp(cmd, "TEXT", 4) == 0) {
        int x,y,c;
        char text[64];
        if (sscanf(cmd, "TEXT %d %d %d %[^\n]",
                   &x,&y,&c,text) == 4)
            draw_text(x,y,text,c);
    }
}

void process_commands() {

    while (cmd_tail != cmd_head) {

        char *cmd = command_queue[cmd_tail];
        cmd_tail = (cmd_tail + 1) % CMD_QUEUE_SIZE;

        execute_command(cmd);
    }
}

// MAIN

int main() {

    stdio_init_all();

    uart_init(UART_ID, 115200);
    // these are used to control UART pins
    // TX = GPIO0, RX = GPIO1. They are inputs for commands
    gpio_set_function(0, GPIO_FUNC_UART); // TX
    gpio_set_function(1, GPIO_FUNC_UART); // RX

    vga_init();

    clear_screen(rgb(0,0,255));

    while (1) {

        poll_uart();
        process_commands();

        // End VSYNC without blocking
        if (vsync_active &&
            absolute_time_diff_us(get_absolute_time(),
                                  vsync_end_time) <= 0) {

            gpio_put(VSYNC_GPIO, 1);
            vsync_active = false;
        }
    }
}