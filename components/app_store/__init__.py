import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.http_request import CONF_HTTP_REQUEST_ID, HttpRequestComponent
from esphome.const import CONF_ID, CONF_PLATFORM

DEPENDENCIES = ["http_request", "lua_runtime"]

CONF_INDEX_URL = "index_url"
CONF_LUA_RUNTIME_ID = "lua_runtime_id"

app_store_ns = cg.esphome_ns.namespace("app_store")
lua_runtime_ns = cg.esphome_ns.namespace("lua_runtime")
AppStore = app_store_ns.class_("AppStore", cg.Component)
LuaRuntime = lua_runtime_ns.class_("LuaRuntime", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AppStore),
        cv.GenerateID(CONF_HTTP_REQUEST_ID): cv.use_id(HttpRequestComponent),
        cv.Required(CONF_INDEX_URL): cv.url,
        cv.Optional(CONF_PLATFORM, default="s3_t"): cv.string_strict,
        cv.Optional(CONF_LUA_RUNTIME_ID): cv.use_id(LuaRuntime),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_HTTP_REQUEST_ID])

    cg.add(var.set_index_url(config[CONF_INDEX_URL]))
    cg.add(var.set_platform(config[CONF_PLATFORM]))

    if CONF_LUA_RUNTIME_ID in config:
        lua_runtime = await cg.get_variable(config[CONF_LUA_RUNTIME_ID])
        cg.add(var.set_lua_runtime(lua_runtime))
