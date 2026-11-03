#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate stable setting IDs and a compressed canonical-CBOR UI schema."""

import argparse
import gzip
import hashlib
import importlib.util
import json
import pickle
import re
import sys
from pathlib import Path
from types import SimpleNamespace


VALUE_TYPES = {
    "bool": (1, False, True),
    "u8": (8, 0, (1 << 8) - 1),
    "u16": (16, 0, (1 << 16) - 1),
    "u32": (32, 0, (1 << 32) - 1),
    "s8": (8, -(1 << 7), (1 << 7) - 1),
    "s16": (16, -(1 << 15), (1 << 15) - 1),
    "s32": (32, -(1 << 31), (1 << 31) - 1),
}
NAME_RE = re.compile(r"^[a-z][a-z0-9_-]{0,47}$")
MAX_SETTING_DESCRIPTORS = 0xFFFF
MAX_CALLBACKS = 0xFFFF
MAX_PERSISTENT_SETTINGS_PER_GROUP = 0xFF
GROUP_RECORD_HEADER_SIZE = 17


def node_value(node, name, default=None):
    prop = node.props.get(name)
    return default if prop is None else prop.val


def normalize_signed_dts_cell(value, value_type):
    """Convert a sign-extended 32-bit DTS cell to its signed Python value."""
    if (
        value_type in {"s8", "s16", "s32"}
        and type(value) is int
        and 0x80000000 <= value <= 0xFFFFFFFF
    ):
        return value - (1 << 32)
    return value


def stable_id(parts):
    canonical = "\x1f".join(parts).encode("utf-8")
    value = int.from_bytes(hashlib.sha256(canonical).digest()[:4], "big")
    if value == 0:
        raise ValueError(f"reserved zero ID for {'/'.join(parts)}")
    return value


