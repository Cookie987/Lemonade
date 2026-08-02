import shutil
from pathlib import Path
from esphome.core import CORE
import esphome.config_validation as cv

CONFIG_SCHEMA = cv.Schema({}).extend(cv.COMPONENT_SCHEMA)
async def to_code(config):
    source_dir = Path(__file__).resolve().parent
    src_bootloader = source_dir / "bootloader_components"
    dst_bootloader = CORE.relative_build_path("bootloader_components")
    if dst_bootloader.exists():
        shutil.rmtree(dst_bootloader)
    shutil.copytree(src_bootloader, dst_bootloader)
