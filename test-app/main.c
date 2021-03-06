/* main.c
 *
 * Test bare-metal blinking led application
 *
 * Copyright (C) 2018 wolfSSL Inc.
 *
 * This file is part of wolfBoot.
 *
 * wolfBoot is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfBoot is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "system.h"
#include "timer.h"
#include "led.h"
#include "hal.h"
#include "wolfboot/wolfboot.h"

#ifdef PLATFORM_stm32f4

extern int update_started;

#define UART3 (0x40004800)

#define UART3_SR       (*(volatile uint32_t *)(UART3))
#define UART3_DR       (*(volatile uint32_t *)(UART3 + 0x04))
#define UART3_BRR      (*(volatile uint32_t *)(UART3 + 0x08))
#define UART3_CR1      (*(volatile uint32_t *)(UART3 + 0x0c))
#define UART3_CR2      (*(volatile uint32_t *)(UART3 + 0x10))

#define UART_CR1_UART_ENABLE    (1 << 13)
#define UART_CR1_SYMBOL_LEN     (1 << 12)
#define UART_CR1_PARITY_ENABLED (1 << 10)
#define UART_CR1_PARITY_ODD     (1 << 9)
#define UART_CR1_TX_ENABLE      (1 << 3)
#define UART_CR1_RX_ENABLE      (1 << 2)
#define UART_CR2_STOPBITS       (3 << 12)
#define UART_SR_TX_EMPTY        (1 << 7)
#define UART_SR_RX_NOTEMPTY     (1 << 5)


#define CLOCK_SPEED (168000000)

#define APB1_CLOCK_ER           (*(volatile uint32_t *)(0x40023840))
#define UART3_APB1_CLOCK_ER_VAL 	(1 << 18)

#define AHB1_CLOCK_ER (*(volatile uint32_t *)(0x40023830))
#define GPIOD_AHB1_CLOCK_ER (1 << 3)
#define GPIOD_BASE 0x40020c00
#define GPIOD_MODE  (*(volatile uint32_t *)(GPIOD_BASE + 0x00))
#define GPIOD_AFL   (*(volatile uint32_t *)(GPIOD_BASE + 0x20))
#define GPIOD_AFH   (*(volatile uint32_t *)(GPIOD_BASE + 0x24))
#define GPIO_MODE_AF (2)
#define UART3_PIN_AF 7
#define UART3_RX_PIN 9
#define UART3_TX_PIN 8

#define MSGSIZE 16
#define PAGESIZE (256)
static uint8_t page[PAGESIZE];
static const char ERR='!';
static const char START='*';
static const char UPDATE='U';
static const char ACK='#';
static uint8_t msg[MSGSIZE];


void uart_write(const char c)
{
    uint32_t reg;
    do {
        reg = UART3_SR;
    } while ((reg & UART_SR_TX_EMPTY) == 0);
    UART3_DR = c;
}

static void uart_pins_setup(void)
{
    uint32_t reg;
    AHB1_CLOCK_ER |= GPIOD_AHB1_CLOCK_ER;
    /* Set mode = AF */
    reg = GPIOD_MODE & ~ (0x03 << (UART3_RX_PIN * 2));
    GPIOD_MODE = reg | (2 << (UART3_RX_PIN * 2));
    reg = GPIOD_MODE & ~ (0x03 << (UART3_TX_PIN * 2));
    GPIOD_MODE = reg | (2 << (UART3_TX_PIN * 2));

    /* Alternate function: use high pins (8 and 9) */
    reg = GPIOD_AFH & ~(0xf << ((UART3_TX_PIN - 8) * 4));
    GPIOD_AFH = reg | (UART3_PIN_AF << ((UART3_TX_PIN - 8) * 4));
    reg = GPIOD_AFH & ~(0xf << ((UART3_RX_PIN - 8) * 4));
    GPIOD_AFH = reg | (UART3_PIN_AF << ((UART3_RX_PIN - 8) * 4));
}

int uart_setup(uint32_t bitrate, uint8_t data, char parity, uint8_t stop)
{
    uint32_t reg;
    /* Enable pins and configure for AF7 */
    uart_pins_setup();
    /* Turn on the device */
    APB1_CLOCK_ER |= UART3_APB1_CLOCK_ER_VAL;

    /* Configure for TX + RX */
    UART3_CR1 |= (UART_CR1_TX_ENABLE | UART_CR1_RX_ENABLE);

    /* Configure clock */
    UART3_BRR =  CLOCK_SPEED / bitrate;

    /* Configure data bits */
    if (data == 8)
        UART3_CR1 &= ~UART_CR1_SYMBOL_LEN;
    else
        UART3_CR1 |= UART_CR1_SYMBOL_LEN;

    /* Configure parity */
    switch (parity) {
        case 'O':
            UART3_CR1 |= UART_CR1_PARITY_ODD;
            /* fall through to enable parity */
        case 'E':
            UART3_CR1 |= UART_CR1_PARITY_ENABLED;
            break;
        default:
            UART3_CR1 &= ~(UART_CR1_PARITY_ENABLED | UART_CR1_PARITY_ODD);
    }
    /* Set stop bits */
    reg = UART3_CR2 & ~UART_CR2_STOPBITS;
    if (stop > 1)
        UART3_CR2 = reg & (2 << 12);
    else
        UART3_CR2 = reg;

    /* Turn on uart */
    UART3_CR1 |= UART_CR1_UART_ENABLE;

    return 0;
}

