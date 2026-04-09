import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import esp32
from esphome.const import CONF_ID, CONF_PATH, CONF_SUBSTITUTIONS
from esphome.core import CORE
from pathlib import Path

DEPENDENCIES = ["esp32"]

CONF_ENABLE_STUB = "enable_stub"
CONF_FORCE_32BIT = "force_32bit"
CONF_ASYNC_CORE = "async_core"

lua_runtime_ns = cg.esphome_ns.namespace("lua_runtime")
LuaRuntime = lua_runtime_ns.class_("LuaRuntime", cg.Component)
LuaRunFileAction = lua_runtime_ns.class_("LuaRunFileAction", automation.Action)
LuaRunFileAsyncAction = lua_runtime_ns.class_("LuaRunFileAsyncAction", automation.Action)

CONFIG_SCHEMA = cv.All(
    cv.require_esphome_version(2025, 7, 0),
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LuaRuntime),
            cv.Optional(CONF_ENABLE_STUB, default=True): cv.boolean,
            cv.Optional(CONF_FORCE_32BIT, default=True): cv.boolean,
            cv.Optional(CONF_ASYNC_CORE, default=0): cv.one_of(0, 1, int=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
)


# Exclude standalone CLI entrypoints from the firmware build
# (they define their own main()).
EXCLUDED_SOURCES = {
    "lua.c",
    "luac.c",
}


def FILTER_SOURCE_FILES():
    return EXCLUDED_SOURCES


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CORE.is_esp32:
        esp32.include_builtin_idf_component("esp_http_client")
        esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)

    if config[CONF_ENABLE_STUB]:
        cg.add_define("LUA_RUNTIME_STUB")

    if config[CONF_FORCE_32BIT]:
        # Avoid 'long long' requirement on some embedded toolchains
        cg.add_define("LUA_32BITS")

    cg.add(var.set_async_core(config[CONF_ASYNC_CORE]))

    # Export lemonade_version (from substitutions) as a C define if present
    lemonade_ver = None
    try:
        subs = CORE.config.get(CONF_SUBSTITUTIONS, {}) if CORE.config else {}
        lemonade_ver = subs.get("lemonade_version")
    except Exception:
        lemonade_ver = None

    if lemonade_ver:
        cg.add_define("LEMONADE_VERSION", f"\"{lemonade_ver}\"")

    component_dir = Path(__file__).resolve().parent
    cg.add_build_flag(f"-I{component_dir}")


LUA_RUN_FILE_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(LuaRuntime),
        cv.Required(CONF_PATH): cv.templatable(cv.string_strict),
    }
)


@automation.register_action(
    "lua_runtime.run_file",
    LuaRunFileAction,
    LUA_RUN_FILE_ACTION_SCHEMA,
    synchronous=True,
)
async def lua_run_file_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    path_ = await cg.templatable(config[CONF_PATH], args, cg.std_string)
    cg.add(var.set_path(path_))
    return var


@automation.register_action(
    "lua_runtime.run_file_async",
    LuaRunFileAsyncAction,
    LUA_RUN_FILE_ACTION_SCHEMA,
    synchronous=True,
)
async def lua_run_file_async_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    path_ = await cg.templatable(config[CONF_PATH], args, cg.std_string)
    cg.add(var.set_path(path_))
    return var
