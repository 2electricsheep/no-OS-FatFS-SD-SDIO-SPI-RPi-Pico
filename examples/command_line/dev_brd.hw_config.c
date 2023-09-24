/* hw_config.c
Copyright 2021 Carl John Kugler III

Licensed under the Apache License, Version 2.0 (the License); you may not use
this file except in compliance with the License. You may obtain a copy of the
License at

   http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an AS IS BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.
*/
/*

This file should be tailored to match the hardware design.

There should be one element of the spi[] array for each hardware SPI used.

There should be one element of the sd_cards[] array for each SD card slot.
The name is should correspond to the FatFs "logical drive" identifier.
(See http://elm-chan.org/fsw/ff/doc/filename.html#vol)
In general, this should correspond to the (zero origin) array index.
The rest of the constants will depend on the type of
socket, which SPI it is driven by, and how it is wired.

*/

#include <string.h>
//
#include "my_debug.h"
//
#include "hw_config.h"
//
#include "ff.h" /* Obtains integer types */
//
#include "diskio.h" /* Declarations of disk functions */

/* 
See https://docs.google.com/spreadsheets/d/1BrzLWTyifongf_VQCc2IpJqXWtsrjmG7KnIbSBy-CPU/edit?usp=sharing,
tab "Monster", for pin assignments assumed in this configuration file.
*/

/* Tested
    0: SPI: OK, 12 MHz only
       SDIO: OK at 31.25 MHz
    1: SPI: OK at 20.8 MHz
    2: SPI: OK at 20.8 MHz
    3: SDIO: OK at 20.8 MHz (not at 31.25 MHz)
    4: SDIO: OK at 20.8 MHz (not at 31.25 MHz)
*/


// Hardware Configuration of SPI "objects"
// Note: multiple SD cards can be driven by one SPI if they use different slave
// selects.
static spi_t spis[] = {  // One for each SPI.
    {
        .hw_inst = spi0,  // SPI component
        .sck_gpio = 2,  // GPIO number (not Pico pin number)
        .mosi_gpio = 3,
        .miso_gpio = 4,
        .set_drive_strength = true,
        .mosi_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        .sck_gpio_drive_strength = GPIO_DRIVE_STRENGTH_2MA,

        // .baud_rate = 25 * 1000 * 1000,  // Actual frequency: 20833333.
        .baud_rate = 12 * 1000 * 1000,  // Actual frequency: 10416666.
       // .baud_rate = 1 * 1000 * 1000,

        .DMA_IRQ_num = DMA_IRQ_0,
        .use_exclusive_DMA_IRQ_handler = true
    },
    {
        .hw_inst = spi1,  // SPI component
        .miso_gpio = 8,  // GPIO number (not Pico pin number)
        .sck_gpio = 10,
        .mosi_gpio = 11,
        .set_drive_strength = true,
        .mosi_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        .sck_gpio_drive_strength = GPIO_DRIVE_STRENGTH_2MA,

        // .baud_rate = 25 * 1000 * 1000,   // Actual frequency: 20833333.
        .baud_rate = 12 * 1000 * 1000,      // Actual frequency: 10416666.

        .DMA_IRQ_num = DMA_IRQ_1
    }
   };

