import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import SdMmc, CONF_SD_MMC_CARD_ID

DEPENDENCIES = ["sd_mmc_card"]

CONF_CARD_DETECTED = "card_detected"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_SD_MMC_CARD_ID): cv.use_id(SdMmc),
        cv.Optional(CONF_CARD_DETECTED): binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ),
    }
)


async def to_code(config):
    sd_mmc_component = await cg.get_variable(config[CONF_SD_MMC_CARD_ID])

    if CONF_CARD_DETECTED in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_CARD_DETECTED])
        cg.add(sd_mmc_component.set_card_detected_binary_sensor(sens))
