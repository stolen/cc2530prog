/*
 * cc2530prog - Texas Instruments CC2530 programming tool
 *
 * Copyright (C) 2010, Florian Fainelli <f.fainelli@gmail.com>
 *
 * This file is part of "cc2530prog", this file is distributed under
 * a 2-clause BSD license, see LICENSE for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include "gpio.h"

#define ARRAY_SIZE(x)		(sizeof((x)) / sizeof((x[0])))
#define DIV_ROUND_UP(n,d)	(((n) + (d) - 1) / (d))

static unsigned int gpios[3] = { RST_GPIO, CCLK_GPIO, DATA_GPIO };

struct cc2530_cmd {
	char name[32];
	uint8_t	id;
	uint8_t in;
	uint8_t out;
};

static unsigned debug_enabled;
static unsigned verbose, progress;

#define CLOCK_PULSE_NS		5
#define DIR_CHANGE_NS		73

#define DEFAULT_TIMEOUT		1000

#define CMD_ERASE		0x10
#define CMD_WR_CFG		0x18
#define CMD_RD_CFG		0x20
#define CMD_GET_PC		0x28
#define CMD_RD_ST		0x30
#define CMD_SET_BRK		0x38
#define CMD_HALT		0x40
#define CMD_RESUME		0x48
#define CMD_DBG_INST		0x50
#define CMD_STEP_INST		0x58
#define CMD_GET_BM		0x60
#define CMD_GET_CHIP		0x68
#define CMD_BURST_WR		0x80

/* Various chip statuses */
#define STACK_OVF		0x01
#define OSC_STABLE		0x02
#define DBG_LOCKED		0x04
#define HALT_STATUS		0x08
#define PWR_MODE_0		0x10
#define CPU_HALTED		0x20
#define PCON_IDLE		0x40
#define CHIP_ERASE_BSY		0x80

#define FCTL_BUSY		0x80

/* IDs that we recognize */
#define CC2530_ID		0xA5
#define CC2541_ID		0x41

/* Buffers */
#define ADDR_BUF0		0x0000 /* 1K */
#define ADDR_BUF1		0x0400 /* 1K */
#define ADDR_DMA_DESC		0x0800 /* 32 bytes */

/* DMA Channels */
#define CH_DBG_TO_BUF0	 	0x02
#define CH_DBG_TO_BUF1	 	0x04
#define CH_BUF0_TO_FLASH 	0x08
#define CH_BUF1_TO_FLASH 	0x10

#define PROG_BLOCK_SIZE		1024
#define LOCK_DATA_SIZE      16

#define LOBYTE(w) ((uint8_t)(w))
#define HIBYTE(w) ((uint8_t)(((uint16_t)(w) >> 8) & 0xFF))

/*
 * Register offsets from ioCC2530.h
 * make sure that these are always extended registers (16-bits addr)
 * otherwise this simply will not work.
 */
#define X_EXT_ADDR_BASE	0x616A
#define DBGDATA		0x6260
#define FCTL		0x6270
#define FLASH_BANK_SIZE 0x8000
#define FLASH_WORD_SIZE 4
#define FADDRL		0x6271
#define FADDRH		0x6272
#define FWDATA		0x6273
#define X_CHIPINFO0	0x6276
#define X_CHIPINFO1	0x6277

#define MEMCTR	    0xC7
#define X_MEMCTR	(0x70 | MEMCTR)
#define X_DMA1CFGH	0x70D3
#define X_DMA1CFGL	0x70D4
#define X_DMAARM	0x70D6

#define X_CLKCONCMD	0x70C6
#define X_CLKCONSTA	0x709E

const uint8_t dma_desc[32] = {
	/* Debug Interface -> Buffer 0 (Channel 1) */
	HIBYTE(DBGDATA),   		/* src[15:8] */
	LOBYTE(DBGDATA),   		/* src[7:0] */
	HIBYTE(ADDR_BUF0),              /* dest[15:8] */
	LOBYTE(ADDR_BUF0),              /* dest[7:0] */
	HIBYTE(PROG_BLOCK_SIZE),
	LOBYTE(PROG_BLOCK_SIZE),
	31,                             /* trigger DBG_BW */
	0x11,                           /* increment destination */

	/* Debug Interface -> Buffer 1 (Channel 2) */
	HIBYTE(DBGDATA),   		/* src[15:8] */
	LOBYTE(DBGDATA),   		/* src[7:0] */
	HIBYTE(ADDR_BUF1),              /* dest[15:8] */
	LOBYTE(ADDR_BUF1),              /* dest[7:0] */
	HIBYTE(PROG_BLOCK_SIZE),
	LOBYTE(PROG_BLOCK_SIZE),
	31,                             /* trigger DBG_BW */
	0x11,                           /* increment destination */

	/* Buffer 0 -> Flash controller (Channel 3) */
	HIBYTE(ADDR_BUF0),              /* src[15:8] */
	LOBYTE(ADDR_BUF0),              /* src[7:0] */
	HIBYTE(FWDATA),    		/* dest[15:8] */
	LOBYTE(FWDATA),    		/* dest[7:0] */
	HIBYTE(PROG_BLOCK_SIZE),
	LOBYTE(PROG_BLOCK_SIZE),
	18,                             /* trigger FLASH */
	0x42,                           /* increment source */

	/* Buffer 1 -> Flash controller (Channel 4) */
	HIBYTE(ADDR_BUF1),              /* src[15:8] */
	LOBYTE(ADDR_BUF1),              /* src[7:0] */
	HIBYTE(FWDATA),    		/* dest[15:8] */
	LOBYTE(FWDATA),    		/* dest[7:0] */
	HIBYTE(PROG_BLOCK_SIZE),
	LOBYTE(PROG_BLOCK_SIZE),
	18,                             /* trigger FLASH */
	0x42                            /* increment source */
};