// Hardware Configuration of the SD Card "objects"
static sd_card_t sd_cards[] = {  // One for each SD card
    {        // Socket sd0 over SPI
        .pcName = "0:",  // Name used to mount device
        .type = SD_IF_SPI,
        .spi_if.spi = &spis[0],  // Pointer to the SPI driving this card
        .spi_if.ss_gpio = 7,     // The SPI slave select GPIO for this SD card
        .spi_if.set_drive_strength = true,
        .spi_if.ss_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        // SD Card detect:
        .use_card_detect = true,
        .card_detect_gpio = 9,  
        .card_detected_true = 0, // What the GPIO read returns when a card is
                                 // present.
        .card_detect_use_pull = true,
        .card_detect_pull_hi = true                                 
    },
    // {        // Socket sd0 SDIO
    //     .pcName = "0:",  // Name used to mount device
    //     .type = SD_IF_SDIO,
    //     /*
    //     Pins CLK_gpio, D1_gpio, D2_gpio, and D3_gpio are at offsets from pin D0_gpio.
    //     The offsets are determined by sd_driver\SDIO\rp2040_sdio.pio.
    //         CLK_gpio = (D0_gpio + SDIO_CLK_PIN_D0_OFFSET) % 32;
    //         As of this writing, SDIO_CLK_PIN_D0_OFFSET is 30,
    //           which is -2 in mod32 arithmetic, so:
    //         CLK_gpio = D0_gpio -2.
    //         D1_gpio = D0_gpio + 1;
    //         D2_gpio = D0_gpio + 2;
    //         D3_gpio = D0_gpio + 3;
    //     */
    //     .sdio_if = {
    //         .CMD_gpio = 3,
    //         .D0_gpio = 4,
    //         .SDIO_PIO = pio0,
    //         .DMA_IRQ_num = DMA_IRQ_0},
    //     // SD Card detect:
    //     .use_card_detect = true,
    //     .card_detect_gpio = 9,  
    //     .card_detected_true = 0, // What the GPIO read returns when a card is
    //                              // present.
    //     .card_detect_use_pull = true,
    //     .card_detect_pull_hi = true
    // },
    {        // Socket sd1
        .pcName = "1:",  // Name used to mount device
        .type = SD_IF_SPI,
        .spi_if.spi = &spis[1],  // Pointer to the SPI driving this card
        .spi_if.ss_gpio = 12,     // The SPI slave select GPIO for this SD card
        .spi_if.set_drive_strength = true,
        .spi_if.ss_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        // SD Card detect:
        .use_card_detect = true,
        .card_detect_gpio = 14,  
        .card_detected_true = 0, // What the GPIO read returns when a card is
                                 // present.
        .card_detect_use_pull = true,
        .card_detect_pull_hi = true                                 
    },
    {        // Socket sd2
        .pcName = "2:",  // Name used to mount device
        .type = SD_IF_SPI,
        .spi_if.spi = &spis[1],  // Pointer to the SPI driving this card
        .spi_if.ss_gpio = 13,     // The SPI slave select GPIO for this SD card
        .spi_if.set_drive_strength = true,
        .spi_if.ss_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        // SD Card detect:
        .use_card_detect = true,
        .card_detect_gpio = 15,  
        .card_detected_true = 0, // What the GPIO read returns when a card is
                                 // present.
        .card_detect_use_pull = true,
        .card_detect_pull_hi = true                                 
    },
    {        // Socket sd3
        .pcName = "3:",  // Name used to mount device
        .type = SD_IF_SDIO,
        /*
        Pins CLK_gpio, D1_gpio, D2_gpio, and D3_gpio are at offsets from pin D0_gpio.
        The offsets are determined by sd_driver\SDIO\rp2040_sdio.pio.
            CLK_gpio = (D0_gpio + SDIO_CLK_PIN_D0_OFFSET) % 32;
            As of this writing, SDIO_CLK_PIN_D0_OFFSET is 30,
              which is -2 in mod32 arithmetic, so:
            CLK_gpio = D0_gpio -2.
            D1_gpio = D0_gpio + 1;
            D2_gpio = D0_gpio + 2;
            D3_gpio = D0_gpio + 3;
        */
        .sdio_if = {
            .CMD_gpio = 17,
            .D0_gpio = 18,
            .SDIO_PIO = pio1,
            .DMA_IRQ_num = DMA_IRQ_1,
            .baud_rate = 15*1000*1000 // 15 MHz
        },
        // SD Card detect:
        .use_card_detect = true,
        .card_detect_gpio = 22,  
        .card_detected_true = 0, // What the GPIO read returns when a card is
                                 // present.
        .card_detect_use_pull = true,
        .card_detect_pull_hi = true
    }
    // {        // Socket sd4
    //     .pcName = "4:",  // Name used to mount device
    //     .type = SD_IF_SDIO,
    //     .sdio_if = {
    //         .CMD_gpio = 17,
    //         .D0_gpio = 18,
    //         .SDIO_PIO = pio1,
    //         .DMA_IRQ_num = DMA_IRQ_0},
    //     // SD Card detect:
    //     .use_card_detect = true,
    //     .card_detect_gpio = 26,  
    //     .card_detected_true = 0, // What the GPIO read returns when a card is
    //                              // present.
    //     .card_detect_use_pull = true,
    //     .card_detect_pull_hi = true
    // }
    };

/* ********************************************************************** */
size_t sd_get_num() { return count_of(sd_cards); }
sd_card_t *sd_get_by_num(size_t num) {
    if (num <= sd_get_num()) {
        return &sd_cards[num];
    } else {
        return NULL;
    }
}
size_t spi_get_num() { return count_of(spis); }
spi_t *spi_get_by_num(size_t num) {
    if (num <= spi_get_num()) {
        return &spis[num];
    } else {
        return NULL;
    }
}

/* [] END OF FILE */
