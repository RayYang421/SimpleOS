#include "sd.h"
#include "mmio.h"
#include "gpio.h"
#include "timer.h"
#include "uart.h"
#include "string.h"

/* The Arasan EMMC host controller, which is where the SD card ends up once the
 * GPIO mux is told to put it there. Transfers are programmed I/O: 512 bytes is
 * 128 reads of the data register, and setting up a DMA descriptor would cost
 * more than it saves at this size.
 *
 * Everything here polls with a deadline rather than waiting forever, so a card
 * that never answers reports a failure instead of wedging the kernel. */

#define EMMC_BASE        (MMIO_BASE + 0x00300000)

#define EMMC_ARG2        (EMMC_BASE + 0x00)
#define EMMC_BLKSIZECNT  (EMMC_BASE + 0x04)
#define EMMC_ARG1        (EMMC_BASE + 0x08)
#define EMMC_CMDTM       (EMMC_BASE + 0x0C)
#define EMMC_RESP0       (EMMC_BASE + 0x10)
#define EMMC_RESP1       (EMMC_BASE + 0x14)
#define EMMC_RESP2       (EMMC_BASE + 0x18)
#define EMMC_RESP3       (EMMC_BASE + 0x1C)
#define EMMC_DATA        (EMMC_BASE + 0x20)
#define EMMC_STATUS      (EMMC_BASE + 0x24)
#define EMMC_CONTROL0    (EMMC_BASE + 0x28)
#define EMMC_CONTROL1    (EMMC_BASE + 0x2C)
#define EMMC_INTERRUPT   (EMMC_BASE + 0x30)
#define EMMC_INT_MASK    (EMMC_BASE + 0x34)
#define EMMC_INT_EN      (EMMC_BASE + 0x38)
#define EMMC_CONTROL2    (EMMC_BASE + 0x3C)
#define EMMC_SLOTISR_VER (EMMC_BASE + 0xFC)

/* CMDTM: the command index sits in the top byte and the rest describes what
 * the command does, so the host knows how long to wait and which way the data
 * moves. */
#define CMD_RSPNS_48     (2u << 16)
#define CMD_RSPNS_136    (1u << 16)
#define CMD_RSPNS_48B    (3u << 16)
#define CMD_CRCCHK_EN    (1u << 19)
#define CMD_IXCHK_EN     (1u << 20)
#define CMD_ISDATA       (1u << 21)
#define TM_DAT_CARD_TO_HOST (1u << 4)

#define CMD_GO_IDLE      0x00000000u
#define CMD_ALL_SEND_CID (0x02000000u | CMD_RSPNS_136 | CMD_CRCCHK_EN)
#define CMD_SEND_REL_ADDR (0x03000000u | CMD_RSPNS_48 | CMD_CRCCHK_EN)
#define CMD_CARD_SELECT  (0x07000000u | CMD_RSPNS_48B | CMD_CRCCHK_EN)
#define CMD_SEND_IF_COND (0x08000000u | CMD_RSPNS_48 | CMD_CRCCHK_EN)
#define CMD_SET_BLOCKLEN (0x10000000u | CMD_RSPNS_48 | CMD_CRCCHK_EN)
#define CMD_READ_SINGLE  (0x11000000u | CMD_RSPNS_48 | CMD_CRCCHK_EN | \
                          CMD_IXCHK_EN | CMD_ISDATA | TM_DAT_CARD_TO_HOST)
#define CMD_WRITE_SINGLE (0x18000000u | CMD_RSPNS_48 | CMD_CRCCHK_EN | \
                          CMD_IXCHK_EN | CMD_ISDATA)
#define CMD_APP_CMD      (0x37000000u | CMD_RSPNS_48 | CMD_CRCCHK_EN)
#define CMD_APP_CMD_RCA  (0x37000000u | CMD_RSPNS_48 | CMD_CRCCHK_EN)
#define CMD_SET_BUS_WIDTH (0x06000000u | CMD_RSPNS_48 | CMD_CRCCHK_EN)
#define CMD_SEND_OP_COND (0x29000000u | CMD_RSPNS_48)