static uint32_t flash_ptr = 0;

static void init_flash_ptr(void)
{
	flash_ptr = 0;
}

static unsigned char *fwdata;

static inline uint8_t get_next_flash_byte(void)
{
	return fwdata[flash_ptr++];
}

static inline void bytes_to_bits(uint8_t byte)
{
	int i;

	for (i = 0; i < 8; i++) {
		if (byte & (1 << i))
			printf("1");
		else
			printf("0");
	}
}

static struct cc2530_cmd cc2530_commands[] = {
	{
		.name	= "erase",
		.id	= CMD_ERASE,
		.in	= 0,
		.out	= 1,
	}, {
		.name	= "write_config",
		.id	= CMD_WR_CFG,
		.in	= 1,
		.out	= 1,
	}, {
		.name	= "read_config",
		.id	= CMD_RD_CFG,
		.in	= 0,
		.out	= 1,
	}, {
		.name	= "get_pc",
		.id	= CMD_GET_PC,
		.in	= 0,
		.out	= 2,
	}, {
		.name	= "read_status",
		.id	= CMD_RD_ST,
		.in	= 0,
		.out	= 1,
	}, {
		.name	= "halt",
		.id	= CMD_HALT,
		.in	= 0,
		.out	= 1,
	}, {
		.name	= "resume",
		.id	= CMD_RESUME,
		.in	= 0,
		.out	= 1,
	}, {
		.name	= "debug_inst",
		.id	= CMD_DBG_INST,
		.in	= -1,		/* variable */
		.out	= 1,
	}, {
		.name	= "step_inst",
		.id	= CMD_STEP_INST,
		.in	= 0,
		.out	= 1,
	}, {
		.name	= "get_bm",
		.id	= CMD_GET_BM,
		.in	= 0,
		.out	= 1,
	}, {
		.name	= "get_chip_id",
		.id	= CMD_GET_CHIP,
		.in	= 0,
		.out	= 2,
	}, {
		.name	= "burst_write",
		.id	= CMD_BURST_WR,
		.in	= -1,		/* variable */
		.out	= 1,
	},
};

static inline struct cc2530_cmd *find_cmd_by_name(const char *name)
{
	int len;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cc2530_commands); i++) {
		len = strlen(name);
		if (!strncmp(cc2530_commands[i].name, name, len))
			return &cc2530_commands[i];
	}

	return NULL;
}

static void cc2530_show_command_list(void)
{
	unsigned int i;

	printf("Supported commands:\n");
	for (i = 0; i < ARRAY_SIZE(cc2530_commands); i++)
		printf("\t%s\n", cc2530_commands[i].name);
}

/*
 * Perform GPIO initialization
 */
static int cc2530_gpio_init(void)
{
	int ret;
	unsigned int i;

    if (gpio_init() != 0) {
        fprintf(stderr, "failed to initialize gpio\n");
        return -1;
    }
	for (i = 0; i < ARRAY_SIZE(gpios); i++) {
		ret = gpio_export(gpios[i]);
		if (ret) {
			fprintf(stderr, "failed to export %d\n", gpios[i]);
			return ret;
		}

		ret = gpio_set_direction(gpios[i], GPIO_DIRECTION_OUT);
		if (ret) {
			fprintf(stderr, "failed to set direction on %d\n", gpios[i]);
			return ret;
		}
	}

	return 0;
}

/*
 * Put back GPIOs in a sane state
 */
static int cc2530_gpio_deinit(void)
{
	int ret;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gpios); i++) {
		ret = gpio_set_direction(gpios[i], GPIO_DIRECTION_IN);
		if (ret) {
			fprintf(stderr, "failed to set direction on %d\n", gpios[i]);
			return ret;
		}

		ret = gpio_unexport(gpios[i]);
		if (ret) {
			fprintf(stderr, "failed to unexport %d\n", gpios[i]);
			return ret;
		}
	}

	return 0;
}


