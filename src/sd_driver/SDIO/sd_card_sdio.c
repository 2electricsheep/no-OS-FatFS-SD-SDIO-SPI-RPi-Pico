// Driver for accessing SD card in SDIO mode on RP2040.

#include "ZuluSCSI_platform.h"

#ifdef SD_USE_SDIO

#include <stdint.h>
#include <string.h>
//
#include <hardware/gpio.h>
#include <hardware/clocks.h>
//
#include "diskio.h"
#include "my_debug.h"
#include "portability.h"
#include "rp2040_sdio.h"
#include "rp2040_sdio.pio.h"  // build\build\rp2040_sdio.pio.h
#include "sd_card_constants.h"
#include "sd_card.h"
#include "SdioCard.h"
#include "util.h"

#define STATE sd_card_p->sdio_if_p->state

// #define azdbg(...)

//FIXME
#define azdbg(arg1, ...) {\
    DBG_PRINTF("%s,%d: %s\n", __func__, __LINE__, arg1); \
}

#define checkReturnOk(call) ((STATE.error = (call)) == SDIO_OK ? true : logSDError(sd_card_p, __LINE__))

static bool logSDError(sd_card_t *sd_card_p, int line)
{
    STATE.error_line = line;
    EMSG_PRINTF("SDIO SD card error on line %d error code %d\n", line, (int)STATE.error);
    return false;
}

/*
    CLKDIV is from sd_driver\SDIO\rp2040_sdio.pio

    baud = clk_sys / (CLKDIV * clk_div) 
    baud * CLKDIV * clk_div = clk_sys;
    clk_div = clk_sys / (CLKDIV * baud)
*/
static float calculate_clk_div(uint baud) {
    float div = (float)clock_get_hz(clk_sys) / (CLKDIV * baud);
    /* Baud rate cannot exceed clk_sys frequency divided by CLKDIV! */
    myASSERT(div >= 1 && div <= 65536);
    return div;
}

