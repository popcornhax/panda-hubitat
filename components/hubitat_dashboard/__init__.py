"""LVGL dashboard driven by the private Hubitat device mapping."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light, wifi
from esphome.components.lvgl import defines
from esphome.components.lvgl.lv_validation import lv_font
from esphome.const import CONF_ID

DEPENDENCIES = ["lvgl", "wifi", "hubitat_local", "dashboard_config"]
AUTO_LOAD = []

hubitat_ns = cg.esphome_ns.namespace("hubitat_dashboard")
HubitatDashboard = hubitat_ns.class_("HubitatDashboard", cg.Component)
DeviceKind = hubitat_ns.enum("DashboardDeviceKind")

KINDS = {
    "switch": DeviceKind.SWITCH,
    "dimmer": DeviceKind.DIMMER,
    "motion": DeviceKind.MOTION,
    "weather": DeviceKind.WEATHER,
    "presence": DeviceKind.PRESENCE,
    "water": DeviceKind.WATER,
    "contact": DeviceKind.CONTACT,
    "sensor": DeviceKind.SENSOR,
}


def _device(value):
    value = cv.Schema({
        cv.Required("id"): cv.int_range(min=1, max=0xFFFFFFFF),
        cv.Required("label"): cv.string_strict,
        cv.Required("kind"): cv.one_of(*KINDS, lower=True),
        cv.Optional("favorite", default=False): cv.boolean,
    })(value)
    if not value["label"] or len(value["label"]) > 32:
        raise cv.Invalid("device label must contain 1 to 32 characters")
    return value


CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(HubitatDashboard),
    cv.Required("transport_id"): cv.use_id(cg.esphome_ns.namespace("hubitat_local").class_("HubitatLocal")),
    cv.Required("config_id"): cv.use_id(cg.esphome_ns.namespace("dashboard_config").class_("DashboardConfig")),
    cv.Required("devices"): cv.All(cv.ensure_list(_device), cv.Length(min=1, max=32)),
    cv.Required("backlight_id"): cv.use_id(light.LightState),
    cv.Required("wifi_id"): cv.use_id(wifi.WiFiComponent),
    cv.Optional("fonts", default=["montserrat_14", "montserrat_18", "montserrat_22", "montserrat_28"]): cv.ensure_list(lv_font),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    defines.add_define("LV_USE_SNAPSHOT", "1")
    # The C++ dashboard creates these widgets; no YAML demo should be needed
    # to make ESPHome include their LVGL implementations.
    defines.add_lv_use("label", "button", "bar", "slider", "image")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    transport = await cg.get_variable(config["transport_id"])
    dashboard_config = await cg.get_variable(config["config_id"])
    backlight = await cg.get_variable(config["backlight_id"])
    wifi_component = await cg.get_variable(config["wifi_id"])
    cg.add(var.set_transport(transport))
    cg.add(var.set_config(dashboard_config))
    cg.add(var.set_backlight(backlight))
    cg.add(var.set_wifi(wifi_component))
    for device in config["devices"]:
        cg.add(var.add_device(device["id"], device["label"], KINDS[device["kind"]], device["favorite"]))