/* STATUS */
#define SR_READ_AVAILABLE  0x00000800u
#define SR_WRITE_AVAILABLE 0x00000400u
#define SR_CMD_INHIBIT     0x00000001u
#define SR_DAT_INHIBIT     0x00000002u
#define SR_APP_CMD         0x00000020u

/* INTERRUPT */
#define INT_CMD_DONE   0x00000001u
#define INT_DATA_DONE  0x00000002u
#define INT_READ_RDY   0x00000020u
#define INT_WRITE_RDY  0x00000010u
#define INT_ERROR_MASK 0x017E8000u

/* CONTROL1 */
#define C1_CLK_INTLEN  0x00000001u
#define C1_CLK_STABLE  0x00000002u
#define C1_CLK_EN      0x00000004u
#define C1_TOUNIT_MAX  0x000e0000u
#define C1_SRST_HC     0x01000000u
#define C1_SRST_DATA   0x04000000u

/* Card status bits in an R1 response. */
#define ST_APP_CMD     0x00000020u

/* OCR bits used when asking the card what it can do. */
#define ACMD41_ARG_HC  0x51ff8000u

static uint32_t card_rca;
static int      card_ready;
static int      card_is_sdhc;
static uint64_t stat_reads, stat_writes;

static void delay_us(uint64_t us) {
    uint64_t end = timer_count() + (timer_freq() * us) / 1000000;
    while (timer_count() < end) { }
}

/* Spins until the status bits clear, or gives up. Everything that talks to the
 * controller goes through this, so a dead card costs a bounded wait. */
static int wait_status(uint32_t mask, uint64_t timeout_us) {
    uint64_t end = timer_count() + (timer_freq() * timeout_us) / 1000000;

    while (mmio_read(EMMC_STATUS) & mask) {
        if (timer_count() > end) return -1;
    }
    return 0;
}

/* Waits for one of the interrupt-status bits, treating the error bits as a
 * failure however the wait was going to end. */
static int wait_interrupt(uint32_t mask, uint64_t timeout_us) {
    uint64_t end = timer_count() + (timer_freq() * timeout_us) / 1000000;
    uint32_t want = mask | INT_ERROR_MASK;
    uint32_t status;

    while (((status = mmio_read(EMMC_INTERRUPT)) & want) == 0) {
        if (timer_count() > end) return -1;
    }

    if (status & INT_ERROR_MASK) {
        mmio_write(EMMC_INTERRUPT, status);     /* write to clear */
        return -1;
    }

    mmio_write(EMMC_INTERRUPT, mask);
    return 0;
}

/* Issues one command and returns its 32-bit response, or leaves *err set. */
static uint32_t sd_command(uint32_t code, uint32_t arg, int *err) {
    *err = 0;

    if (wait_status(SR_CMD_INHIBIT, 1000000) != 0) { *err = -1; return 0; }

    mmio_write(EMMC_INTERRUPT, mmio_read(EMMC_INTERRUPT));   /* clear stale */
    mmio_write(EMMC_ARG1, arg);
    mmio_write(EMMC_CMDTM, code);

    if (wait_interrupt(INT_CMD_DONE, 1000000) != 0) { *err = -1; return 0; }

    return mmio_read(EMMC_RESP0);
}

/* An application command is two commands: CMD55 to say one is coming, then the
 * command itself. */
static uint32_t sd_app_command(uint32_t code, uint32_t arg, int *err) {
    sd_command(card_rca ? CMD_APP_CMD_RCA : CMD_APP_CMD,
               card_rca << 16, err);
    if (*err) return 0;

    return sd_command(code, arg, err);
}

/* The controller divides the base clock; the divider is split across two
 * fields, which is why it is assembled rather than just written. */