static void delay_ns(unsigned int ns)
{
    struct timespec sleeper, dummy;
    sleeper.tv_sec  = 0;
    sleeper.tv_nsec = ns;
    nanosleep (&sleeper, &dummy) ;
}

void delay (unsigned int millis)
{
  struct timespec sleeper, dummy ;

  sleeper.tv_sec  = (time_t)(millis / 1000) ;
  sleeper.tv_nsec = (long)(millis % 1000) * 1000000 ;
  nanosleep (&sleeper, &dummy) ;
}


static int cc2530_leave_debug(void)
{
    gpio_set_value(RST_GPIO, 0);
    delay(100);
    gpio_set_value(RST_GPIO, 1);
    return 0;
}


/*
 * Hold reset low while raising clock twice
 */


static int cc2530_enter_debug(void)
{
	// Cleanup
    cc2530_leave_debug();

	/*  Debug mode is entered by forcing two falling-edge transitions
		on pin P2.2 (debug clock) while the RESET_N input is held low.
		When RESET_N is set high, the device is in debug mode. */

	// Start from CLOCK high
    gpio_set_value(CCLK_GPIO, 1);
    gpio_set_value(DATA_GPIO, 0);
    delay(10);


	// Put RESET to low
    gpio_set_value(RST_GPIO, 0);
    delay(10);

	// First CLOCK falling edge
    gpio_set_value(CCLK_GPIO, 0);
    delay(10);
    gpio_set_value(CCLK_GPIO, 1);
    delay(10);

	// Second CLOCK falling edge
    gpio_set_value(CCLK_GPIO, 0);
    delay(10);

	// Return Reset to high
    gpio_set_value(RST_GPIO, 1);
    delay(10);

    debug_enabled = 1;

    return 0;
}


/*
 * Bit-bang a byte on the GPIO data line
 */
static inline void send_byte(unsigned char byte)
{
    int i;

    /* Data is driven on the positive edge of the debug clock and sampled on the negative edge.*/
    for (i = 7; i >= 0; i--) {
		// Positive edge
        gpio_set_value(CCLK_GPIO, 1);

        // Set data
        if (byte & (1 << i))
            gpio_set_value(DATA_GPIO, 1);
        else
            gpio_set_value(DATA_GPIO, 0);

		// Let the receiver to see the edge
        delay_ns(CLOCK_PULSE_NS);

		// Negative edge
        gpio_set_value(CCLK_GPIO, 0);

		// Let the receiver to sample the data
        delay_ns(CLOCK_PULSE_NS);
    }
}

/*
 * Clock in a byte from the GPIO line
 */
static inline void read_byte(unsigned char *byte)
{
    int i;
    bool val;
    *byte = 0;

    /* Data is driven on the positive edge of the debug clock and sampled on the negative edge.*/
    for (i = 7; i >= 0; i--) {
		// Positive edge
        gpio_set_value(CCLK_GPIO, 1);

		// Let the receiver to set the data
        delay_ns(CLOCK_PULSE_NS);

		// Negative edge
        gpio_set_value(CCLK_GPIO, 0);

		// Sample data bit
        gpio_get_value(DATA_GPIO, &val);

		// Wait before next pulse
        delay_ns(CLOCK_PULSE_NS);

		// Accumulate bit
        if (val)
            *byte |= (1 << i);
    }
}


/*
 * Send the command to the chip
 */
static int cc2530_do_cmd(const struct cc2530_cmd *cmd, unsigned char *params, unsigned char *outbuf)
{
	int ret;
	int bytes;
	bool val;
	unsigned char *answer;

	if (!cmd) {
		fprintf(stderr, "invalid command\n");
		return -1;
	}

	/* allocate as many bytes as we need to store the result */
	answer = malloc(cmd->out);
	if (!answer) {
		perror("malloc");
		return -1;
	}
	memset(answer, 0, cmd->out);

	ret = gpio_set_direction(DATA_GPIO, GPIO_DIRECTION_OUT);
	if (ret) {
		fprintf(stderr, "failed to put gpio in output direction\n");
		goto out_exit;
	}

	/*
	 * Debug instruction also needs to set the number of bytes
	 * of the sent instruction.
	 */
	if (cmd->id == CMD_DBG_INST)
		send_byte(cmd->id | cmd->in);
	else
		send_byte(cmd->id);

	/* If there is any command payload also send it */
	for (bytes = 0; bytes < cmd->in; bytes++)
		send_byte(params[bytes]);

	/* Now change the pin direction and wait for the chip to be ready
	 * and sample the data pin until the chip is ready to answer */
	ret = gpio_set_direction(DATA_GPIO, GPIO_DIRECTION_IN);
	if (ret) {
		fprintf(stderr, "failed to put back gpio in input direction\n");
		goto out_exit;
	}

    if (wait_chip_ready() != 0) {
		goto out_exit;
	}

	/* Now read the answer */
	for (bytes = 0; bytes < cmd->out; bytes++)
		read_byte(&answer[bytes]);

	memcpy(outbuf, answer, cmd->out);
out_exit:
	free(answer);
	return ret;
}