bool sd_sdio_begin(sd_card_t *sd_card_p)
{
    uint32_t reply;
    sdio_status_t status;
    
    // Initialize at 400 kHz clock speed
    if (!rp2040_sdio_init(sd_card_p, calculate_clk_div(400 * 1000)))
        return false; 

    // Establish initial connection with the card
    for (int retries = 0; retries < 5; retries++)
    {
        delay_ms(1);
        reply = 0;
        rp2040_sdio_command_R1(sd_card_p, CMD0_GO_IDLE_STATE, 0, NULL); // GO_IDLE_STATE
        status = rp2040_sdio_command_R1(sd_card_p, CMD8_SEND_IF_COND, 0x1AA, &reply); // SEND_IF_COND

        if (status == SDIO_OK && reply == 0x1AA)
        {
            break;
        }
    }

    if (reply != 0x1AA || status != SDIO_OK)
    {
        // azdbg("SDIO not responding to CMD8 SEND_IF_COND, status ", (int)status, " reply ", reply);
        EMSG_PRINTF("%s,%d SDIO not responding to CMD8 SEND_IF_COND, status 0x%x reply 0x%lx\n", 
            __func__, __LINE__, status, reply);
        return false;
    }

    // Send ACMD41 to begin card initialization and wait for it to complete
    uint32_t start = millis();
    do {
        if (!checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD55_APP_CMD, 0, &reply)) || // APP_CMD
            !checkReturnOk(rp2040_sdio_command_R3(sd_card_p, ACMD41_SD_SEND_OP_COND, 0xD0040000, &STATE.ocr))) // 3.0V voltage
            // !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, ACMD41, 0xC0100000, &STATE.ocr)))
        {
            return false;
        }

        if ((uint32_t)(millis() - start) > 1000)
        {
            EMSG_PRINTF("SDIO card initialization timeout\n");
            return false;
        }
    } while (!(STATE.ocr & (1 << 31)));

    // Get CID
    // CMD2 is valid only in "ready" state;
    // Transitions to "ident" state
    // Note: CMD10 is valid only in "stby" state
    if (!checkReturnOk(rp2040_sdio_command_R2(sd_card_p, CMD2_ALL_SEND_CID, 0, (uint8_t *)&sd_card_p->CID)))
    {
        azdbg("SDIO failed to read CID");
        return false;
    }

    // Get relative card address
    // Valid in "ident" or "stby" state; transitions to "stby"
    // Transitions from "card-identification-mode" to "data-transfer-mode"
    if (!checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD3_SEND_RELATIVE_ADDR, 0, &STATE.rca)))
    {
        azdbg("SDIO failed to get RCA");
        return false;
    }

    // Get CSD
    // Valid in "stby" state; stays in "stby" state
    if (!checkReturnOk(rp2040_sdio_command_R2(sd_card_p, CMD9_SEND_CSD, STATE.rca, sd_card_p->CSD)))
    {
        azdbg("SDIO failed to read CSD");
        return false;
    }
    sd_card_p->sectors = CSD_sectors(sd_card_p->CSD);

    // Select card
    // Valid in "stby" state; 
    // If card is addressed, transitions to "tran" state
    if (!checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD7_SELECT_CARD, STATE.rca, &reply)))
    {
        azdbg("SDIO failed to select card");
        return false;
    }

    /* At power up CD/DAT3 has a 50KOhm pull up enabled in the card. 
    This resistor serves two functions Card detection and Mode Selection. 
    For Mode Selection, the host can drive the line high or let it be pulled high to select SD mode. 
    If the host wants to select SPI mode it should drive the line low. 
    For Card detection, the host detects that the line is pulled high. 
    This pull-up should be disconnected by the user, during regular data transfer, 
    with SET_CLR_CARD_DETECT (ACMD42) command. */
    // Disconnect the 50 KOhm pull-up resistor on CD/DAT3
    // Valid in "tran" state; stays in "tran" state
    if (!checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD55_APP_CMD, STATE.rca, &reply)) ||
        !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, ACMD42_SET_CLR_CARD_DETECT, 0, &reply)))
    {
        azdbg("SDIO failed to disconnect pull-up");
        return false;
    }

    // Set 4-bit bus mode
    // Valid in "tran" state; stays in "tran" state
    if (!checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD55_APP_CMD, STATE.rca, &reply)) ||
        !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, ACMD6_SET_BUS_WIDTH, 2, &reply)))
    {
        azdbg("SDIO failed to set bus width");
        return false;
    }
    if (!checkReturnOk(rp2040_sdio_command_R1(sd_card_p, 16, 512, &reply))) // SET_BLOCKLEN
    {
        EMSG_PRINTF("%s,%d SDIO failed to set BLOCKLEN\n", __func__, __LINE__);
        return false;
    }
    // Increase to high clock rate
    if (!sd_card_p->sdio_if_p->baud_rate)
        sd_card_p->sdio_if_p->baud_rate = 10*1000*1000; // 10 MHz default
    if (!rp2040_sdio_init(sd_card_p, calculate_clk_div(sd_card_p->sdio_if_p->baud_rate)))
        return false; 

    return true;
}

uint8_t sd_sdio_errorCode(sd_card_t *sd_card_p) // const
{
    return STATE.error;
}

uint32_t sd_sdio_errorData() // const
{
    return 0;
}

uint32_t sd_sdio_errorLine(sd_card_t *sd_card_p) // const
{
    return STATE.error_line;
}

bool sd_sdio_isBusy(sd_card_t *sd_card_p) 
{
    // return (sio_hw->gpio_in & (1 << SDIO_D0)) == 0;
    return (sio_hw->gpio_in & (1 << sd_card_p->sdio_if_p->D0_gpio)) == 0;
}

uint32_t sd_sdio_kHzSdClk(sd_card_t *sd_card_p)
{
    return 0;
}

bool sd_sdio_readOCR(sd_card_t *sd_card_p, uint32_t* ocr)
{
    // SDIO mode does not have CMD58, but main program uses this to
    // poll for card presence. Return status register instead.
    return checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD13_SEND_STATUS, STATE.rca, ocr));
}

