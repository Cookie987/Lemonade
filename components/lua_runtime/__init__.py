import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_PATH
from pathlib import Path

DEPENDENCIES = ["esp32"]

CONF_ENABLE_STUB = "enable_stub"
CONF_FORCE_32BIT = "force_32bit"

lua_runtime_ns = cg.esphome_ns.namespace("lua_runtime")
LuaRuntime = lua_runtime_ns.class_("LuaRuntime", cg.Component)
LuaRunFileAction = lua_runtime_ns.class_("LuaRunFileAction", automation.Action)

CONFIG_SCHEMA = cv.All(
    cv.require_esphome_version(2025, 7, 0),
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LuaRuntime),
            cv.Optional(CONF_ENABLE_STUB, default=True): cv.boolean,
            cv.Optional(CONF_FORCE_32BIT, default=True): cv.boolean,
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

    if config[CONF_ENABLE_STUB]:
        cg.add_define("LUA_RUNTIME_STUB")

    if config[CONF_FORCE_32BIT]:
        # Avoid 'long long' requirement on some embedded toolchains
        cg.add_define("LUA_32BITS")

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