int wait_chip_ready(void)
{
	unsigned int timeout = DEFAULT_TIMEOUT;
	unsigned char dummy;
    bool val = 1;
	/*
	 * Cope with commands requiring a response delay and those which do
	 * not. In case the data line was not low right after we wrote data to
	 * clock out the chip 8 times as per the specification mentions. The
	 * data line should then be low and we are ready to read out from the
	 * chip.
	 */
	while (timeout--) {
		// Wait for pad direction change
		delay_ns(DIR_CHANGE_NS);
		// Check if chip is ready to send result
		gpio_get_value(DATA_GPIO, &val);
		if (val == 0) break;
		// When result is not ready, do dummy byte read
		read_byte(&dummy);
		if ((dummy|1) != 0xFF) printf("Suddenly chip returned something despite it was not ready: %02x\n", dummy);
        // Avoid extra wait when chip sets data low on last clock front
        if ((dummy & 1) == 0) break;
	}

	if (!timeout) {
		fprintf(stderr, "timed out waiting for chip to be ready again\n");
        return -1;
    }

    return 0;
}

static int set_dptr(struct cc2530_cmd *cmd, uint16_t addr);
static int set_acc(struct cc2530_cmd *cmd, uint8_t value);

/*
 * Bypass the command sending infrastructure to allow
 * direct access to the what is sent to the chip
 */
static int cc2530_burst_write(void)
{
	int ret;
	uint16_t i;
	unsigned char result;
	bool val;

	ret = gpio_set_direction(DATA_GPIO, GPIO_DIRECTION_OUT);
	if (ret) {
		fprintf(stderr, "failed to put gpio in output direction\n");
		return ret;
	}

	send_byte(CMD_BURST_WR | HIBYTE(PROG_BLOCK_SIZE));
	send_byte(LOBYTE(PROG_BLOCK_SIZE));

	for (i = 0; i < PROG_BLOCK_SIZE; i++) {
		send_byte(get_next_flash_byte());
    }

	ret = gpio_set_direction(DATA_GPIO, GPIO_DIRECTION_IN);
	if (ret) {
		fprintf(stderr, "failed to put gpio in input direction\n");
		return ret;
	}

	if (wait_chip_ready() != 0) {
		return -1;
	}

	read_byte(&result);

	return 0;
}

static int cc2530_chip_erase(struct cc2530_cmd *cmd)
{
	int ret;
	unsigned char result;
	unsigned int timeout = DEFAULT_TIMEOUT;

	cmd = find_cmd_by_name("erase");
	ret = cc2530_do_cmd(cmd, NULL, &result);
	if (ret) {
		fprintf(stderr, "%s: failed to issue: %s\n", __func__, cmd->name);
		return ret;
	}

	cmd = find_cmd_by_name("read_status");
	do {
		ret = cc2530_do_cmd(cmd, NULL, &result);
		if (ret) {
			fprintf(stderr, "%s: failed to issue: %s\n", __func__, cmd->name);
			return ret;
		}
		usleep(10);
	} while ((result & CHIP_ERASE_BSY) && timeout--);

	if (!timeout) {
		fprintf(stderr, "timeout waiting for the chip to be erased\n");
		return -1;
	}

	return 0;
}


static int write_sfr(struct cc2530_cmd *cmd, uint8_t sfr, uint8_t value) {
	unsigned char instr[3];
	unsigned char result;
	int ret;
    instr[0] = 0x75;
    instr[1] = sfr;
    instr[2] = value;
    cmd->in = sizeof(instr);

    ret = cc2530_do_cmd(cmd, instr, &result);
    if (ret) {
        fprintf(stderr, "%s: write sfr command failed\n", __func__);
    }
    return ret;
}

static int set_dptr(struct cc2530_cmd *cmd, uint16_t addr) {
	unsigned char instr[3];
	unsigned char result;
	int ret;
    instr[0] = 0x90;
    instr[1] = HIBYTE(addr);
    instr[2] = LOBYTE(addr);
    cmd->in = sizeof(instr);

    ret = cc2530_do_cmd(cmd, instr, &result);
    if (ret) {
        fprintf(stderr, "%s: set dptr command failed\n", __func__);
    }
    return ret;
}

static int copy_acc_to_dptr(struct cc2530_cmd *cmd) {
	unsigned char instr[1];
	unsigned char result;
	int ret;
    instr[0] = 0xF0;
    cmd->in = 1;

    ret = cc2530_do_cmd(cmd, instr, &result);
    if (ret) {
        fprintf(stderr, "failed to issue MOVX A, @DPTR\n");
    }
    return ret;
}

static int read_at_dptr(struct cc2530_cmd *cmd, uint8_t* result) {
	unsigned char instr[1];
	int ret;
    instr[0] = 0xE0;
    cmd->in = 1;

    ret = cc2530_do_cmd(cmd, instr, result);
    if (ret) {
        fprintf(stderr, "failed to issue MOVX A, @DPTR\n");
    }
    return ret;
}