uint32_t sd_sdio_status(sd_card_t *sd_card_p)
{
    uint32_t reply;
    if (checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD13_SEND_STATUS, STATE.rca, &reply)))
        return reply;
    else
        return 0;
}

bool sd_sdio_stopTransmission(sd_card_t *sd_card_p, bool blocking)
{
    uint32_t reply;
    if (!checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD12_STOP_TRANSMISSION, 0, &reply)))
    {
        return false;
    }

    if (!blocking)
    {
        return true;
    }
    else
    {
        uint32_t start = millis();
        while (millis() - start < 200 && sd_sdio_isBusy(sd_card_p));
        if (sd_sdio_isBusy(sd_card_p))
        {
            EMSG_PRINTF("sd_sdio_stopTransmission() timeout\n");
            return false;
        }
        else
        {
            return true;
        }
    }
}

bool sd_sdio_syncDevice(sd_card_t *sd_card_p)
{
    return true;
}

uint8_t sd_sdio_type(sd_card_t *sd_card_p) // const
{
    if (STATE.ocr & (1 << 30))
        return SDCARD_V2HC;
    else
        return SDCARD_V2;
}


/* Writing and reading */

bool sd_sdio_writeSector(sd_card_t *sd_card_p, uint32_t sector, const uint8_t* src)
{
    if (((uint32_t)src & 3) != 0)
    {
        // Buffer is not aligned, need to memcpy() the data to a temporary buffer.
        memcpy(STATE.dma_buf, src, sizeof(STATE.dma_buf));
        src = (uint8_t*)STATE.dma_buf;
    }

    uint32_t reply;
    if (/* !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, 16, 512, &reply)) || // SET_BLOCKLEN */
        !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD24_WRITE_BLOCK, sector, &reply)) || // WRITE_BLOCK
        !checkReturnOk(rp2040_sdio_tx_start(sd_card_p, src, 1))) // Start transmission
    {
        return false;
    }

    do {
        uint32_t bytes_done;
        STATE.error = rp2040_sdio_tx_poll(sd_card_p, &bytes_done);
    } while (STATE.error == SDIO_BUSY);

    if (STATE.error != SDIO_OK)
    {
        EMSG_PRINTF("sd_sdio_writeSector(%lu) failed: %d\n", sector, (int)STATE.error);
    }

    return STATE.error == SDIO_OK;
}

bool sd_sdio_writeSectors(sd_card_t *sd_card_p, uint32_t sector, const uint8_t* src, size_t n)
{
    if (((uint32_t)src & 3) != 0)
    {
        // Unaligned write, execute sector-by-sector
        for (size_t i = 0; i < n; i++)
        {
            if (!sd_sdio_writeSector(sd_card_p, sector + i, src + 512 * i))
            {
                return false;
            }
        }
        return true;
    }

    uint32_t reply;
    if (/* !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, 16, 512, &reply)) || // SET_BLOCKLEN */
        !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD55_APP_CMD, STATE.rca, &reply)) || // APP_CMD
        !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, ACMD23_SET_WR_BLK_ERASE_COUNT, n, &reply)) || // SET_WR_CLK_ERASE_COUNT
        !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD25_WRITE_MULTIPLE_BLOCK, sector, &reply)) || // WRITE_MULTIPLE_BLOCK
        !checkReturnOk(rp2040_sdio_tx_start(sd_card_p, src, n))) // Start transmission
    {
        return false;
    }

    do {
        uint32_t bytes_done;
        STATE.error = rp2040_sdio_tx_poll(sd_card_p, &bytes_done);
    } while (STATE.error == SDIO_BUSY);

    if (STATE.error != SDIO_OK)
    {
        EMSG_PRINTF("sd_sdio_writeSectors(,%lu,,%zu) failed: %d\n", sector, n, (int)STATE.error);
        sd_sdio_stopTransmission(sd_card_p, true);
        return false;
    }
    else
    {
        return sd_sdio_stopTransmission(sd_card_p, true);
    }
}