static int sd_set_clock(uint32_t freq) {
    uint32_t base = 41666666;                   /* the EMMC base clock */
    uint32_t divisor = 1;

    while ((base / divisor) > freq && divisor < 0x3FF) divisor++;

    /* The SD spec's "10-bit divided clock mode": the low 8 bits of the divider
     * go in one field and the top 2 in another. */
    uint32_t lo = (divisor & 0xFF) << 8;
    uint32_t hi = ((divisor >> 8) & 0x3) << 6;

    if (wait_status(SR_CMD_INHIBIT | SR_DAT_INHIBIT, 1000000) != 0) return -1;

    uint32_t c1 = mmio_read(EMMC_CONTROL1) & ~C1_CLK_EN;
    mmio_write(EMMC_CONTROL1, c1);
    delay_us(10);

    c1 = (mmio_read(EMMC_CONTROL1) & 0xffff003f) | lo | hi;
    mmio_write(EMMC_CONTROL1, c1);
    delay_us(10);

    mmio_write(EMMC_CONTROL1, mmio_read(EMMC_CONTROL1) | C1_CLK_EN);
    delay_us(10);

    uint64_t end = timer_count() + timer_freq();        /* one second */
    while (!(mmio_read(EMMC_CONTROL1) & C1_CLK_STABLE)) {
        if (timer_count() > end) return -1;
    }
    return 0;
}

/* Routes the card to this controller. On a Pi the same pins can be driven by
 * either host, and which one gets them is a GPIO alternate function. */
static void sd_gpio_setup(void) {
    uint32_t r;

    /* GPIO 47 is card detect: an input, pulled up. */
    r = mmio_read(GPFSEL4);
    r &= ~(7u << (7 * 3));
    mmio_write(GPFSEL4, r);

    /* GPIO 48-53 to ALT3, which is the EMMC controller. */
    r = mmio_read(GPFSEL4);
    r &= ~((7u << (8 * 3)) | (7u << (9 * 3)));
    r |=  ((7u << (8 * 3)) | (7u << (9 * 3)));      /* ALT3 is 0b111 */
    mmio_write(GPFSEL4, r);

    r = mmio_read(GPFSEL5);
    r &= ~((7u << 0) | (7u << 3) | (7u << 6) | (7u << 9));
    r |=  ((7u << 0) | (7u << 3) | (7u << 6) | (7u << 9));
    mmio_write(GPFSEL5, r);

    /* Pull-ups on the data and command lines. */
    mmio_write(GPPUD, 2);
    delay_us(150);
    mmio_write(GPPUDCLK1, (1u << (47 - 32)) | (1u << (48 - 32)) |
                          (1u << (49 - 32)) | (1u << (50 - 32)) |
                          (1u << (51 - 32)) | (1u << (52 - 32)) |
                          (1u << (53 - 32)));
    delay_us(150);
    mmio_write(GPPUD, 0);
    mmio_write(GPPUDCLK1, 0);
}