static int inc_dptr(struct cc2530_cmd *cmd) {
	unsigned char instr[1];
	unsigned char result;
	int ret;
    instr[0] = 0xA3;
    cmd->in = 1;

    ret = cc2530_do_cmd(cmd, instr, &result);
    if (ret) {
        fprintf(stderr, "failed to INC DPTR\n");
    }
    return ret;
}

static int set_acc(struct cc2530_cmd *cmd, uint8_t value) {
	unsigned char instr[2];
	unsigned char result;
	int ret;

    instr[0] = 0x74;
    instr[1] = value;
    cmd->in = 2;

    ret = cc2530_do_cmd(cmd, instr, &result);
    if (ret) {
        fprintf(stderr, "failed to set acc value\n");
    }
    return ret;
}




static int cc2530_read_xdata_memory(struct cc2530_cmd *cmd, uint16_t addr, unsigned char *result)
{
	int ret;
	uint16_t i;

	cmd = find_cmd_by_name("debug_inst");

	ret = set_dptr(cmd, addr);
	if (ret) { return ret; }

	ret = read_at_dptr(cmd, result);
	if (ret) { return ret; }

	return 0;
}

static int cc2530_read_xdata_memory_block(struct cc2530_cmd *cmd,
				uint16_t addr, uint8_t *buffer, uint16_t num_bytes)
{
	int ret;
	uint16_t i;

	cmd = find_cmd_by_name("debug_inst");

	ret = set_dptr(cmd, addr);
	if (ret) { return ret; }

	for (i = 0; i < num_bytes; i++) {
        ret = read_at_dptr(cmd, &buffer[i]);
        if (ret) { return ret; }

		ret = inc_dptr(cmd);
		if (ret) { return ret; }
    }

	return 0;
}


static int cc2530_write_xdata_memory(struct cc2530_cmd *cmd, uint16_t addr, uint8_t value)
{
	int ret;
	uint16_t i;

	cmd = find_cmd_by_name("debug_inst");

	ret = set_dptr(cmd, addr);
	if (ret) { return ret; }

    ret = set_acc(cmd, value);
    if (ret) { return ret; }

    ret = copy_acc_to_dptr(cmd);
    if (ret) { return ret; }

	return 0;
}


static int cc2530_write_xdata_memory_block(struct cc2530_cmd *cmd,
				uint16_t addr, const uint8_t *values, uint16_t num_bytes)
{
	int ret;
	uint16_t i;

	cmd = find_cmd_by_name("debug_inst");

	ret = set_dptr(cmd, addr);
	if (ret) { return ret; }

	for (i = 0; i < num_bytes; i++) {
		ret = set_acc(cmd, values[i]);
		if (ret) { return ret; }

		ret = copy_acc_to_dptr(cmd);
		if (ret) { return ret; }

		ret = inc_dptr(cmd);
		if (ret) { return ret; }
    }

	return 0;
}


// Read block from flash. Assume caller provides a range in single bank
static int read_flash_block(struct cc2530_cmd *cmd,
				uint32_t addr, uint8_t *buffer, uint16_t num_bytes)
{
    uint8_t bank = addr / FLASH_BANK_SIZE;
    uint16_t xaddr = (addr % FLASH_BANK_SIZE) | 0x8000;
	int ret;

	cmd = find_cmd_by_name("debug_inst");

    //printf("read_flash_block @%06X: bank %d, addr %04X\n", addr, bank, xaddr);
    ret = write_sfr(cmd, MEMCTR, bank);
    if (ret) { return ret; }

	return cc2530_read_xdata_memory_block(cmd, xaddr, buffer, num_bytes);
}


static uint32_t cc2530_flash_verify(struct cc2530_cmd *cmd, uint32_t max_addr)
{
	uint8_t bank;
	unsigned char instr[3];
	unsigned char result;
	unsigned char expected;
	uint16_t i;
	uint32_t addr = 0;
	int ret;

	for (bank = 0; bank < 8; bank++) {
		if (verbose)
			printf("Reading bank: %d\n", bank);

		ret = cc2530_write_xdata_memory(cmd, X_MEMCTR, bank);
		if (ret) {
			fprintf(stderr, "%s: failed to write to X_MEMCTR\n", __func__);
			return ret;
		}

        cmd = find_cmd_by_name("debug_inst");

        ret = set_dptr(cmd, 0x8000);
        if (ret) { return ret; }

		for (i = 0; i < 32*1024; i++) {
			if (addr == max_addr)
				return addr;

            read_at_dptr(cmd, &result);
			if (ret) { return ret; }

			expected = get_next_flash_byte();
			if (result != expected) {
				printf("[bank%d][%d], result: %02x, expected: %02x\n",
						bank, i, result, expected);
			}

            inc_dptr(cmd);
			if (ret) { return ret; }

			addr++;
		}
	}

	return addr;
}