char uart_read(void)
{
    char c;
    volatile uint32_t reg;
    do {
        reg = UART3_SR;
    } while ((reg & UART_SR_RX_NOTEMPTY) == 0);
    c = (char)(UART3_DR & 0xff);
    return c;
}

static void ack(uint32_t _off)
{
    uint8_t *off = (uint8_t *)(&_off);
    int i;
    uart_write(ACK);
    for (i = 0; i < 4; i++) {
        uart_write(off[i]);
    }
}

static int check(uint8_t *pkt, int size)
{
    int i;
    uint16_t c = 0;
    uint16_t c_rx = *((uint16_t *)(pkt + 2));
    uint16_t *p = (uint16_t *)(pkt + 4);
    for (i = 0; i < ((size - 4) >> 1); i++)
        c += p[i];
    if (c == c_rx)
        return 0;
    return -1;
}

void main(void) {
    uint32_t tlen = 0;
    volatile uint32_t recv_seq;
    uint32_t r_total = 0;
    uint32_t tot_len = 0;
    uint32_t next_seq = 0;
    uint32_t version = 0;
    uint8_t *v_array = (uint8_t *)&version;
    int i;
    memset(page, 0xFF, PAGESIZE);
    boot_led_on();
    flash_set_waitstates();
    clock_config();
    led_pwm_setup();
    pwm_init(CPU_FREQ, 0);
    /* Dim the led by altering the PWM duty-cicle
     * in isr_tim2 (timer.c)
     *
     * Every 50ms, the duty cycle of the PWM connected
     * to the blue led increases/decreases making a pulse
     * effect.
     */
    timer_init(CPU_FREQ, 1, 50);
    uart_setup(115200, 8, 'N', 1);
    memset(page, 0xFF, PAGESIZE);
    asm volatile ("cpsie i");
    /* Initiate update */

    //wolfBoot_success();

    hal_flash_unlock();
    uart_write(START);
    while (1) {
        r_total = 0;
        do {
            while(r_total < 2) {
                msg[r_total++] = uart_read();
                if ((r_total == 2) && ((msg[0] != 0xA5) || msg[1] != 0x5A)) {
                    r_total = 0;
                    continue;
                }
            }
            msg[r_total++] = uart_read();
            if ((tot_len == 0) && r_total == 2 + sizeof(uint32_t))
                break;
            if ((r_total > 8)  && (tot_len <= ((r_total - 8) + next_seq)))
                break;
        } while (r_total < MSGSIZE);
        if (tot_len == 0)  {
            tlen = msg[2] + (msg[3] << 8) + (msg[4] << 16) + (msg[5] << 24);
            if (tlen > WOLFBOOT_PARTITION_SIZE - 8) {
                uart_write(ERR);
                uart_write(ERR);
                uart_write(ERR);
                uart_write(ERR);
                uart_write(START);
                recv_seq = 0;
                tot_len = 0;
                update_started = 1;
                continue;
            }
            tot_len = tlen;
            ack(0);
            continue;
        }
        if (check(msg, r_total) < 0) {
            ack(next_seq);
            continue;
        }
        recv_seq = msg[4] + (msg[5] << 8) + (msg[6] << 16) + (msg[7] << 24);
        if (recv_seq == next_seq)
        {
            int psize = r_total - 8;
            int page_idx = recv_seq % PAGESIZE;
            memcpy(&page[recv_seq % PAGESIZE], msg + 8, psize);
            page_idx += psize;
            if ((page_idx == PAGESIZE) || (next_seq + psize >= tot_len)) {
                uint32_t dst = (WOLFBOOT_PARTITION_UPDATE_ADDRESS + recv_seq + psize) - page_idx;
                if ((dst % WOLFBOOT_SECTOR_SIZE) == 0) {
                    hal_flash_erase(dst, WOLFBOOT_SECTOR_SIZE);
                }
                hal_flash_write(dst, page, PAGESIZE);
                memset(page, 0xFF, PAGESIZE);
            }
            next_seq += psize;
        }
        ack(next_seq);
        if (next_seq >= tot_len) {
            /* Update complete */
            wolfBoot_update_trigger();
            hal_flash_lock();
            break;
        }
    }
    /* Wait for reboot */
    while(1)
        ;
}
#endif

#ifdef PLATFORM_nrf52
#define GPIO_BASE (0x50000000)
#define GPIO_OUT        *((volatile uint32_t *)(GPIO_BASE + 0x504))
#define GPIO_OUTSET     *((volatile uint32_t *)(GPIO_BASE + 0x508))
#define GPIO_OUTCLR     *((volatile uint32_t *)(GPIO_BASE + 0x50C))
#define GPIO_PIN_CNF     ((volatile uint32_t *)(GPIO_BASE + 0x700)) // Array

static void gpiotoggle(uint32_t pin)
{
    uint32_t reg_val = GPIO_OUT;
    GPIO_OUTCLR = reg_val & (1 << pin);
    GPIO_OUTSET = (~reg_val) & (1 << pin);
}

void main(void)
{
    uint32_t pin = 19;
    int i;
    GPIO_PIN_CNF[pin] = 1; /* Output */
    while(1) {
        gpiotoggle(pin);
        for (i = 0; i < 800000; i++)  // Wait a bit.
              asm volatile ("nop");
    }
}

#endif

#ifdef PLATFORM_samr21
void main(void) {
    asm volatile ("cpsie i");
    while(1)
        WFI();
}
#endif