int sd_init(void) {
    int err;

    card_ready = 0;
    card_rca   = 0;

    sd_gpio_setup();

    /* Reset the host controller and start its clock. */
    mmio_write(EMMC_CONTROL0, 0);
    mmio_write(EMMC_CONTROL1, mmio_read(EMMC_CONTROL1) | C1_SRST_HC);

    uint64_t end = timer_count() + timer_freq();
    while (mmio_read(EMMC_CONTROL1) & C1_SRST_HC) {
        if (timer_count() > end) {
            uart_puts("sd: the host controller would not reset\n");
            return -1;
        }
    }

    mmio_write(EMMC_CONTROL1,
               mmio_read(EMMC_CONTROL1) | C1_CLK_INTLEN | C1_TOUNIT_MAX);
    delay_us(10);

    /* Identification runs at 400 kHz; the card is not told to go faster until
     * it has been addressed. */
    if (sd_set_clock(400000) != 0) {
        uart_puts("sd: the clock would not settle\n");
        return -1;
    }

    mmio_write(EMMC_INT_EN, 0xffffffff);
    mmio_write(EMMC_INT_MASK, 0xffffffff);

    sd_command(CMD_GO_IDLE, 0, &err);
    if (err) { uart_puts("sd: no card responded to GO_IDLE\n"); return -1; }

    /* CMD8 tells a v2 card the voltage on offer; the card echoes the check
     * pattern back if it understood. */
    sd_command(CMD_SEND_IF_COND, 0x000001AA, &err);
    if (err) { uart_puts("sd: the card did not accept the voltage\n"); return -1; }

    /* ACMD41 until the card has finished powering up. */
    uint32_t ocr = 0;
    end = timer_count() + timer_freq() * 2;

    for (;;) {
        ocr = sd_app_command(CMD_SEND_OP_COND, ACMD41_ARG_HC, &err);

        /* Bit 31 is "initialisation complete". */
        if (!err && (ocr & 0x80000000)) break;

        if (timer_count() > end) {
            uart_puts("sd: the card never finished initialising\n");
            return -1;
        }
        delay_us(1000);
    }

    /* Bit 30 says the card addresses blocks rather than bytes, which is what
     * every card larger than 2 GiB does. */
    card_is_sdhc = (ocr & 0x40000000) != 0;

    sd_command(CMD_ALL_SEND_CID, 0, &err);
    if (err) { uart_puts("sd: no CID\n"); return -1; }

    uint32_t rca = sd_command(CMD_SEND_REL_ADDR, 0, &err);
    if (err) { uart_puts("sd: no relative address\n"); return -1; }
    card_rca = (rca >> 16) & 0xFFFF;

    if (sd_set_clock(25000000) != 0) {
        uart_puts("sd: could not switch to the full clock\n");
        return -1;
    }

    sd_command(CMD_CARD_SELECT, card_rca << 16, &err);
    if (err) { uart_puts("sd: the card would not be selected\n"); return -1; }

    /* A byte-addressed card has to be told the block size; a block-addressed
     * one already works in 512-byte units. */
    if (!card_is_sdhc) {
        sd_command(CMD_SET_BLOCKLEN, SD_BLOCK_SIZE, &err);
        if (err) { uart_puts("sd: could not set the block length\n"); return -1; }
    }

    card_ready = 1;
    return 0;
}

int sd_present(void) { return card_ready; }

/* The card takes a block number or a byte offset depending on what it is. */
static uint32_t sd_address(uint32_t lba) {
    return card_is_sdhc ? lba : lba * SD_BLOCK_SIZE;
}

int readblock(uint32_t lba, void *buf) {
    int err;

    if (!card_ready || buf == 0) return -1;
    if (wait_status(SR_DAT_INHIBIT, 1000000) != 0) return -1;

    mmio_write(EMMC_BLKSIZECNT, (1u << 16) | SD_BLOCK_SIZE);
    sd_command(CMD_READ_SINGLE, sd_address(lba), &err);
    if (err) return -1;

    if (wait_interrupt(INT_READ_RDY, 1000000) != 0) return -1;

    /* The data register is a window on the controller's buffer: 128 reads
     * drain one block. */
    uint32_t *out = (uint32_t *)buf;
    for (int i = 0; i < SD_BLOCK_SIZE / 4; i++) out[i] = mmio_read(EMMC_DATA);

    if (wait_interrupt(INT_DATA_DONE, 1000000) != 0) return -1;

    stat_reads++;
    return 0;
}

int writeblock(uint32_t lba, const void *buf) {
    int err;

    if (!card_ready || buf == 0) return -1;
    if (wait_status(SR_DAT_INHIBIT, 1000000) != 0) return -1;

    mmio_write(EMMC_BLKSIZECNT, (1u << 16) | SD_BLOCK_SIZE);
    sd_command(CMD_WRITE_SINGLE, sd_address(lba), &err);
    if (err) return -1;

    if (wait_interrupt(INT_WRITE_RDY, 1000000) != 0) return -1;

    const uint32_t *in = (const uint32_t *)buf;
    for (int i = 0; i < SD_BLOCK_SIZE / 4; i++) mmio_write(EMMC_DATA, in[i]);

    if (wait_interrupt(INT_DATA_DONE, 1000000) != 0) return -1;

    stat_writes++;
    return 0;
}

void sd_stats(uint64_t *reads, uint64_t *writes) {
    if (reads)  *reads  = stat_reads;
    if (writes) *writes = stat_writes;
}