static uint8_t cc2530_program_flash(struct cc2530_cmd *cmd, uint16_t num_buffers)
{
	uint8_t dbg_arm, flash_arm;
	uint8_t max_speed = 1;
	uint16_t i;
	uint8_t wait;
	unsigned char result;
	unsigned char bank;
	int ret;
	unsigned int timeout = DEFAULT_TIMEOUT;

	/* Write the 4 DMA descriptors */
	ret = cc2530_write_xdata_memory_block(cmd, ADDR_DMA_DESC, dma_desc, ARRAY_SIZE(dma_desc));
	if (ret) {
		fprintf(stderr, "%s: failed to write DMA descriptors\n", __func__);
		return ret;
	}

	/* Set the pointer to the DMA descriptors */
	ret = cc2530_write_xdata_memory(cmd, X_DMA1CFGH, HIBYTE(ADDR_DMA_DESC));
	if (ret) {
		fprintf(stderr, "%s: failed to set DMA descriptors (part 1)\n", __func__);
		return ret;
	}
	ret = cc2530_write_xdata_memory(cmd, X_DMA1CFGL, LOBYTE(ADDR_DMA_DESC));
	if (ret) {
		fprintf(stderr, "%s: failed to set DMA descriptors (part 2)\n", __func__);
		return ret;
	}

	/* Make sure FADDR is 0x0000 */
	ret = cc2530_write_xdata_memory(cmd, FADDRH, 0);
	if (ret) {
		fprintf(stderr, "%s: failed to set FADDRH\n", __func__);
		return ret;
	}
	ret = cc2530_write_xdata_memory(cmd, FADDRL, 0);
	if (ret) {
		fprintf(stderr, "%s: failed to set FADDRL\n", __func__);
		return ret;
	}

	for (i = 0; i < num_buffers; i++) {
		if (progress) {
			printf("%d/%d\n", i, num_buffers - 1);
			fflush(stdout);
		}
		/* Set what is to be written to DMAARM, based on loop iteration */
		if ((i & 0x0001) == 0) {
			dbg_arm = CH_DBG_TO_BUF0;
			flash_arm = CH_BUF0_TO_FLASH;
		} else {
			dbg_arm = CH_DBG_TO_BUF1;
			flash_arm = CH_BUF1_TO_FLASH;
		}

		/* transfer next buffer (first buffer when i == 0) */
		ret = cc2530_write_xdata_memory(cmd, X_DMAARM, dbg_arm);
		if (ret) {
			fprintf(stderr, "%s: failed to arm DMA\n", __func__);
			return ret;
		}
		cc2530_burst_write();

		/* wait for write to finish */
		wait = 0;
		do  {
			ret = cc2530_read_xdata_memory(cmd, FCTL, &result);
			if (ret) {
				fprintf(stderr, "%s: failed at %i\n", __func__, i);
				return ret;
			}
			wait = 1;
		} while ((result & FCTL_BUSY) && timeout--);

		if (!timeout) {
			fprintf(stderr, "%s: timeout at %i\n", __func__, i);
			return -1;
		}

		/* no waiting means programming finished before burst write */
		if (i > 0 && wait == 0)
			max_speed = 0;

		/* start programming current buffer */
		ret = cc2530_write_xdata_memory(cmd, X_DMAARM, flash_arm);
		if (ret) {
			fprintf(stderr, "%s: failed programming current buffer: %d\n", __func__, i);
			return ret;
		}
		ret = cc2530_write_xdata_memory(cmd, FCTL, 0x06);
		if (ret) {
			fprintf(stderr, "%s: failed to set FCTL\n", __func__);
			return ret;
		}
	}

	timeout = DEFAULT_TIMEOUT;

	/* Programming last buffer in progress, wait until done */
	do {
		ret = cc2530_read_xdata_memory(cmd, FCTL, &result);
		if (ret) {
			fprintf(stderr, "%s: failed\n", __func__);
			return ret;
		}
	} while ((result & FCTL_BUSY) && timeout--);

	if (!timeout) {
		fprintf(stderr, "%s: timeout programming last buffer\n", __func__);
		return -1;
	}

	return max_speed;
}

void println_hex(uint8_t * values, int size) {
	int i;
    for (i = 0; i < size - 1; i++) printf("%02x:", values[i]);
    printf("%02x\n", values[i]);
}