bool sd_sdio_readSector(sd_card_t *sd_card_p, uint32_t sector, uint8_t* dst)
{
    uint8_t *real_dst = dst;
    if (((uint32_t)dst & 3) != 0)
    {
        // Buffer is not aligned, need to memcpy() the data from a temporary buffer.
        dst = (uint8_t*)STATE.dma_buf;
    }
    uint32_t reply;
    if (/* !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, 16, 512, &reply)) || // SET_BLOCKLEN */
        !checkReturnOk(rp2040_sdio_rx_start(sd_card_p, dst, 1, SDIO_BLOCK_SIZE)) || // Prepare for reception
        !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD17_READ_SINGLE_BLOCK, sector, &reply))) // READ_SINGLE_BLOCK
    {
        return false;
    }

    do {
        STATE.error = rp2040_sdio_rx_poll(sd_card_p, SDIO_WORDS_PER_BLOCK);
    } while (STATE.error == SDIO_BUSY);

    if (STATE.error != SDIO_OK)
    {
        EMSG_PRINTF("sd_sdio_readSector(,%lu,) failed: %d\n\n", sector, (int)STATE.error);
    }

    if (dst != real_dst)
    {
        memcpy(real_dst, STATE.dma_buf, sizeof(STATE.dma_buf));
    }

    return STATE.error == SDIO_OK;
}

bool sd_sdio_readSectors(sd_card_t *sd_card_p, uint32_t sector, uint8_t* dst, size_t n)
{
    if (((uint32_t)dst & 3) != 0 || sector + n >= sd_card_p->sectors)
    {
        // Unaligned read or end-of-drive read, execute sector-by-sector
        for (size_t i = 0; i < n; i++)
        {
            if (!sd_sdio_readSector(sd_card_p, sector + i, dst + 512 * i))
            {
                return false;
            }
        }
        return true;
    }

    uint32_t reply;
    if (/* !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, 16, 512, &reply)) || // SET_BLOCKLEN */
        !checkReturnOk(rp2040_sdio_rx_start(sd_card_p, dst, n, SDIO_BLOCK_SIZE)) || // Prepare for reception
        !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD18_READ_MULTIPLE_BLOCK, sector, &reply))) // READ_MULTIPLE_BLOCK
    {
        return false;
    }

    do {
        STATE.error = rp2040_sdio_rx_poll(sd_card_p, SDIO_WORDS_PER_BLOCK);
    } while (STATE.error == SDIO_BUSY);

    if (STATE.error != SDIO_OK)
    {
        EMSG_PRINTF("sd_sdio_readSectors(%ld,...,%d)  failed: %d\n", sector, n, STATE.error);
        sd_sdio_stopTransmission(sd_card_p, true);
        return false;
    }
    else
    {
        return sd_sdio_stopTransmission(sd_card_p, true);
    }
}

// Get 512 bit (64 byte) SD Status
bool rp2040_sdio_get_sd_status(sd_card_t *sd_card_p, uint8_t response[64]) {
    uint32_t reply;
    if (!checkReturnOk(rp2040_sdio_rx_start(sd_card_p, response, 1, 64)) || // Prepare for reception
        !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, CMD55_APP_CMD, STATE.rca, &reply)) ||  // APP_CMD
        !checkReturnOk(rp2040_sdio_command_R1(sd_card_p, ACMD13_SD_STATUS, 0, &reply))) // SD Status
    {
        EMSG_PRINTF("ACMD13 failed\n");
        return false;
    }
    // Read 512 bit block on DAT bus (not CMD)
    do {
        STATE.error = rp2040_sdio_rx_poll(sd_card_p, 64 / 4);
    } while (STATE.error == SDIO_BUSY);

    if (STATE.error != SDIO_OK)
    {
        EMSG_PRINTF("ACMD13 failed: %d\n", (int)STATE.error);
    }
    return STATE.error == SDIO_OK;

}

