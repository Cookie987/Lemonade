from urllib.parse import urlparse

from esphome import automation
from esphome.automation import Condition
import esphome.codegen as cg
from esphome.components import socket
import esphome.config_validation as cv
from esphome.const import (
    CONF_AUTO_CONNECT,
    CONF_ID,
    CONF_MESSAGE,
    CONF_ON_CONNECT,
    CONF_ON_DISCONNECT,
    CONF_ON_MESSAGE,
    CONF_RECONNECT_INTERVAL,
    CONF_TRIGGER_ID,
    CONF_URL,
    PLATFORM_ESP32,
    PLATFORM_ESP8266,
)
from esphome.types import ConfigType

DEPENDENCIES = ["network"]
CODEOWNERS = ["@esphome/core"]
MULTI_CONF = True

websocket_client_ns = cg.esphome_ns.namespace("websocket_client")
WebsocketClient = websocket_client_ns.class_("WebsocketClient", cg.Component)
WebsocketClientMessageTrigger = websocket_client_ns.class_(
    "WebsocketClientMessageTrigger", automation.Trigger.template(cg.std_string)
)
WebsocketClientConnectTrigger = websocket_client_ns.class_(
    "WebsocketClientConnectTrigger", automation.Trigger.template()
)
WebsocketClientDisconnectTrigger = websocket_client_ns.class_(
    "WebsocketClientDisconnectTrigger", automation.Trigger.template()
)
WebsocketClientSendAction = websocket_client_ns.class_(
    "WebsocketClientSendAction", automation.Action
)
WebsocketClientConnectAction = websocket_client_ns.class_(
    "WebsocketClientConnectAction", automation.Action
)
WebsocketClientDisconnectAction = websocket_client_ns.class_(
    "WebsocketClientDisconnectAction", automation.Action
)
WebsocketClientConnectedCondition = websocket_client_ns.class_(
    "WebsocketClientConnectedCondition", Condition
)


def validate_websocket_url(value: str) -> str:
    value = cv.string_strict(value)
    parsed = urlparse(value)
    if parsed.scheme not in ("ws", "wss"):
        raise cv.Invalid("URL must start with 'ws://' or 'wss://'")
    if not parsed.netloc:
        raise cv.Invalid("WebSocket URL must include a host")
    return value


def _consume_websocket_socket(config: ConfigType) -> ConfigType:
    socket.consume_sockets(1, "websocket_client")(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WebsocketClient),
            cv.Required(CONF_URL): validate_websocket_url,
            cv.Optional(CONF_AUTO_CONNECT, default=True): cv.boolean,
            cv.Optional(
                CONF_RECONNECT_INTERVAL, default="5s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_MESSAGE): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        WebsocketClientMessageTrigger
                    ),
                }
            ),
            cv.Optional(CONF_ON_CONNECT): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        WebsocketClientConnectTrigger
                    ),
                }
            ),
            cv.Optional(CONF_ON_DISCONNECT): automation.validate_automation(
                {
                    cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                        WebsocketClientDisconnectTrigger
                    ),
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32, PLATFORM_ESP8266]),
    cv.only_with_arduino,
    _consume_websocket_socket,
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add_define("USE_WEBSOCKET_CLIENT")
    cg.add_global(websocket_client_ns.using)
    cg.add_library("gilmaimon/ArduinoWebsockets", "0.5.4")

    cg.add(var.set_url(config[CONF_URL]))
    cg.add(var.set_auto_connect(config[CONF_AUTO_CONNECT]))
    cg.add(var.set_reconnect_interval(config[CONF_RECONNECT_INTERVAL]))

    for conf in config.get(CONF_ON_MESSAGE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "message")], conf)

    for conf in config.get(CONF_ON_CONNECT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_DISCONNECT, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)


WEBSOCKET_CLIENT_SEND_ACTION_SCHEMA = automation.maybe_simple_value(
    {
        cv.GenerateID(): cv.use_id(WebsocketClient),
        cv.Required(CONF_MESSAGE): cv.templatable(cv.string),
    },
    key=CONF_MESSAGE,
)


@automation.register_action(
    "websocket_client.send",
    WebsocketClientSendAction,
    WEBSOCKET_CLIENT_SEND_ACTION_SCHEMA,
    synchronous=True,
)
async def websocket_client_send_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(config[CONF_MESSAGE], args, cg.std_string)
    cg.add(var.set_message(template_))
    return var


@automation.register_action(
    "websocket_client.connect",
    WebsocketClientConnectAction,
    cv.Schema({cv.GenerateID(): cv.use_id(WebsocketClient)}),
    synchronous=True,
)
async def websocket_client_connect_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "websocket_client.disconnect",
    WebsocketClientDisconnectAction,
    cv.Schema({cv.GenerateID(): cv.use_id(WebsocketClient)}),
    synchronous=True,
)
async def websocket_client_disconnect_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_condition(
    "websocket_client.connected",
    WebsocketClientConnectedCondition,
    cv.Schema({cv.GenerateID(): cv.use_id(WebsocketClient)}),
)
async def websocket_client_connected_to_code(config, condition_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, parent)