static int cc2530_chip_identify(struct cc2530_cmd *cmd, int *flash_size)
{
	int ret = 0;
	unsigned char result[2] = { 0 };
	unsigned char mac_addr[8] = { 0 };
	unsigned char extra_data[24] = { 0 };
	int i;
    unsigned int mac_location; int mac_size = 8;

	ret = gpio_set_direction(DATA_GPIO, GPIO_DIRECTION_OUT);
	if (ret) {
		fprintf(stderr, "failed to set data gpio direction\n");
		return ret;
	}

	cmd = find_cmd_by_name("get_chip_id");
	ret = cc2530_do_cmd(cmd, NULL, result);
	if (ret) {
		fprintf(stderr, "%s: failed to issue: %s\n", __func__, cmd->name);
		goto out;
	}

	/* Check that we actually know that chip */
	if ((result[0] != CC2530_ID) && (result[0] != CC2541_ID)) {
		fprintf(stderr, "unknown Chip ID: %02x\n", result[0]);
		if (result[0] == 0xFF || result[0] == 0)
			fprintf(stderr, "someone is holding the CLK/DATA lines against us "
					"make sure no debugger is *connected*\n");
		ret = -EINVAL;
		goto out;
	}
    if (result[0] == CC2530_ID) {
        mac_location = 0x780C;
        mac_size = 8;
    } else {
        mac_location = 0x780E;
        mac_size = 6;
    }

	if (verbose)
        printf("Texas Instruments CC2530 (ID: 0x%02x, rev 0x%02x)\n", result[0], result[1]);

    // We need to halt the chip before sending debug instructions
	cmd = find_cmd_by_name("halt");
	ret = cc2530_do_cmd(cmd, NULL, result);
	if (ret) { fprintf(stderr, "%s: failed to issue: %s\n", __func__, cmd->name); goto out; }
	if (verbose) {
        printf("status halt returns: %02x\n", result[0]);
    }

	/*
	 * Do some chip identification
	 */
	ret = cc2530_read_xdata_memory(cmd, X_CHIPINFO0, result);
	if (ret) {
		fprintf(stderr, "failed to read X_CHIPINFO0 register\n");
		return ret;
	}

	if (verbose) {
		printf("Chip Info 0: %02x\n", result[0]);
		if (result[0] & 8)
			printf("USB available\n");
		else
			printf("USB: not availabe\n");
	}

	switch ((result[0] & 0x70) >> 4) {
	case 1:
		*flash_size = 32;
		break;
	case 2:
		*flash_size = 64;
		break;
	case 3:
		*flash_size = 128;
		break;
	case 4:
		*flash_size = 256;
	}

	if (verbose)
		printf("Flash size: %d KB\n", *flash_size);

	*flash_size *= 1024;

	ret = cc2530_read_xdata_memory(cmd, X_CHIPINFO1, result);
	if (ret) {
		fprintf(stderr, "failed to read X_CHIPINFO1 register\n");
		return ret;
	}

	for (i = 0; i < mac_size; i++) {
		ret = cc2530_read_xdata_memory(cmd, mac_location + i, result);
		if (ret) {
			fprintf(stderr, "%s: failed to read Mac addr %d\n", __func__, i);
			return ret;
		}
		mac_addr[i] = result[0];
	}

	if (verbose) {
		printf("Mac addr: ");
        println_hex(mac_addr, mac_size);
    }

    ret = read_flash_block(cmd, *flash_size - 16 - mac_size, extra_data, 16 + mac_size);
    if (ret) { return ret; }

	if (verbose) {
		printf("Mac2 addr (@%06X): ", *flash_size - 16 - mac_size);
        println_hex(extra_data, mac_size);

		printf("Lock data (@%06X): ", *flash_size - 16);
        println_hex(extra_data + mac_size, 16);
    }
    
    ret = cc2530_write_xdata_memory(cmd, X_MEMCTR, 0);
    if (ret) {
        fprintf(stderr, "%s: failed to write to X_MEMCTR\n", __func__);
        return ret;
    }

out:
	return ret;
}

/*
 * Perform full CC2530 chip initialization and programming
 */
static int cc2530_do_program(struct cc2530_cmd *cmd, off_t fwsize, unsigned do_readback)
{
	int ret = 0;
	unsigned char config;
	unsigned char result;
	uint32_t num_bytes_ok = 0;
	uint16_t blocks;
	unsigned int timeout = DEFAULT_TIMEOUT;
	unsigned int retry_cnt = 3;

	while (retry_cnt--) {
		/* Enable DMA */
		cmd = find_cmd_by_name("write_config");
		config = 0x22;
		ret = cc2530_do_cmd(cmd, &config, &result);
		if (ret) {
			fprintf(stderr, "failed to enable DMA\n");
			return ret;
		}

		if (result != config) {
			fprintf(stderr, "write config failed (retry count: %d)\n", retry_cnt);

			cc2530_enter_debug();
			continue;
		}

		break;
	}

	ret = cc2530_write_xdata_memory(cmd, X_CLKCONCMD, 0x80);
	if (ret) {
		fprintf(stderr, "failed to write X_CLKCONCMD\n");
		return ret;
	}

	do {
		ret = cc2530_read_xdata_memory(cmd, X_CLKCONSTA, &result);
		if (ret) {
			fprintf(stderr, "%s: failed to read X_CLKCONSTA\n", __func__);
			return ret;
		}
	} while ((result != 0x80) && timeout--);

	if (!timeout) {
		fprintf(stderr, "%s: timeout waiting for CLKCONSTA\n", __func__);
		return -1;
	}

	ret = cc2530_chip_erase(cmd);
	if (ret) {
		fprintf(stderr, "failed to erase chip\n");
		return ret;
	}

	init_flash_ptr();

	blocks = DIV_ROUND_UP(fwsize, PROG_BLOCK_SIZE);

	ret = cc2530_program_flash(cmd, blocks);
	if (ret && verbose)
		printf("Programmed at maximum speed\n");

	if (!do_readback)
		goto cc2530_reset_mcu;

	init_flash_ptr();

	num_bytes_ok = cc2530_flash_verify(cmd, blocks * PROG_BLOCK_SIZE);
	if (num_bytes_ok == (blocks * PROG_BLOCK_SIZE)) {
		if (verbose)
			printf("Verification OK\n");
		goto cc2530_reset_mcu;
	} else
		if (verbose)
			printf("Verification failed\n");

cc2530_reset_mcu:
	cc2530_leave_debug();

	return 0;
}