static bool sd_sdio_test_com(sd_card_t *sd_card_p) {
    bool success = false;

    if (!(sd_card_p->m_Status & STA_NOINIT)) {
        // SD card is currently initialized

        // Get status
        uint32_t reply = 0;
        sdio_status_t status = rp2040_sdio_command_R1(sd_card_p, CMD13_SEND_STATUS, STATE.rca, &reply);

        // Only care that communication succeeded
        success = (status == SDIO_OK);

        if (!success) {
            // Card no longer sensed - ensure card is initialized once re-attached
            sd_card_p->m_Status |= STA_NOINIT;
        }
    } else {
        // Do a "light" version of init, just enough to test com

        // Initialize at 400 kHz clock speed
        if (!rp2040_sdio_init(sd_card_p, calculate_clk_div(400 * 1000)))
            return false; 

        // Establish initial connection with the card
        rp2040_sdio_command_R1(sd_card_p, CMD0_GO_IDLE_STATE, 0, NULL); // GO_IDLE_STATE
        uint32_t reply = 0;
        sdio_status_t status = rp2040_sdio_command_R1(sd_card_p, CMD8_SEND_IF_COND, 0x1AA, &reply); // SEND_IF_COND

        success = (reply == 0x1AA && status == SDIO_OK);
    }

    return success;
}

// Helper function to configure whole GPIO in one line
static void gpio_conf(uint gpio, enum gpio_function fn, bool pullup, bool pulldown, bool output, bool initial_state, bool fast_slew)
{
    gpio_put(gpio, initial_state);
    gpio_set_dir(gpio, output);
    gpio_set_pulls(gpio, pullup, pulldown);
    gpio_set_function(gpio, fn);

    if (fast_slew)
    {
        padsbank0_hw->io[gpio] |= PADS_BANK0_GPIO0_SLEWFAST_BITS;
    }
}

static int sd_sdio_init(sd_card_t *sd_card_p) {
	//FIXME: Initialize lock?
    sd_lock(sd_card_p);

    // Make sure there's a card in the socket before proceeding
    sd_card_detect(sd_card_p);
    if (sd_card_p->m_Status & STA_NODISK) {
        sd_unlock(sd_card_p);
        return sd_card_p->m_Status;
    }
    // Make sure we're not already initialized before proceeding
    if (!(sd_card_p->m_Status & STA_NOINIT)) {
        sd_unlock(sd_card_p);
        return sd_card_p->m_Status;
    }
    // Initialize the member variables
    sd_card_p->card_type = SDCARD_NONE;

    //        pin                             function        pup   pdown  out    state fast
    gpio_conf(sd_card_p->sdio_if_p->CLK_gpio, GPIO_FUNC_PIO1, true, false, true,  true, true);
    gpio_conf(sd_card_p->sdio_if_p->CMD_gpio, GPIO_FUNC_PIO1, true, false, true,  true, true);
    gpio_conf(sd_card_p->sdio_if_p->D0_gpio,  GPIO_FUNC_PIO1, true, false, false, true, true);
    gpio_conf(sd_card_p->sdio_if_p->D1_gpio,  GPIO_FUNC_PIO1, true, false, false, true, true);
    gpio_conf(sd_card_p->sdio_if_p->D2_gpio,  GPIO_FUNC_PIO1, true, false, false, true, true);
    gpio_conf(sd_card_p->sdio_if_p->D3_gpio,  GPIO_FUNC_PIO1, true, false, false, true, true);

    bool ok = sd_sdio_begin(sd_card_p);
    if (ok) {
        // The card is now initialized
        sd_card_p->m_Status &= ~STA_NOINIT;
    }
    sd_unlock(sd_card_p);
    return sd_card_p->m_Status;
}
static void sd_sdio_deinit(sd_card_t *sd_card_p) {
    sd_lock(sd_card_p);

    sd_card_p->m_Status |= STA_NOINIT;
    sd_card_p->card_type = SDCARD_NONE;

    //        pin                             function        pup   pdown   out    state  fast
    gpio_conf(sd_card_p->sdio_if_p->CLK_gpio, GPIO_FUNC_NULL, false, false, false, false, false);
    gpio_conf(sd_card_p->sdio_if_p->CMD_gpio, GPIO_FUNC_NULL, false, false, false, false, false);
    gpio_conf(sd_card_p->sdio_if_p->D0_gpio,  GPIO_FUNC_NULL, false, false, false, false, false);
    gpio_conf(sd_card_p->sdio_if_p->D1_gpio,  GPIO_FUNC_NULL, false, false, false, false, false);
    gpio_conf(sd_card_p->sdio_if_p->D2_gpio,  GPIO_FUNC_NULL, false, false, false, false, false);
    gpio_conf(sd_card_p->sdio_if_p->D3_gpio,  GPIO_FUNC_NULL, false, false, false, false, false);

    //TODO: free other resources: PIO, SMs, etc.

    sd_unlock(sd_card_p);    
}

