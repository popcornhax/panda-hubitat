"""Local-only asynchronous Hubitat Maker API transport for ESP-IDF."""

import ipaddress
import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome.const import CONF_ID
from esphome.core import CORE

DEPENDENCIES = ["wifi"]
AUTO_LOAD = ["json"]

hubitat_ns = cg.esphome_ns.namespace("hubitat_local")
HubitatLocal = hubitat_ns.class_("HubitatLocal", cg.Component)
HubitatCommand = hubitat_ns.enum("HubitatCommand")

COMMANDS = {
    "on": HubitatCommand.ON,
    "off": HubitatCommand.OFF,
    "setLevel": HubitatCommand.SET_LEVEL,
    "setHeatingSetpoint": HubitatCommand.SET_HEATING_SETPOINT,
    "setCoolingSetpoint": HubitatCommand.SET_COOLING_SETPOINT,
}
RFC1918 = tuple(map(ipaddress.ip_network, ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")))


def _private_ipv4(value):
    value = cv.string_strict(value)
    try:
        address = ipaddress.ip_address(value)
    except ValueError as err:
        raise cv.Invalid("host must be a literal RFC1918 IPv4 address") from err
    if address.version != 4 or not any(address in network for network in RFC1918):
        raise cv.Invalid("host must be a literal RFC1918 IPv4 address")
    return value


def _numeric_app_id(value):
    value = cv.string_strict(value)
    if len(value) > 20 or not re.fullmatch(r"[0-9]+", value):
        raise cv.Invalid("app_id must contain digits only")
    return value


def _device_id(value):
    try:
        value = cv.int_(value)
    except cv.Invalid as err:
        raise cv.Invalid("device ID must be a positive 32-bit integer") from err
    if value < 1 or value > 0xFFFFFFFF:
        raise cv.Invalid("device ID must be a positive 32-bit integer")
    return value


def _command(value):
    # YAML 1.1 parsers treat bare "on" and "off" as booleans.
    if value is True:
        return "on"
    if value is False:
        return "off"
    return cv.one_of(*COMMANDS.keys(), lower=False)(value)


def _validate(config):
    if not CORE.is_esp32:
        raise cv.Invalid("hubitat_local supports ESP32 only")
    if CORE.using_arduino:
        raise cv.Invalid("hubitat_local requires the ESP-IDF framework")
    ids = config["device_ids"]
    if not 1 <= len(ids) <= 32:
        raise cv.Invalid("device_ids must contain between 1 and 32 IDs")
    if len(set(ids)) != len(ids):
        raise cv.Invalid("device_ids must be unique")
    if not config["access_token"] or len(config["access_token"]) > 512:
        raise cv.Invalid("access_token must contain between 1 and 512 characters")
    selected = set(ids)
    seen = set()
    for entry in config["command_allowlist"]:
        device_id = entry["device_id"]
        if device_id not in selected:
            raise cv.Invalid("command_allowlist device IDs must also appear in device_ids")
        if device_id in seen:
            raise cv.Invalid("command_allowlist may contain each device ID only once")
        seen.add(device_id)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HubitatLocal),
            cv.Required("host"): _private_ipv4,
            cv.Required("app_id"): _numeric_app_id,
            cv.Required("access_token"): cv.string_strict,
            cv.Required("device_ids"): cv.ensure_list(_device_id),
            cv.Optional("temperature_scale", default="F"): cv.one_of("C", "F", upper=True),
            cv.Optional("command_allowlist", default=[]): cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required("device_id"): _device_id,
                        cv.Required("commands"): cv.ensure_list(_command),
                    }
                )
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
)


async def to_code(config):
    esp32.request_http_client()
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_host(config["host"]))
    cg.add(var.set_app_id(config["app_id"]))
    cg.add(var.set_access_token(config["access_token"]))
    cg.add(var.set_temperature_scale(config["temperature_scale"]))
    for device_id in config["device_ids"]:
        cg.add(var.add_device_id(device_id))
    for entry in config["command_allowlist"]:
        for command_name in entry["commands"]:
            cg.add(var.add_allowed_command(entry["device_id"], COMMANDS[command_name]))