static int cc2530_oneshot_command(struct cc2530_cmd *cmd, const char *command)
{
	int ret;
	unsigned char result;

	cmd = find_cmd_by_name(command);
	if (!cmd) {
		fprintf(stderr, "unknown command: %s\n", command);
		return -1;
	}

	ret = cc2530_do_cmd(cmd, NULL, &result);
	if (ret)
		return -1;

	printf("result: %02x\n", result);

	return 0;
}


static void usage(void)
{
	printf("Usage: cc2530prog [options]\n"
		"\t-v:     verbose\n"
		"\t-i:     identify device\n"
		"\t-P:     show progress\n"
		"\t-f:     firmware file\n"
		"\t-r:     perform readback\n"
		"\t-c:     single command to send\n"
		"\t-l:     list available commands\n");
	exit(-1);
}

int main(int argc, char **argv)
{
	int opt, ret = 0;
	const char *firmware = NULL;
	unsigned do_readback = 0;
	unsigned do_list = 0;
	unsigned do_identify = 0;
	char *command = NULL;
	struct cc2530_cmd *cmd = NULL;
	int f;
	off_t fwsize;
	struct stat buf;
	int flash_size = 0;
	unsigned int retry_cnt = 3;

	while ((opt = getopt(argc, argv, "f:rlc:ivP")) > 0) {
		switch (opt) {
		case 'f':
			firmware = optarg;
			break;
		case 'r':
			do_readback = 1;
			break;
		case 'l':
			do_list = 1;
			break;
		case 'c':
			command = optarg;
			break;
		case 'i':
			do_identify = 1;
			verbose = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'P':
			progress = 1;
			break;
		default:
			break;
		}
	}

	argc -= optind;
	argc += optind;

	if (argc < 2)
		usage();

	if (cc2530_gpio_init()) {
		fprintf(stderr, "failed to initialize GPIOs\n");
		return -1;
	}

	if (!debug_enabled)
		cc2530_enter_debug();

	if (do_identify) {
		ret = cc2530_chip_identify(cmd, &flash_size);
		if (ret)
			fprintf(stderr, "failed to identify chip\n");

		goto out;
	}

	if (command) {
		cc2530_oneshot_command(cmd, command);
		goto out;
	}

	if (do_list) {
		cc2530_show_command_list();
		goto out;
	}

	if (stat(firmware, &buf) < 0) {
		perror("stat");
		goto out;
	}

	if (!S_ISREG(buf.st_mode)) {
		fprintf(stderr, "%s is not a regular file\n", firmware);
		goto out;
	}

	fwsize = buf.st_size;

	while (retry_cnt--) {
		ret = cc2530_chip_identify(cmd, &flash_size);
		if (ret) {
			fprintf(stderr, "failed to identify chip\n");
			continue;
		}

		break;
	}

	if (!retry_cnt) {
		fprintf(stderr, "timeout identifying the chip\n");
		goto out;
	}

	if (fwsize > flash_size) {
		fprintf(stderr, "firmware file too big: %ld (max: %d)\n",
						fwsize, flash_size);
		goto out;
	}

	f = open(firmware, O_RDONLY);
	if (!f) {
		fprintf(stderr, "cannot open firmware: %s\n", firmware);
		goto out;
	}

	if (verbose)
		printf("Using firmware file: %s (%ld bytes)\n", firmware, fwsize);

	fwdata = malloc(fwsize);
	if (!fwdata) {
		perror("malloc");
		ret = -1;
		goto out;
	}

	if (read(f, fwdata, fwsize) < 0) {
		fprintf(stderr, "premature end of read\n");
		goto out_free;
	}

	if (cc2530_do_program(cmd, fwsize, do_readback)) {
		fprintf(stderr, "failed to program chip\n");
		ret = -1;
	}

out_free:
	free(fwdata);
	close(f);
out:
	cc2530_leave_debug();
	cc2530_gpio_deinit();
	return ret;
}