uint64_t sd_sdio_sectorCount(sd_card_t *sd_card_p) {
    sd_card_p->init(sd_card_p);
    // return g_sdio_csd.capacity();
    return CSD_sectors(sd_card_p->CSD);
}

static int sd_sdio_write_blocks(sd_card_t *sd_card_p, const uint8_t *buffer,
                                uint64_t ulSectorNumber, uint32_t blockCnt) {
    // bool sd_sdio_writeSectors(sd_card_t *sd_card_p, uint32_t sector, const uint8_t* src, size_t ns);
    bool ok;

    sd_lock(sd_card_p);

    if (1 == blockCnt)
        ok = sd_sdio_writeSector(sd_card_p, ulSectorNumber, buffer);
    else
        ok = sd_sdio_writeSectors(sd_card_p, ulSectorNumber, buffer, blockCnt);

    sd_unlock(sd_card_p);

    if (ok)
        return SD_BLOCK_DEVICE_ERROR_NONE;
    else
        return SD_BLOCK_DEVICE_ERROR_WRITE;                                    
}
static int sd_sdio_read_blocks(sd_card_t *sd_card_p, uint8_t *buffer, uint64_t ulSectorNumber,
                               uint32_t ulSectorCount) {
    // bool sd_sdio_readSectors(sd_card_t *sd_card_p, uint32_t sector, uint8_t* dst, size_t n)
    bool ok;

    sd_lock(sd_card_p);

    if (1 == ulSectorCount) 
        ok = sd_sdio_readSector(sd_card_p, ulSectorNumber, buffer);
    else        
        ok= sd_sdio_readSectors(sd_card_p, ulSectorNumber, buffer, ulSectorCount);

    sd_unlock(sd_card_p);
    
    if (ok)
        return SD_BLOCK_DEVICE_ERROR_NONE;
    else
        return SD_BLOCK_DEVICE_ERROR_NO_RESPONSE;
}

void sd_sdio_ctor(sd_card_t *sd_card_p) {
    myASSERT(sd_card_p->sdio_if_p); // Must have an interface object
    /*
    Pins CLK_gpio, D1_gpio, D2_gpio, and D3_gpio are at offsets from pin D0_gpio.
    The offsets are determined by sd_driver\SDIO\rp2040_sdio.pio.
    */
    myASSERT(!sd_card_p->sdio_if_p->CLK_gpio);
    myASSERT(!sd_card_p->sdio_if_p->D1_gpio);
    myASSERT(!sd_card_p->sdio_if_p->D2_gpio);
    myASSERT(!sd_card_p->sdio_if_p->D3_gpio);

    sd_card_p->sdio_if_p->CLK_gpio = (sd_card_p->sdio_if_p->D0_gpio + SDIO_CLK_PIN_D0_OFFSET) % 32;
    sd_card_p->sdio_if_p->D1_gpio = sd_card_p->sdio_if_p->D0_gpio + 1;
    sd_card_p->sdio_if_p->D2_gpio = sd_card_p->sdio_if_p->D0_gpio + 2;
    sd_card_p->sdio_if_p->D3_gpio = sd_card_p->sdio_if_p->D0_gpio + 3;

    sd_card_p->m_Status = STA_NOINIT;

    sd_card_p->init = sd_sdio_init;
    sd_card_p->deinit = sd_sdio_deinit;
    sd_card_p->write_blocks = sd_sdio_write_blocks;
    sd_card_p->read_blocks = sd_sdio_read_blocks;
    sd_card_p->get_num_sectors = sd_sdio_sectorCount;
    sd_card_p->sd_test_com = sd_sdio_test_com;
}

#endif