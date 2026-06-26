#include "hw_config.h"

// Hardware configuration mapping for the FatFS library over SDIO
static sd_sdio_if_t sdioInterface = {
    .CMD_gpio = 4,
    .D0_gpio = 0,
//    .D1_gpio = 1,
//    .D2_gpio = 2,
//    .D3_gpio = 3,
//    .CLK_gpio = 5,
    .set_drive_strength = true,
    .CMD_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
    .D0_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
    .D1_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
    .D2_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
    .D3_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
    .CLK_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
    .SDIO_PIO = pio1, 
    .DMA_IRQ_num = DMA_IRQ_1,
    .use_exclusive_DMA_IRQ_handler = true
};

static sd_card_t sdCard = {
    .type = SD_IF_SDIO,
    .sdio_if_p = &sdioInterface,
    .use_card_detect = true,
    .card_detect_gpio = 6,
    .card_detected_true = 0, // Assuming standard active-low switch inside the SD socket
    .card_detect_use_pull = true,
    .card_detect_pull_hi = true
};

// These two function names are forced to use underscores because they are 
// hardcoded overrides required by the third-party FatFS library API.
size_t sd_get_num() { 
    return 1; 
}

sd_card_t *sd_get_by_num(size_t num) {
    if (num == 0) {
        return &sdCard;
    }
    return NULL;
}