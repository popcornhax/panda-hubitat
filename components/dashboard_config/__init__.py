"""Editable local dashboard layout and wallpaper service."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome.const import CONF_ID

DEPENDENCIES = ["wifi", "hubitat_local"]
AUTO_LOAD = ["json"]

dashboard_config_ns = cg.esphome_ns.namespace("dashboard_config")
DashboardConfig = dashboard_config_ns.class_("DashboardConfig", cg.Component)


def _device(value):
    value = cv.Schema({
        cv.Required("id"): cv.int_range(min=1, max=0xFFFFFFFF),
        cv.Required("label"): cv.string_strict,
        cv.Required("kind"): cv.one_of("switch", "dimmer", "motion", "weather", "presence", "water", "contact", "sensor"),
        cv.Optional("favorite", default=False): cv.boolean,
    })(value)
    if not value["label"] or len(value["label"]) > 32 or not value["label"].isascii():
        raise cv.Invalid("device label must contain 1 to 32 ASCII characters")
    return value


def _validate(config):
    devices = config["devices"]
    if not 1 <= len(devices) <= 32:
        raise cv.Invalid("devices must contain between 1 and 32 entries")
    ids = [device["id"] for device in devices]
    if len(set(ids)) != len(ids):
        raise cv.Invalid("device IDs must be unique")
    return config


CONFIG_SCHEMA = cv.All(cv.Schema({
    cv.GenerateID(): cv.declare_id(DashboardConfig),
    cv.Required("transport_id"): cv.use_id(cg.esphome_ns.namespace("hubitat_local").class_("HubitatLocal")),
    cv.Required("devices"): cv.All(cv.ensure_list(_device), cv.Length(min=1, max=32)),
}).extend(cv.COMPONENT_SCHEMA), _validate)


async def to_code(config):
    esp32.include_builtin_idf_component("esp_http_server")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    transport = await cg.get_variable(config["transport_id"])
    cg.add(var.set_transport(transport))
    for device in config["devices"]:
        cg.add(var.add_device(device["id"], device["label"], device["kind"], device["favorite"]))