def owner_contract_digest(group_id, format_version, definition):
    contract = {
        "contract_version": 1,
        "group_id": group_id,
        "format_version": format_version,
        "definition": definition,
    }
    canonical = json.dumps(contract, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def expand_text_setting(setting):
    """Lower one logical UTF-8 setting into the u32 slots used by the store."""
    if node_value(setting, "value-type") != "text":
        return [setting]

    name = check_name(node_value(setting, "setting-name"), "setting-name")
    text_length = node_value(setting, "text-length")
    if type(text_length) is not int or not 1 <= text_length <= 256:
        raise ValueError(f"{name}: text-length must be in 1..256")
    if node_value(setting, "ui-component") is not None:
        raise ValueError(f"{name}: text settings generate ui-component values automatically")
    if node_value(setting, "widget", "auto") not in ("auto", "textbox"):
        raise ValueError(f"{name}: text settings require the textbox widget")
    unsupported = (
        "default-bool", "default-int", "default-enum", "default-property", "minimum",
        "maximum", "step", "enum-values", "unit", "max-length", "runtime-field",
    )
    if any(key in setting.props for key in unsupported):
        raise ValueError(f"{name}: text settings cannot declare scalar defaults, bounds, units, or runtime-field")

    text = node_value(setting, "default-string", "")
    if not isinstance(text, str) or "\0" in text:
        raise ValueError(f"{name}: default-string must be UTF-8 text without NUL")
    encoded = text.encode("utf-8")
    if len(encoded) > text_length:
        raise ValueError(f"{name}: default-string exceeds text-length")
    encoded = encoded.ljust(((text_length + 3) // 4) * 4, b"\0")

    generated = []
    for index in range((text_length + 3) // 4):
        props = dict(setting.props)
        props.pop("default-string", None)
        props["setting-name"] = SimpleNamespace(val=f"{name}-{index}")
        props["value-type"] = SimpleNamespace(val="u32")
        props["default-int"] = SimpleNamespace(
            val=int.from_bytes(encoded[index * 4 : index * 4 + 4], "little")
        )
        props["minimum"] = SimpleNamespace(val=0)
        props["maximum"] = SimpleNamespace(val=0xFFFFFFFF)
        props["step"] = SimpleNamespace(val=1)
        props["widget"] = SimpleNamespace(val="textbox")
        props["ui-component"] = SimpleNamespace(val=f"textbox-{name}-{index}")
        if index == 0:
            props["text-length"] = SimpleNamespace(val=text_length)
        else:
            props.pop("text-length", None)
        generated.append(SimpleNamespace(
            status=setting.status,
            compats=setting.compats,
            props=props,
            children={},
        ))
    return generated


def c_macro_identifier(parts):
    return "_".join(part.upper().replace("-", "_") for part in parts)


def cbor_head(major, value):
    if value < 0:
        raise ValueError("CBOR head value must be nonnegative")
    if value < 24:
        return bytes([(major << 5) | value])
    if value < 1 << 8:
        return bytes([(major << 5) | 24, value])
    if value < 1 << 16:
        return bytes([(major << 5) | 25]) + value.to_bytes(2, "big")
    if value < 1 << 32:
        return bytes([(major << 5) | 26]) + value.to_bytes(4, "big")
    if value < 1 << 64:
        return bytes([(major << 5) | 27]) + value.to_bytes(8, "big")
    raise ValueError("integer exceeds canonical CBOR uint64 range")


def cbor(value):
    if value is None:
        return b"\xf6"
    if value is False:
        return b"\xf4"
    if value is True:
        return b"\xf5"
    if isinstance(value, int):
        if value >= 0:
            return cbor_head(0, value)
        return cbor_head(1, -1 - value)
    if isinstance(value, str):
        encoded = value.encode("utf-8")
        return cbor_head(3, len(encoded)) + encoded
    if isinstance(value, bytes):
        return cbor_head(2, len(value)) + value
    if isinstance(value, (list, tuple)):
        return cbor_head(4, len(value)) + b"".join(cbor(item) for item in value)
    if isinstance(value, dict):
        entries = [(cbor(key), cbor(item)) for key, item in value.items()]
        entries.sort(key=lambda pair: (len(pair[0]), pair[0]))
        return cbor_head(5, len(entries)) + b"".join(k + v for k, v in entries)
    raise ValueError(f"unsupported schema value {type(value).__name__}")


def check_name(value, field):
    if not isinstance(value, str) or not NAME_RE.fullmatch(value):
        raise ValueError(f"{field} must match {NAME_RE.pattern}: {value!r}")
    return value


def validate_callback(value):
    if value is not None and (not isinstance(value, str) or
                              not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value)):
        raise ValueError(f"on-change must be a C function name: {value!r}")
    return value


def canonical_bool(value, field):
    if type(value) is not bool:
        raise ValueError(f"{field} must be boolean")
    return value


def validate_setting(setting, group, hardware_default=None):
    name = check_name(node_value(setting, "setting-name"), "setting-name")
    volatile = canonical_bool(node_value(setting, "volatile", False), f"{name}.volatile")
    persistent = canonical_bool(
        node_value(setting, "persistent", not volatile), f"{name}.persistent"
    )
    if volatile and persistent:
        raise ValueError(f"{name}: volatile setting cannot be persistent")
    if volatile and not canonical_bool(node_value(setting, "read-only", False), f"{name}.read-only"):
        raise ValueError(f"{name}: volatile setting must be read-only")
    ui_subgroup = node_value(setting, "ui-subgroup")
    if ui_subgroup is not None and (
        not isinstance(ui_subgroup, str) or not ui_subgroup.strip() or len(ui_subgroup) > 64
    ):
        raise ValueError(f"{name}: ui-subgroup must be a nonempty string of at most 64 characters")
    ui_component = node_value(setting, "ui-component") or None
    allowed_ui_components = {
        f"curve-{kind}-{index}"
        for kind in ("point", "rate")
        for index in range(1, 4)
    }
    allowed_ui_components.add("curve-input-count")
    allowed_ui_components.update({
        "ble-profile-address-high",
        "ble-profile-address-low",
        "ble-profile-address-paired",
    })
    graph_component = ui_component is not None and re.fullmatch(
        r"graph-[xy]-[1-9][0-9]*", ui_component
    )
    textbox_component = ui_component is not None and re.fullmatch(
        r"textbox-(?:([a-z][a-z0-9_-]*)-)?([0-9]+)", ui_component
    )
    if ui_component is not None and ui_component not in allowed_ui_components and not graph_component and not textbox_component:
        raise ValueError(f"{name}: unsupported ui-component {ui_component!r}")
    value_type = node_value(setting, "value-type")
    default_property = {
        "bool": "default-bool",
        "enum": "default-enum",
    }.get(value_type, "default-int")
    direct_defaults = {
        key: node_value(setting, key)
        for key in ("default-bool", "default-int", "default-enum")
        if key in setting.props
    }
    if len(direct_defaults) > 1 or (direct_defaults and default_property not in direct_defaults):
        raise ValueError(f"{name}: default property does not match value-type")
    if hardware_default is not None and direct_defaults:
        raise ValueError(f"{name}: default and default-property cannot both be declared")
    default = (
        hardware_default
        if hardware_default is not None
        else direct_defaults.get(default_property)
    )
    if default is None:
        raise ValueError(f"{name}: setting needs {default_property} or default-property")
    if value_type == "bool" and default_property in direct_defaults:
        if type(default) is not int or default not in (0, 1):
            raise ValueError(f"{name}: default-bool must be 0 or 1")
        default = bool(default)
    minimum = node_value(setting, "minimum")
    maximum = node_value(setting, "maximum")
    step = node_value(setting, "step", 1)
    if value_type == "enum":
        choices = node_value(setting, "enum-values", [])
        if step != 1:
            raise ValueError(f"{name}: enum step must be 1")
        if not choices or any(not isinstance(choice, str) for choice in choices):
            raise ValueError(f"{name}: enum requires nonempty string enum-values")
        if len(set(choices)) != len(choices) or default not in choices:
            raise ValueError(f"{name}: duplicate enum choice or default outside enum-values")
        minimum = 0
        maximum = len(choices) - 1
        if maximum > 255:
            raise ValueError(f"{name}: enum has more than 256 values")
        default = choices.index(default)
        schema_default = choices[default]
        wire_length = 1
    elif value_type == "bool":
        if minimum is not None or maximum is not None or step != 1:
            raise ValueError(f"{name}: bool cannot declare numeric bounds or step")
        canonical_bool(default, f"{name}.default")
        minimum = 0
        maximum = 1
        choices = []
        schema_default = default
        wire_length = 1
    else:
        if value_type not in VALUE_TYPES or type(default) is not int:
            raise ValueError(f"{name}: unsupported value type or noninteger default")
        _, type_min, type_max = VALUE_TYPES[value_type]
        minimum = type_min if minimum is None else minimum
        maximum = type_max if maximum is None else maximum
        default = normalize_signed_dts_cell(default, value_type)
        minimum = normalize_signed_dts_cell(minimum, value_type)
        maximum = normalize_signed_dts_cell(maximum, value_type)
        if type(minimum) is not int or type(maximum) is not int or type(step) is not int:
            raise ValueError(f"{name}: bounds and step must be integers")
        if minimum > maximum or step <= 0 or not minimum <= default <= maximum:
            raise ValueError(f"{name}: invalid range, step, or default")
        if (default - minimum) % step:
            raise ValueError(f"{name}: default does not align to step")
        if minimum < type_min or maximum > type_max:
            raise ValueError(f"{name}: range exceeds {value_type}")
        choices = []
        schema_default = default
        wire_length = VALUE_TYPES[value_type][0] // 8
        if (maximum - minimum) % step:
            raise ValueError(f"{name}: maximum does not align to step")

    widget = node_value(setting, "widget", "auto")
    text_length = node_value(setting, "text-length")
    if widget == "button" and value_type != "bool":
        raise ValueError(f"{name}: button widget requires a bool setting")
    if widget == "textbox":
        if value_type != "u32" or not textbox_component:
            raise ValueError(f"{name}: textbox widget requires a u32 textbox-N component")
        chunk = int(textbox_component.group(2))
        if text_length is not None and (chunk != 0 or type(text_length) is not int or not 1 <= text_length <= 256):
            raise ValueError(f"{name}: text-length must be 1..256 and declared on textbox-0")
        if chunk == 0 and text_length is None:
            raise ValueError(f"{name}: textbox-0 requires text-length")
    elif textbox_component:
        raise ValueError(f"{name}: textbox-N components require widget = textbox")
    elif text_length is not None:
        raise ValueError(f"{name}: text-length is only valid on textbox-0")
    if widget == "graph":
        if value_type in ("bool", "enum"):
            raise ValueError(f"{name}: graph widget requires a numeric setting")
        if not graph_component:
            raise ValueError(f"{name}: graph widget needs ui-component graph-x-N or graph-y-N")
        if minimum >= maximum:
            raise ValueError(f"{name}: graph axis minimum must be less than maximum")
    elif graph_component:
        raise ValueError(f"{name}: graph-x-N and graph-y-N components require widget = graph")

    max_length = node_value(setting, "max-length", wire_length)
    if type(max_length) is not int or max_length != wire_length:
        raise ValueError(f"{name}: max-length must equal fixed wire width {wire_length}")

    return {
        "name": name,
        "type": value_type,
        "default": schema_default,
        "wire_default": default,
        "default_property": node_value(setting, "default-property"),
        "minimum": minimum,
        "maximum": maximum,
        "step": step,
        "enum_values": choices,
        "label": node_value(setting, "label"),
        "ui_subgroup": ui_subgroup,
        "ui_component": ui_component,
        "help": node_value(setting, "help", ""),
        "unit": node_value(setting, "unit", ""),
        "widget": widget,
        "text_length": text_length,
        "read_only": canonical_bool(node_value(setting, "read-only", False), f"{name}.read-only"),
        "persistent": persistent,
        "reboot_required": canonical_bool(
            node_value(setting, "reboot-required", False), f"{name}.reboot-required"
        ),
        "max_length": max_length,
        "on_change": validate_callback(node_value(setting, "on-change")),
        "runtime_field": node_value(setting, "runtime-field"),
        "group": group,
        "source": setting,
    }


def load_contributors(paths):
    contributors = []
    for index, path in enumerate(paths):
        path = Path(path)
        spec = importlib.util.spec_from_file_location(f"dynamic_schema_contributor_{index}", path)
        if spec is None or spec.loader is None:
            raise ValueError(f"cannot load schema contributor {path}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        contribute = getattr(module, "contribute", None)
        if not callable(contribute):
            raise ValueError(f"schema contributor {path} has no contribute(context) function")
        contributors.append((str(path), contribute))
    return contributors


def build_schema(edt, layer_reordering=False, config_values=None, contributors=()):
    groups = []
    group_ids = {}
    setting_ids = {}
    group_macro_ids = {}
    setting_macro_ids = {}
    ordered_nodes = list(edt.nodes)
    config_values = config_values or {}

    def record_default(node, default, maximum, field):
        value = node_value(node, "record-default", default)
        if value is None or value == []:
            value = default
        if not isinstance(value, (list, tuple)) or not value or len(value) > maximum:
            raise ValueError(f"{field}: record-default must contain 1..{maximum} bytes")
        if any(type(byte) is not int or not 0 <= byte <= 255 for byte in value):
            raise ValueError(f"{field}: record-default values must be bytes in 0..255")
        return list(value)

    def stable_behavior_id(device_node, field):
        # DEVICE_DT_NAME() uses the label property or the node's full name,
        # not edtlib's absolute path (which also includes parent nodes).
        device_name = node_value(device_node, "label") or getattr(device_node, "name", None)
        if not isinstance(device_name, str) or not device_name:
            raise ValueError(f"{field}: behavior node has no stable device name")
        identity = behavior_identity(device_name)
        if identity == 0:
            raise ValueError(f"{field}: behavior device-name hash is reserved zero")
        return identity

    def behavior_identity(name):
        value = 2166136261
        for byte in name.encode("utf-8"):
            value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
        value ^= value >> 16
        return value & 0xFFFF

    # The runtime record stores only a 16-bit identity. Detect ambiguity across
    # all declared ZMK behaviors before emitting a schema that could fail only
    # after a setting is written or first used.
    behavior_metadata_enabled = bool(config_values.get("CONFIG_ZMK_BEHAVIOR_METADATA", 0))
    behavior_identities = {}
    for behavior in ordered_nodes:
        if behavior.status != "okay" or not any(
            "behavior-" in compatible for compatible in set(behavior.compats)
        ):
            continue
        device_name = node_value(behavior, "label") or getattr(behavior, "name", None)
        if not isinstance(device_name, str) or not device_name:
            continue
        identity_names = [device_name]
        display_name = node_value(behavior, "display-name")
        if behavior_metadata_enabled and isinstance(display_name, str) and display_name:
            identity_names.append(display_name)
        for identity_name in identity_names:
            identity = behavior_identity(identity_name)
            if identity == 0:
                raise ValueError(
                    f"Behavior identity for {device_name!r} is reserved zero"
                )
            previous = behavior_identities.get(identity)
            if previous is not None and previous[0] is not behavior:
                raise ValueError(
                    f"16-bit Behavior identity collision 0x{identity:04x}: "
                    f"{previous[1]!r} ({previous[2]}) and "
                    f"{identity_name!r} ({device_name})"
                )
            behavior_identities[identity] = (behavior, identity_name, device_name)

    def default_behavior_binding(node, property_name, field):
        value = node_value(node, property_name)
        if value is None:
            return None
        if not isinstance(value, list) or len(value) != 1:
            raise ValueError(f"{field}: {property_name} must contain exactly one Behavior binding")
        binding = value[0]
        behavior = getattr(binding, "controller", None)
        params = getattr(binding, "data", None)
        if behavior is None or not isinstance(params, dict):
            raise ValueError(f"{field}: {property_name} must reference a valid Behavior binding")
        param1 = params.get("param1", 0)
        param2 = params.get("param2", 0)
        if any(type(param) is not int or not 0 <= param <= 0xFFFFFFFF
               for param in (param1, param2)):
            raise ValueError(f"{field}: {property_name} parameters must fit unsigned 32-bit values")
        return (stable_behavior_id(behavior, field), param1, param2)

    def make_group(values):
        return SimpleNamespace(
            status="okay",
            compats={"sekigon,dynamic-settings-group"},
            props={key: SimpleNamespace(val=value) for key, value in values.items()},
            children={},
        )

    def validate_slot_count(feature, nodes):
        config_key = f"CONFIG_ZMK_DYNAMIC_{feature.upper().replace('-', '_')}_MAX_SLOTS"
        maximum = config_values.get(config_key)
        if maximum is not None and len(nodes) > maximum:
            raise ValueError(
                f"{feature} has {len(nodes)} dispatcher slots, exceeding {config_key}={maximum}"
            )
        indices = [node_value(node, "slot-index") for node in nodes]
        if any(type(index) is not int or index < 0 for index in indices):
            raise ValueError(f"{feature} dispatcher slot-index must be nonnegative integers")
        if sorted(indices) != list(range(len(indices))):
            raise ValueError(f"{feature} dispatcher slot-index values must be unique and contiguous from 0")

    keyboard_target_roles = {
        node_value(node, "target-role")
        for node in ordered_nodes
        if node.status == "okay"
        and isinstance(node_value(node, "target-role"), str)
        and node_value(node, "target-role").startswith("keyboard-")
    }
    schema_default_target_role = (
        next(iter(keyboard_target_roles)) if len(keyboard_target_roles) == 1 else "keyboard-right"
    )

    def default_target_role():
        return schema_default_target_role

    context = SimpleNamespace(
        nodes=ordered_nodes,
        config_values=config_values,
        layer_reordering=layer_reordering,
        node_value=node_value,
        record_default=record_default,
        stable_behavior_id=stable_behavior_id,
        default_behavior_binding=default_behavior_binding,
        check_name=check_name,
        make_group=make_group,
        validate_slot_count=validate_slot_count,
        default_target_role=default_target_role,
    )
    for contributor_name, contribute in contributors:
        try:
            ordered_nodes.extend(contribute(context))
        except ValueError as error:
            raise ValueError(f"{contributor_name}: {error}") from error

    group_order = {}
    for node in ordered_nodes:
        if node.status != "okay":
            continue
        compatible = set(node.compats)
        if "sekigon,dynamic-settings-group" not in compatible and "sekigon,dynamic-settings-proxy-group" not in compatible:
            continue
        kconfig_condition = node_value(node, "kconfig-condition")
        if kconfig_condition is not None:
            if not isinstance(kconfig_condition, str) or not re.fullmatch(
                r"CONFIG_[A-Z0-9_]+", kconfig_condition
            ):
                raise ValueError("kconfig-condition must be a Kconfig symbol name")
            if not config_values.get(kconfig_condition, 0):
                continue
        proxy = "sekigon,dynamic-settings-proxy-group" in compatible
        module = check_name(node_value(node, "module-name"), "module-name")
        group_name = check_name(node_value(node, "group-name"), "group-name")
        instance = check_name(node_value(node, "instance-name"), "instance-name")
        ordering_key = (module, group_name)
        ui_order = group_order.get(ordering_key, 0)
        group_order[ordering_key] = ui_order + 1
        canonical = [module, group_name, instance]
        group_macro = "DYNAMIC_GROUP_ID_" + c_macro_identifier(canonical)
        group_id = stable_id(canonical)
        for macro_name in (group_macro, f"{group_macro}_{group_id:08X}"):
            previous = group_macro_ids.get(macro_name)
            if previous is not None:
                raise ValueError(
                    f"group ID macro collision: {'/'.join(previous)} and "
                    f"{'/'.join(canonical)}"
                )
            group_macro_ids[macro_name] = canonical
        previous = group_ids.get(group_id)
        if previous is not None:
            if previous != canonical:
                raise ValueError(f"group ID collision: {previous} and {canonical}")
            raise ValueError(f"duplicate group: {'/'.join(canonical)}")
        group_ids[group_id] = canonical
        scope = node_value(node, "scope")
        if scope not in ("local", "shared", "owner"):
            raise ValueError(f"{'/'.join(canonical)}: invalid scope {scope!r}")
        format_version = node_value(node, "format-version")
        if type(format_version) is not int or not 1 <= format_version <= 65535:
            raise ValueError(f"{'/'.join(canonical)}: invalid format-version")
        default_provider = validate_callback(node_value(node, "default-provider"))
        default_provider_context_free = canonical_bool(
            node_value(node, "default-provider-context-free", False),
            f"{'/'.join(canonical)}.default-provider-context-free",
        )
        runtime_read_provider = validate_callback(node_value(node, "runtime-read-provider"))
        runtime_apply_provider = validate_callback(node_value(node, "runtime-apply-provider"))
        runtime_struct_header = node_value(node, "runtime-struct-header")
        runtime_struct_type = node_value(node, "runtime-struct-type")
        runtime_lock_field = node_value(node, "runtime-lock-field")
        has_runtime_struct = bool(runtime_struct_header or runtime_struct_type)
        if has_runtime_struct and not (
            isinstance(runtime_struct_header, str)
            and re.fullmatch(r"[A-Za-z0-9_./-]+", runtime_struct_header)
            and isinstance(runtime_struct_type, str)
            and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", runtime_struct_type)
        ):
            raise ValueError(
                f"{'/'.join(canonical)}: runtime struct header and type must be declared together"
            )
        if runtime_apply_provider and not has_runtime_struct:
            raise ValueError(f"{'/'.join(canonical)}: runtime-apply-provider requires runtime struct metadata")
        if runtime_lock_field and (
            not has_runtime_struct
            or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(?:\[[0-9]+\])?", runtime_lock_field)
        ):
            raise ValueError(f"{'/'.join(canonical)}: runtime-lock-field requires a valid runtime struct member")
        record_type = node_value(node, "record-type")
        record_max_length = node_value(node, "record-max-length")
        record_default = node_value(node, "record-default")
        record_callback = validate_callback(node_value(node, "record-on-change"))
        record_callback_context_free = canonical_bool(
            node_value(node, "record-on-change-context-free", False),
            f"{'/'.join(canonical)}.record-on-change-context-free",
        )
        slot_index = node_value(node, "slot-index", 0xFF)
        if type(slot_index) is not int or not 0 <= slot_index <= 0xFF:
            raise ValueError(
                f"{'/'.join(canonical)}: slot-index must be an unsigned 8-bit integer"
            )
        is_record = record_type is not None
        if is_record:
            if not proxy and record_default is None:
                record_default = []
            if not isinstance(record_type, str) or not NAME_RE.fullmatch(record_type):
                raise ValueError(f"{'/'.join(canonical)}: invalid record-type")
            if type(record_max_length) is not int or not 1 <= record_max_length <= 540:
                raise ValueError(f"{'/'.join(canonical)}: record-max-length must be in 1..540")
            if (not proxy and not isinstance(record_default, list)) or (
                record_default is not None and not isinstance(record_default, list)
            ) or any(
                type(value) is not int or not 0 <= value <= 255 for value in (record_default or [])
            ) or len(record_default or []) > record_max_length:
                raise ValueError(f"{'/'.join(canonical)}: record-default must be a byte array within record-max-length")
            if default_provider or node_value(node, "default-provider"):
                raise ValueError(f"{'/'.join(canonical)}: record groups use record-default, not default-provider")
            if runtime_read_provider:
                raise ValueError(f"{'/'.join(canonical)}: record groups cannot use runtime-read-provider")
        elif record_max_length is not None or record_default is not None or record_callback:
            raise ValueError(f"{'/'.join(canonical)}: record properties require record-type")
        if proxy:
            if default_provider_context_free:
                raise ValueError(
                    f"{'/'.join(canonical)}: proxy cannot use default-provider-context-free"
                )
            if (default_provider or runtime_read_provider or runtime_apply_provider or
                    runtime_struct_header or runtime_struct_type or runtime_lock_field or record_default is not None or record_callback):
                raise ValueError(f"{'/'.join(canonical)}: proxy cannot define actual record defaults or callbacks")
            target = node_value(node, "target-id")
            if not isinstance(target, str) or not target:
                raise ValueError(f"{'/'.join(canonical)}: proxy needs target-id")
            target_source = node_value(node, "target-source")
            if target_source is not None and (
                type(target_source) is not int or not 0 <= target_source < 2
            ):
                raise ValueError(f"{'/'.join(canonical)}: target-source must be 0 or 1")
        else:
            target = node_value(node, "target-role")
            if not isinstance(target, str) or not target:
                raise ValueError(f"{'/'.join(canonical)}: actual group needs target-role")
            if default_provider_context_free and not default_provider:
                raise ValueError(
                    f"{'/'.join(canonical)}: default-provider-context-free needs default-provider"
                )
            if (
                node_value(node, "actual-device") is None
                and not default_provider_context_free
                and not record_callback_context_free
            ):
                raise ValueError(f"{'/'.join(canonical)}: actual group needs actual-device")
            target_source = None

        settings = []
        definition = {
            "format_version": format_version,
            "scope": scope,
            "record_type": record_type,
            "record_max_length": record_max_length,
            "settings": [],
        }
        actual_device = node_value(node, "actual-device") if not proxy else None
        device_labels = getattr(actual_device, "labels", []) if actual_device else []
        device_label = device_labels[0] if device_labels else None
        expanded_children = (
            generated
            for source in node.children.values()
            if "sekigon,dynamic-setting" in set(source.compats)
            for generated in expand_text_setting(source)
        )
        for child in expanded_children:
            default_property = node_value(child, "default-property")
            hardware_default = None
            if default_property:
                if proxy or actual_device is None or not hasattr(actual_device, "props"):
                    raise ValueError(f"{'/'.join(canonical)}: default-property requires an actual device")
                hardware_default = node_value(actual_device, default_property)
                if hardware_default is None:
                    if node_value(child, "value-type") == "bool":
                        hardware_default = False
                    else:
                        raise ValueError(
                            f"{'/'.join(canonical)}: actual device has no {default_property} property"
                        )
            item = validate_setting(child, canonical, hardware_default)
            if item["runtime_field"] is not None:
                if not has_runtime_struct or not re.fullmatch(
                    r"[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*(?:\[[0-9]+\])?",
                    item["runtime_field"],
                ):
                    raise ValueError(
                        f"{'/'.join(canonical)}/{item['name']}: runtime-field requires a valid runtime struct binding"
                    )
                if item["on_change"]:
                    raise ValueError(
                        f"{'/'.join(canonical)}/{item['name']}: runtime-field cannot be combined with on-change"
                    )
            elif runtime_apply_provider:
                raise ValueError(
                    f"{'/'.join(canonical)}/{item['name']}: runtime-apply-provider groups require runtime-field on every setting"
                )
            if proxy and item["on_change"]:
                raise ValueError(f"{'/'.join(canonical)}: proxy settings cannot have on-change")
            needs_device_context = (
                item["on_change"]
                or (default_provider and not default_provider_context_free)
                or runtime_read_provider
                or runtime_apply_provider
                or item["default_property"]
                or (record_callback and not record_callback_context_free)
            )
            if needs_device_context and not proxy and (
                not isinstance(device_label, str)
                or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", device_label)
            ):
                raise ValueError(
                    f"{'/'.join(canonical)}: device-backed settings need an actual-device with a devicetree label"
                )
            item_canonical = [module, group_name, item["name"], instance]
            item_macro = "DYNAMIC_SETTING_ID_" + c_macro_identifier(item_canonical)
            storage_contract = {
                "type": item["type"],
                "persistent": item["persistent"],
                "enum_values": item["enum_values"],
                "minimum": item["minimum"],
                "maximum": item["maximum"],
                "step": item["step"],
            }
            item_id = stable_id(item_canonical + [
                json.dumps(storage_contract, sort_keys=True, separators=(",", ":"))
            ])
            for macro_name in (item_macro, f"{item_macro}_{item_id:08X}"):
                previous = setting_macro_ids.get(macro_name)
                if previous is not None:
                    raise ValueError(
                        f"setting ID macro collision: {'/'.join(previous)} and "
                        f"{'/'.join(item_canonical)}"
                    )
                setting_macro_ids[macro_name] = item_canonical
            previous = setting_ids.get(item_id)
            if previous is not None:
                if previous != item_canonical:
                    raise ValueError(f"setting ID collision: {previous} and {item_canonical}")
                raise ValueError(f"duplicate setting: {'/'.join(item_canonical)}")
            setting_ids[item_id] = item_canonical
            item["id"] = item_id
            item.pop("group")
            item.pop("source")
            settings.append(item)
            definition["settings"].append({
                key: item[key]
                for key in (
                    "id",
                    "type",
                    "default",
                    "minimum",
                    "maximum",
                    "step",
                    "enum_values",
                    "text_length",
                    "persistent",
                    "read_only",
                    "reboot_required",
                    "runtime_field",
                )
            })
        graph_points = {}
        for item in settings:
            if item["widget"] != "graph":
                continue
            match = re.fullmatch(
                r"graph-([xy])-([1-9][0-9]*)", item["ui_component"] or ""
            )
            if match is None:
                raise ValueError(
                    f"{'/'.join(canonical)}/{item['name']}: invalid graph ui-component"
                )
            axis, point_index = match.groups()
            pair = graph_points.setdefault(point_index, {})
            if axis in pair:
                raise ValueError(
                    f"{'/'.join(canonical)}: graph point {point_index} has duplicate {axis.upper()} settings"
                )
            pair[axis] = item
        for point_index, pair in graph_points.items():
            if set(pair) != {"x", "y"}:
                raise ValueError(
                    f"{'/'.join(canonical)}: graph point {point_index} needs "
                    f"graph-x-{point_index} and graph-y-{point_index}"
                )
            if pair["x"]["ui_subgroup"] != pair["y"]["ui_subgroup"]:
                raise ValueError(
                    f"{'/'.join(canonical)}: graph point {point_index} settings "
                    "need the same ui-subgroup"
                )
        textbox_groups = {}
        for item in settings:
            if item["widget"] != "textbox":
                continue
            component = item["ui_component"] or ""
            match = re.fullmatch(r"textbox-(?:([a-z][a-z0-9_-]*)-)?([0-9]+)", component)
            if match is None or item["ui_subgroup"] is None:
                raise ValueError(
                    f"{'/'.join(canonical)}/{item['name']}: textbox settings need "
                    "textbox-N and ui-subgroup"
                )
            textbox_id, chunk_text = match.groups()
            chunk = int(chunk_text)
            textbox_key = (item["ui_subgroup"], textbox_id or "")
            textbox = textbox_groups.setdefault(textbox_key, {})
            if chunk in textbox:
                raise ValueError(
                    f"{'/'.join(canonical)}: duplicate textbox chunk {chunk} "
                    f"in ui-subgroup {item['ui_subgroup']!r}"
                )
            textbox[chunk] = item
        for (subgroup, textbox_id), textbox in textbox_groups.items():
            text_length = textbox.get(0, {}).get("text_length")
            expected_chunks = (text_length + 3) // 4 if text_length else 0
            if set(textbox) != set(range(expected_chunks)):
                raise ValueError(
                    f"{'/'.join(canonical)}: textbox {textbox_id!r} in ui-subgroup {subgroup!r} "
                    "must contain all textbox chunks required by textbox-0 text-length"
                )
        if is_record and settings:
            raise ValueError(f"{'/'.join(canonical)}: record groups cannot contain scalar settings")
        if not is_record and not settings:
            raise ValueError(f"{'/'.join(canonical)}: group must contain a setting")
        # Preserve child order for UI consumers; only the contract definition
        # below is canonicalized by setting ID.
        definition["settings"].sort(key=lambda item: item["id"])
        groups.append({
            "id": group_id,
            "module": module,
            "name": group_name,
            "instance": instance,
            "label": node_value(node, "label"),
            "ui_order": ui_order,
            "scope": scope,
            "format_version": format_version,
            "owner_contract_digest": (
                owner_contract_digest(group_id, format_version, definition)
                if scope == "owner"
                else "00" * 32
            ),
            "definition_hash": hashlib.sha256(
                json.dumps(definition, sort_keys=True, separators=(",", ":")).encode("utf-8")
            ).digest()[:8].hex(),
            "target_role": target,
            "target_source": target_source,
            "proxy": proxy,
            "default_provider": default_provider,
            "runtime_read_provider": runtime_read_provider,
            "runtime_apply_provider": runtime_apply_provider,
            "runtime_struct_header": runtime_struct_header,
            "runtime_struct_type": runtime_struct_type,
            "runtime_lock_field": runtime_lock_field,
            "record_type": record_type,
            "record_max_length": record_max_length,
            "record_default": record_default,
            "record_on_change": record_callback,
            "slot_index": slot_index,
            "device_label": device_label,
            "settings": settings,
        })
    # Keep the schema byte-for-byte stable across independently built peers,
    # while honoring explicit UI order for instances in the same group.
    groups.sort(key=lambda group: (
        group["module"], group["name"], group["ui_order"], group["instance"]
    ))
    setting_count = sum(len(group["settings"]) for group in groups)
    callback_count = sum(
        bool(setting["on_change"])
        for group in groups
        for setting in group["settings"]
    )
    if setting_count > MAX_SETTING_DESCRIPTORS or callback_count > MAX_CALLBACKS:
        raise ValueError("settings schema exceeds 16-bit descriptor index capacity")
    for group in groups:
        persistent_count = sum(
            bool(setting["persistent"]) for setting in group["settings"]
        )
        if persistent_count > MAX_PERSISTENT_SETTINGS_PER_GROUP:
            raise ValueError(
                f"group {group['module']}/{group['name']} has more than "
                f"{MAX_PERSISTENT_SETTINGS_PER_GROUP} persistent settings; "
                "the settings record stores its item count in one byte"
            )
    return {"format_version": 1, "groups": groups}


def write_artifacts(schema, output):
    output.mkdir(parents=True, exist_ok=True)
    ui_schema = {
        **schema,
        "groups": [
            {
                **{
                    key: value
                    for key, value in group.items()
                    if key not in (
                        "definition_hash", "device_label", "default_provider", "runtime_read_provider",
                        "runtime_apply_provider", "runtime_struct_header", "runtime_struct_type",
                        "runtime_lock_field", "target_source",
                        "record_default", "record_on_change", "slot_index",
                    )
                },
                # Proxy routing is local to the USB-attached endpoint. Keep
                # the displayed setting schema identical to its owner so the
                # peer schema hash only reports actual definition differences.
                "proxy": False,
                "settings": [
                    {
                        key: value
                        for key, value in setting.items()
                        if key not in ("on_change", "wire_default", "default_property", "runtime_field")
                    }
                    for setting in group["settings"]
                ],
            }
            for group in schema["groups"]
        ],
    }
    json_bytes = json.dumps(ui_schema, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    cbor_bytes = cbor(ui_schema)
    compressed = bytearray(gzip.compress(cbor_bytes, compresslevel=9, mtime=0))
    # Python/zlib versions may choose a platform-specific gzip OS marker.
    compressed[9] = 255
    digest = hashlib.sha256(cbor_bytes).digest()
    shared_contract = {
        "contract_version": 1,
        "groups": [
            {
                "id": group["id"],
                "format_version": group["format_version"],
                "definition_hash": group["definition_hash"],
            }
            for group in schema["groups"]
            if group["scope"] == "shared"
        ],
    }
    shared_contract_digest = hashlib.sha256(
        json.dumps(shared_contract, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).digest()
    (output / "settings-schema.json").write_bytes(json_bytes)
    (output / "settings-schema.cbor").write_bytes(cbor_bytes)
    (output / "settings-schema.cbor.gz").write_bytes(compressed)
    header = [
        "#pragma once",
        "#include <zmk/dynamic_settings/store.h>",
        "extern const uint8_t dynamic_settings_schema_sha256[32];",
        "extern const uint8_t dynamic_settings_shared_contract_sha256[32];",
        "extern const uint8_t dynamic_settings_schema_gzip[];",
        "extern const size_t dynamic_settings_schema_gzip_size;",
        "extern const size_t dynamic_settings_schema_size;",
    ]
    for group in schema["groups"]:
        macro = c_macro_identifier((group["module"], group["name"], group["instance"]))
        header.append(f"#define DYNAMIC_GROUP_ID_{macro} 0x{group['id']:08x}u")
        header.append(
            f"#define DYNAMIC_GROUP_ID_{macro}_{group['id']:08X} 0x{group['id']:08x}u"
        )
        for setting in group["settings"]:
            setting_macro = c_macro_identifier(
                (group["module"], group["name"], setting["name"], group["instance"])
            )
            header.append(
                f"#define DYNAMIC_SETTING_ID_{setting_macro} 0x{setting['id']:08x}u"
            )
            header.append(
                f"#define DYNAMIC_SETTING_ID_{setting_macro}_{setting['id']:08X} "
                f"0x{setting['id']:08x}u"
            )
    header.append("")
    header.extend([
        "extern const struct dynamic_setting_group_descriptor dynamic_setting_groups[];",
        "extern const size_t dynamic_setting_group_count;",
        "extern const struct dynamic_setting_descriptor dynamic_setting_descriptors[];",
        "extern const size_t dynamic_setting_descriptor_count;",
        "",
    ])
    (output / "settings_schema_generated.h").write_text("\n".join(header), encoding="utf-8")

    lines = [
        "#include <errno.h>",
        "#include <stddef.h>",
        "#include <zephyr/device.h>",
        "#include <zephyr/devicetree.h>",
        "#include <zephyr/sys/byteorder.h>",
        "#include <zmk/dynamic_settings/store.h>",
        '#include "settings_schema_generated.h"',
        "",
        f"const size_t dynamic_setting_group_count = {len(schema['groups'])}u;",
        f"const size_t dynamic_setting_descriptor_count = {sum(len(group['settings']) for group in schema['groups'])}u;",
    ]
    runtime_struct_headers = sorted({
        group["runtime_struct_header"]
        for group in schema["groups"]
        if group["runtime_struct_header"]
    })
    lines[0:0] = [f"#include <{include}>" for include in runtime_struct_headers]
    callbacks = [
        setting["on_change"]
        for group in schema["groups"]
        for setting in group["settings"]
        if setting["on_change"]
    ]
    callback_symbols = sorted(set(callbacks))
    runtime_apply_providers = sorted({
        group["runtime_apply_provider"]
        for group in schema["groups"]
        if group["runtime_apply_provider"]
    })
    record_callbacks = sorted({
        group["record_on_change"]
        for group in schema["groups"]
        if group["record_on_change"]
    })
    type_map = {name: name.upper() for name in VALUE_TYPES}
    type_map["bool"] = "BOOL"
    type_map["enum"] = "ENUM"
    for callback in callback_symbols:
        lines.append(f"extern dynamic_setting_on_change_fn {callback};")
    for provider in runtime_apply_providers:
        lines.append(f"extern dynamic_setting_on_change_fn {provider};")
    for callback in record_callbacks:
        lines.append(f"extern dynamic_setting_record_on_change_fn {callback};")
    default_providers = sorted({
        group["default_provider"]
        for group in schema["groups"]
        if group["default_provider"]
    })
    for provider in default_providers:
        lines.append(f"extern dynamic_setting_default_fn {provider};")
    runtime_read_providers = sorted({
        group["runtime_read_provider"]
        for group in schema["groups"]
        if group["runtime_read_provider"]
    })
    for provider in runtime_read_providers:
        lines.append(f"extern dynamic_setting_runtime_read_fn {provider};")
    generated_default_providers = []
    for group in schema["groups"]:
        if group["proxy"] or group["default_provider"] or not group["settings"]:
            continue
        provider = f"dynamic_settings_default_{group['id']:08x}"
        generated_default_providers.append((group, provider))
        lines.extend([
            "",
            f"static int {provider}(const struct dynamic_setting_descriptor *descriptor,",
            "                           struct dynamic_setting_value *value, const void *context) {",
            "    (void)context;",
            "    if (!descriptor || !value) {",
            "        return -EINVAL;",
            "    }",
            "    switch (descriptor->id) {",
        ])
        for setting in group["settings"]:
            default_property = setting["default_property"]
            if default_property and setting["type"] != "enum":
                property_name = re.sub(r"[^A-Za-z0-9_]", "_", default_property)
                node = f"DT_NODELABEL({group['device_label']})"
                default_expr = (
                    f"DT_PROP_OR({node}, {property_name}, 0)"
                    if setting["type"] == "bool"
                    else f"DT_PROP({node}, {property_name})"
                )
            else:
                default_expr = str(
                    int(setting["wire_default"])
                    if setting["type"] == "bool"
                    else setting["wire_default"]
                )
            length = setting["max_length"]
            type_name = type_map[setting["type"]]
            lines.extend([
                f"    case 0x{setting['id']:08x}u:",
                f"        value->type = DYNAMIC_SETTING_TYPE_{type_name};",
                f"        value->length = {length}u;",
            ])
            if length == 1:
                lines.append(f"        value->data[0] = (uint8_t)({default_expr});")
            else:
                lines.append(
                    f"        sys_put_le{length * 8}((uint{length * 8}_t)({default_expr}), value->data);"
                )
            lines.extend(["        return 0;", ""])
        lines.extend([
            "    default:",
            "        return -ENOENT;",
            "    }",
            "}",
        ])
    lines.extend([
        "",
    ])
    scratch_capacities = [
        (GROUP_RECORD_HEADER_SIZE + group["record_max_length"])
        if group["record_type"]
        else GROUP_RECORD_HEADER_SIZE + sum(
            5 + setting["max_length"]
            for setting in group["settings"]
            if setting["persistent"]
        )
        for group in schema["groups"]
        if not group["proxy"]
    ]
    shared_scratch_size = max(scratch_capacities, default=1)
    lines.extend([
        f"uint8_t dynamic_settings_storage_scratch[{shared_scratch_size}];",
        "const size_t dynamic_settings_storage_scratch_size =",
        "    sizeof(dynamic_settings_storage_scratch);",
        "",
    ])
    record_default_symbols = {}
    record_default_data = {}
    for group in schema["groups"]:
        if group["proxy"] or not group["record_type"] or not group["record_default"]:
            continue
        value = bytes(group["record_default"])
        symbol = f"dynamic_default_record_{hashlib.sha256(value).hexdigest()[:16]}"
        previous = record_default_data.get(symbol)
        if previous is not None and previous != value:
            raise ValueError(f"record default symbol hash collision: {symbol}")
        record_default_data[symbol] = value
        record_default_symbols[(group["id"])] = symbol
    for symbol, value in sorted(record_default_data.items()):
        initializer = ", ".join(f"0x{byte:02x}" for byte in value)
        lines.extend([
            f"static const uint8_t {symbol}[{len(value)}] = {{{initializer}}};",
        ])
    for group in schema["groups"]:
        if group["proxy"]:
            continue
        group_state = f"dynamic_state_{group['id']:08x}"
        if group["record_type"]:
            lines.extend([
                "",
                f"static uint8_t dynamic_record_data_{group['id']:08x}[{group['record_max_length']}];",
                f"static struct dynamic_setting_group_state {group_state} = {{",
                f"    .record_data = dynamic_record_data_{group['id']:08x},",
                "};",
            ])
            continue
        capacity = max(1, len(group["settings"]))
        value_states_size = max(1, (capacity + 3) // 4)
        lines.extend([
            "",
            f"struct {group_state}_value_slots {{",
        ])
        stored_settings = [setting for setting in group["settings"] if not setting["runtime_field"]]
        if stored_settings:
            for setting in stored_settings:
                lines.append(
                    f"    uint8_t slot_{setting['id']:08x}[{setting['max_length']}];"
                )
        else:
            lines.append("    uint8_t empty[1];")
        lines.extend([
            "};",
            f"static struct {group_state}_value_slots {group_state}_values;",
            f"static uint8_t {group_state}_value_states[{value_states_size}];",
            f"static struct dynamic_setting_group_state {group_state} = {{",
            f"    .values = (uint8_t *)&{group_state}_values,",
            f"    .value_states = {group_state}_value_states,",
            "};",
        ])
    lines.extend([
        "",
        "const struct dynamic_setting_group_descriptor dynamic_setting_groups[] = {",
    ])
    setting_offset = 0
    callback_offset = 0
    for group in schema["groups"]:
        group_path = "/".join(
            (group["module"], group["name"], group["instance"])
        )
        scope = {"local": 0, "shared": 1, "owner": 2}[group["scope"]]
        flags = (1 if group["proxy"] else 0) | (2 if group["record_type"] else 0)
        group_digest = ", ".join(
            f"0x{byte:02x}" for byte in bytes.fromhex(group["definition_hash"])
        )
        owner_contract_digest = ", ".join(
            f"0x{byte:02x}" for byte in bytes.fromhex(group["owner_contract_digest"])
        )
        generated_provider = next(
            (provider for owner, provider in generated_default_providers if owner is group),
            None,
        )
        provider = group["default_provider"] or generated_provider or "NULL"
        runtime_read_provider = group["runtime_read_provider"] or "NULL"
        callback_count = sum(bool(item["on_change"]) for item in group["settings"])
        context = (
            f"DEVICE_DT_GET(DT_NODELABEL({group['device_label']}))"
            if group["device_label"]
            else "NULL"
        )
        record_default_name = record_default_symbols.get(group["id"], "NULL")
        record_callback = group["record_on_change"] or "NULL"
        runtime_apply = group["runtime_apply_provider"] or "NULL"
        runtime_target_size = (
            f"sizeof(struct {group['runtime_struct_type']})"
            if group["runtime_struct_type"]
            else "0u"
        )
        runtime_lock_offset = (
            f"offsetof(struct {group['runtime_struct_type']}, {group['runtime_lock_field']})"
            if group["runtime_lock_field"]
            else "UINT16_MAX"
        )
        group_state = "NULL" if group["proxy"] else f"&dynamic_state_{group['id']:08x}"
        lines.append(
            f"    /* group id=0x{group['id']:08x}: {group_path} */"
        )
        lines.append(
            f"    {{ .id = 0x{group['id']:08x}u, .format_version = {group['format_version']}u, "
            f".slot_index = {group['slot_index']}u, .reserved = 0u, "
            f".scope = {scope}u, .flags = {flags}u, .definition_hash = {{{group_digest}}}, "
            f".target_source = {group['target_source'] if group['target_source'] is not None else 255}u, "
            f".owner_contract_digest = {{{owner_contract_digest}}}, "
            f".first_setting = {setting_offset}u, .setting_count = {len(group['settings'])}u, "
            f".record_max_length = {group['record_max_length'] or 0}u, "
            f".default_record_length = {len(group['record_default'] or [])}u, "
            f".runtime_target_size = {runtime_target_size}, "
            f".runtime_lock_offset = {runtime_lock_offset}, "
            f".first_callback = {callback_offset}u, .callback_count = {callback_count}u, "
            f".runtime_apply = {runtime_apply}, "
            f".get_default = {provider}, .runtime_read = {runtime_read_provider}, "
            f".record_on_change = {record_callback}, "
            f".default_record = {record_default_name}, .context = {context}, .state = {group_state} }},"
        )
        setting_offset += len(group["settings"])
        callback_offset += callback_count
    if not schema["groups"]:
        lines.append("    {0},")
    lines.append("};")
    for group in schema["groups"]:
        for setting in group["settings"]:
            if setting["runtime_field"]:
                field_size = (
                    f"sizeof(((struct {group['runtime_struct_type']} *)0)->"
                    f"{setting['runtime_field']})"
                )
                lines.append(
                    f"_Static_assert({field_size} == {setting['max_length']}u, "
                    f"\"{group['module']}/{group['name']}/{setting['name']} runtime field width mismatch\");"
                )
    lines.append("const struct dynamic_setting_descriptor dynamic_setting_descriptors[] = {")

    for group in schema["groups"]:
        ordered_settings = sorted(
            group["settings"], key=lambda setting: not bool(setting["on_change"])
        )
        for setting in ordered_settings:
            setting_path = "/".join(
                (group["module"], group["name"], group["instance"], setting["name"])
            )
            flags = (1 if setting["read_only"] else 0) | (2 if setting["persistent"] else 0)
            flags |= 4 if setting["reboot_required"] else 0
            type_name = type_map[setting["type"]]
            runtime_offset = "0u"
            runtime_size = "0u"
            if setting["runtime_field"]:
                struct_type = group["runtime_struct_type"]
                field = setting["runtime_field"]
                runtime_offset = f"offsetof(struct {struct_type}, {field})"
                runtime_size = f"sizeof(((struct {struct_type} *)0)->{field})"
            lines.append(
                f"    /* setting id=0x{setting['id']:08x}: {setting_path} */"
            )
            lines.append(
                f"    {{ .id = 0x{setting['id']:08x}u, .name = \"{setting['name']}\", "
                f".type = DYNAMIC_SETTING_TYPE_{type_name}, .flags = {flags}u, "
                f".runtime_offset = {runtime_offset}, .runtime_size = {runtime_size} }},"
            )
    if not schema["groups"]:
        lines.append("    {0},")
    lines.extend(["};", ""])
    lines.append("dynamic_setting_on_change_fn *const dynamic_setting_callbacks[] = {")
    lines.extend(f"    {callback}," for callback in callbacks)
    if not callbacks:
        lines.append("    NULL,")
    lines.extend(["};", f"const size_t dynamic_setting_callback_count = {len(callbacks)}u;", ""])
    lines.extend([
        f"const size_t dynamic_settings_schema_gzip_size = {len(compressed)}u;",
        f"const size_t dynamic_settings_schema_size = {len(cbor_bytes)}u;",
        "const uint8_t dynamic_settings_schema_sha256[32] = {",
        "    " + ", ".join(f"0x{byte:02x}" for byte in digest),
        "};",
        "const uint8_t dynamic_settings_shared_contract_sha256[32] = {",
        "    " + ", ".join(f"0x{byte:02x}" for byte in shared_contract_digest),
        "};",
        "const uint8_t dynamic_settings_schema_gzip[] = {",
    ])
    for offset in range(0, len(compressed), 12):
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in compressed[offset:offset + 12]) + ",")
    lines.extend(["};", ""])
    (output / "settings_schema_generated.c").write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--edt", type=Path, required=True)
    parser.add_argument("--zephyr", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--contributor", type=Path, action="append", default=[])
    args = parser.parse_args()
    sys.path.insert(0, str(args.zephyr / "scripts/dts/python-devicetree/src"))
    try:
        edt = pickle.loads(args.edt.read_bytes())
        layer_reordering = False
        if args.config and args.config.is_file():
            layer_reordering = any(
                line.strip() == "CONFIG_ZMK_KEYMAP_LAYER_REORDERING=y"
                for line in args.config.read_text(encoding="utf-8").splitlines()
            )
        config_values = {}
        if args.config and args.config.is_file():
            for line in args.config.read_text(encoding="utf-8").splitlines():
                match = re.fullmatch(r"(CONFIG_[A-Z0-9_]+)=(\d+|y|n)", line.strip())
                if match:
                    value = match.group(2)
                    config_values[match.group(1)] = (
                        int(value) if value.isdigit() else int(value == "y")
                    )
        schema = build_schema(
            edt,
            layer_reordering=layer_reordering,
            config_values=config_values,
            contributors=load_contributors(args.contributor),
        )
        write_artifacts(schema, args.out)
    except (OSError, ValueError, pickle.PickleError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
