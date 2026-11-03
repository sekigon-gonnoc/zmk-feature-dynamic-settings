/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include <zephyr/sys/iterable_sections.h>

#include <zmk/dynamic_settings/store.h>
#include <zmk/studio/rpc.h>

#include "settings_schema_generated.h"

LOG_MODULE_REGISTER(dynamic_settings_rpc, LOG_LEVEL_INF);

#define SETTINGS_SCHEMA_CHUNK_MAX 32

static struct {
    uint32_t group_id;
    uint32_t session_id;
    uint16_t total_length;
    bool active;
    bool relay_enabled;
} shared_record_relay;

static const struct dynamic_setting_descriptor *setting_by_id(uint32_t id) {
    for (size_t i = 0; i < dynamic_setting_descriptor_count; i++) {
        if (dynamic_setting_descriptors[i].id == id) {
            return &dynamic_setting_descriptors[i];
        }
    }
    return NULL;
}

static const struct dynamic_setting_group_descriptor *group_by_id(uint32_t id) {
    for (size_t i = 0; i < dynamic_setting_group_count; i++) {
        if (dynamic_setting_groups[i].id == id) {
            return &dynamic_setting_groups[i];
        }
    }
    return NULL;
}

static zmk_studio_SettingsError error_from_errno(int err) {
    switch (-err) {
    case 0:
        return zmk_studio_SettingsError_SETTINGS_ERROR_NONE;
    case EACCES:
    case EPERM:
        return zmk_studio_SettingsError_SETTINGS_ERROR_READ_ONLY;
    case ENOENT:
        return zmk_studio_SettingsError_SETTINGS_ERROR_NOT_FOUND;
    case ERANGE:
        return zmk_studio_SettingsError_SETTINGS_ERROR_OUT_OF_RANGE;
    case EINVAL:
        return zmk_studio_SettingsError_SETTINGS_ERROR_INVALID_ARGUMENT;
    case ENOSPC:
        return zmk_studio_SettingsError_SETTINGS_ERROR_NO_SPACE;
    case EBUSY:
        return zmk_studio_SettingsError_SETTINGS_ERROR_BUSY;
    case EHOSTUNREACH:
        return zmk_studio_SettingsError_SETTINGS_ERROR_OFFLINE;
    case ETIMEDOUT:
        return zmk_studio_SettingsError_SETTINGS_ERROR_TIMEOUT;
    case ESTALE:
        return zmk_studio_SettingsError_SETTINGS_ERROR_STALE;
    case ECANCELED:
    case EIO:
    case EOVERFLOW:
        return zmk_studio_SettingsError_SETTINGS_ERROR_IO;
    default:
        return zmk_studio_SettingsError_SETTINGS_ERROR_UNSUPPORTED;
    }
}

static zmk_studio_SettingsValueType wire_type(uint8_t type) {
    return (zmk_studio_SettingsValueType)type;
}

static bool target_matches(const struct dynamic_setting_group_descriptor *group,
                           zmk_studio_SettingsTarget target) {
    return group->scope == (uint8_t)target;
}

static int mirror_shared_operation(const zmk_studio_SettingsCommand *command,
                                   zmk_studio_SettingsRelayStatus *relay_status,
                                   uint8_t schema_sha256[32]) {
    struct dynamic_setting_remote_request request = {
        .operation = command->operation,
        .target = zmk_studio_SettingsTarget_SETTINGS_TARGET_SHARED,
        .value_type = command->value_type,
        .value_length = command->value.size,
        .session_id = command->session_id,
        .sequence = command->sequence,
        .group_id = command->group_id,
        .setting_id = command->setting_id,
        .offset = command->offset,
        .total_length = command->length,
    };
    const struct dynamic_setting_group_descriptor *group = group_by_id(command->group_id);
    if (group && (group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD)) {
        if (request.value_length > DYNAMIC_SETTINGS_REMOTE_VALUE_MAX) {
            return -EINVAL;
        }
    } else if (request.value_length > DYNAMIC_SETTINGS_VALUE_MAX) {
        return -EINVAL;
    }
    memcpy(request.value, command->value.bytes, request.value_length);
    struct dynamic_setting_remote_response response;
    int err = dynamic_settings_remote_call(&request, &response);
    if (err == -EHOSTUNREACH) {
        if (relay_status) {
            *relay_status = zmk_studio_SettingsRelayStatus_SETTINGS_RELAY_STATUS_LOCAL_ONLY;
        }
        return 0;
    }
    if (!err && relay_status) {
        *relay_status = zmk_studio_SettingsRelayStatus_SETTINGS_RELAY_STATUS_PEER_APPLIED;
    }
    if (!err && schema_sha256 && response.schema_known) {
        memcpy(schema_sha256, response.schema_sha256, sizeof(response.schema_sha256));
    }
    return err;
}

static zmk_studio_Response settings_response(const zmk_studio_SettingsCommand *command,
                                             zmk_studio_SettingsResponse response) {
    zmk_studio_Response outer = zmk_studio_Response_init_zero;
    outer.which_type = zmk_studio_Response_request_response_tag;
    outer.type.request_response.which_subsystem = zmk_studio_RequestResponse_settings_tag;
    outer.type.request_response.subsystem.settings.operation = command->operation;
    outer.type.request_response.subsystem.settings.session_id = command->session_id;
    outer.type.request_response.subsystem.settings.sequence = command->sequence;
    outer.type.request_response.subsystem.settings.error = response.error;
    outer.type.request_response.subsystem.settings.value_type = response.value_type;
    outer.type.request_response.subsystem.settings.origin = response.origin;
    outer.type.request_response.subsystem.settings.schema_compressed_size =
        response.schema_compressed_size;
    outer.type.request_response.subsystem.settings.schema_size = response.schema_size;
    memcpy(outer.type.request_response.subsystem.settings.schema_sha256, response.schema_sha256,
           sizeof(response.schema_sha256));
    outer.type.request_response.subsystem.settings.schema_data = response.schema_data;
    outer.type.request_response.subsystem.settings.value = response.value;
    outer.type.request_response.subsystem.settings.dirty = response.dirty;
    outer.type.request_response.subsystem.settings.persisted = response.persisted;
    outer.type.request_response.subsystem.settings.generation = response.generation;
    outer.type.request_response.subsystem.settings.relay_status = response.relay_status;
    return outer;
}

static zmk_studio_Response settings_command(const zmk_studio_Request *request) {
    const zmk_studio_SettingsCommand *command = &request->subsystem.settings.request_type.command;
    zmk_studio_SettingsResponse response = zmk_studio_SettingsResponse_init_zero;
    response.operation = command->operation;
    response.error = zmk_studio_SettingsError_SETTINGS_ERROR_NONE;
    response.session_id = command->session_id;
    response.sequence = command->sequence;

    if (command->operation != zmk_studio_SettingsOperation_SETTINGS_OPERATION_READ_SCHEMA) {
        bool by_setting =
            command->setting_id != 0 &&
            (command->operation == zmk_studio_SettingsOperation_SETTINGS_OPERATION_READ ||
             command->operation == zmk_studio_SettingsOperation_SETTINGS_OPERATION_WRITE_TEMP);
        const struct dynamic_setting_group_descriptor *group =
            by_setting ? dynamic_settings_find_setting_group(command->setting_id)
                       : group_by_id(command->group_id);
        if (!group || !target_matches(group, command->target)) {
            response.error = zmk_studio_SettingsError_SETTINGS_ERROR_NOT_FOUND;
            return settings_response(command, response);
        }
        if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY) {
            bool record_group = group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD;
            if (command->value.size >
                (record_group ? DYNAMIC_SETTINGS_REMOTE_VALUE_MAX : DYNAMIC_SETTINGS_VALUE_MAX)) {
                response.error = zmk_studio_SettingsError_SETTINGS_ERROR_INVALID_ARGUMENT;
                return settings_response(command, response);
            }
            struct dynamic_setting_remote_request remote_request = {
                .operation = command->operation,
                .target = command->target,
                .value_type = command->value_type,
                .value_length =
                    record_group && command->operation ==
                                        zmk_studio_SettingsOperation_SETTINGS_OPERATION_READ
                        ? command->length
                        : command->value.size,
                .session_id = command->session_id,
                .sequence = command->sequence,
                .group_id = command->group_id,
                .setting_id = command->setting_id,
                .offset = command->offset,
                .total_length = command->length,
                .target_source = group->target_source == UINT8_MAX ? 0 : group->target_source,
            };
            memcpy(remote_request.value, command->value.bytes, remote_request.value_length);
            struct dynamic_setting_remote_response remote_response;
            int err = dynamic_settings_remote_call(&remote_request, &remote_response);
            response.error = error_from_errno(err);
            if (!err) {
                response.value_type = wire_type(remote_response.value_type);
                response.schema_size = remote_response.total_length;
                response.value.size = remote_response.value_length;
                if (response.value.size > sizeof(response.value.bytes)) {
                    response.error = zmk_studio_SettingsError_SETTINGS_ERROR_IO;
                    response.value.size = 0;
                } else {
                    memcpy(response.value.bytes, remote_response.value, response.value.size);
                }
                response.origin =
                    remote_response.origin == 2
                        ? zmk_studio_SettingsValueOrigin_SETTINGS_VALUE_ORIGIN_TEMP
                    : remote_response.origin == 1
                        ? zmk_studio_SettingsValueOrigin_SETTINGS_VALUE_ORIGIN_PERSISTED
                        : zmk_studio_SettingsValueOrigin_SETTINGS_VALUE_ORIGIN_DEFAULT;
                response.dirty = remote_response.dirty;
                response.persisted = remote_response.persisted;
                if (remote_response.schema_known) {
                    memcpy(response.schema_sha256, remote_response.schema_sha256,
                           sizeof(response.schema_sha256));
                }
                response.generation = remote_response.generation;
            }
            return settings_response(command, response);
        }
    }

    switch (command->operation) {
    case zmk_studio_SettingsOperation_SETTINGS_OPERATION_READ_SCHEMA: {
        if (command->offset > dynamic_settings_schema_gzip_size ||
            command->length > SETTINGS_SCHEMA_CHUNK_MAX ||
            command->length > dynamic_settings_schema_gzip_size - command->offset) {
            response.error = zmk_studio_SettingsError_SETTINGS_ERROR_INVALID_ARGUMENT;
            break;
        }
        response.schema_compressed_size = dynamic_settings_schema_gzip_size;
        response.schema_size = dynamic_settings_schema_size;
        memcpy(response.schema_sha256, dynamic_settings_schema_sha256,
               sizeof(dynamic_settings_schema_sha256));
        response.schema_data.size = command->length;
        memcpy(response.schema_data.bytes, dynamic_settings_schema_gzip + command->offset,
               command->length);
        break;
    }
    case zmk_studio_SettingsOperation_SETTINGS_OPERATION_READ: {
        const struct dynamic_setting_group_descriptor *record_group =
            command->setting_id ? NULL : group_by_id(command->group_id);
        if (record_group && (record_group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD)) {
            uint8_t record[DYNAMIC_SETTINGS_REMOTE_VALUE_MAX];
            size_t total_length = 0;
            enum dynamic_setting_value_origin origin;
            if (command->length > sizeof(record)) {
                response.error = zmk_studio_SettingsError_SETTINGS_ERROR_INVALID_ARGUMENT;
                break;
            }
            int err = dynamic_settings_record_read(record_group->id, command->offset, record,
                                                   command->length, &total_length, &origin);
            response.error = error_from_errno(err);
            if (!err) {
                response.value.size = command->length;
                memcpy(response.value.bytes, record, command->length);
                response.schema_size = total_length;
                response.origin =
                    origin == DYNAMIC_SETTING_ORIGIN_TEMP
                        ? zmk_studio_SettingsValueOrigin_SETTINGS_VALUE_ORIGIN_TEMP
                    : origin == DYNAMIC_SETTING_ORIGIN_PERSISTED
                        ? zmk_studio_SettingsValueOrigin_SETTINGS_VALUE_ORIGIN_PERSISTED
                        : zmk_studio_SettingsValueOrigin_SETTINGS_VALUE_ORIGIN_DEFAULT;
                response.dirty = dynamic_settings_is_dirty(record_group->id);
                response.persisted = origin == DYNAMIC_SETTING_ORIGIN_PERSISTED;
                response.generation = dynamic_settings_generation(record_group->scope);
            }
            break;
        }
        LOG_INF("READ begin req=%u setting=%08x target=%u", request->request_id,
                command->setting_id, command->target);
        const struct dynamic_setting_descriptor *descriptor = setting_by_id(command->setting_id);
        const struct dynamic_setting_group_descriptor *group =
            descriptor ? dynamic_settings_find_setting_group(command->setting_id) : NULL;
        struct dynamic_setting_value value;
        enum dynamic_setting_value_origin origin;
        if (!descriptor || !group || !target_matches(group, command->target)) {
            response.error = zmk_studio_SettingsError_SETTINGS_ERROR_NOT_FOUND;
            LOG_INF("READ end req=%u result=not-found", request->request_id);
            break;
        }
        int err = dynamic_settings_read(command->setting_id, &value, &origin);
        if (err) {
            response.error = error_from_errno(err);
            LOG_INF("READ end req=%u result=error err=%d", request->request_id, err);
            break;
        }
        response.value_type = wire_type(value.type);
        response.origin = origin == DYNAMIC_SETTING_ORIGIN_TEMP
                              ? zmk_studio_SettingsValueOrigin_SETTINGS_VALUE_ORIGIN_TEMP
                          : origin == DYNAMIC_SETTING_ORIGIN_PERSISTED
                              ? zmk_studio_SettingsValueOrigin_SETTINGS_VALUE_ORIGIN_PERSISTED
                              : zmk_studio_SettingsValueOrigin_SETTINGS_VALUE_ORIGIN_DEFAULT;
        response.value.size = value.length;
        memcpy(response.value.bytes, value.data, value.length);
        response.dirty = dynamic_settings_is_dirty(group->id);
        response.persisted = origin == DYNAMIC_SETTING_ORIGIN_PERSISTED;
        response.generation = dynamic_settings_generation(group->scope);
        LOG_INF("READ end req=%u result=ok length=%u origin=%u", request->request_id, value.length,
                origin);
        break;
    }
    case zmk_studio_SettingsOperation_SETTINGS_OPERATION_WRITE_TEMP: {
        const struct dynamic_setting_group_descriptor *record_group =
            command->setting_id ? NULL : group_by_id(command->group_id);
        if (record_group && (record_group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD)) {
            if (command->value.size > DYNAMIC_SETTINGS_REMOTE_VALUE_MAX ||
                command->length > record_group->record_max_length) {
                response.error = zmk_studio_SettingsError_SETTINGS_ERROR_INVALID_ARGUMENT;
                break;
            }
            int err = dynamic_settings_record_write_temp_chunk(
                record_group->id, command->session_id, command->sequence, command->offset,
                command->length, command->value.bytes, command->value.size);
            bool chunk_accepted = err == 0 || err == -EINPROGRESS;
            bool record_complete = err == 0;
            if (err == -EINPROGRESS) {
                err = 0;
            }
            if (chunk_accepted && record_group->scope == DYNAMIC_SETTING_SCOPE_SHARED) {
                if (command->offset == 0) {
                    shared_record_relay.group_id = record_group->id;
                    shared_record_relay.session_id = command->session_id;
                    shared_record_relay.total_length = command->length;
                    shared_record_relay.active = true;
                    shared_record_relay.relay_enabled = true;
                } else if (!shared_record_relay.active ||
                           shared_record_relay.group_id != record_group->id ||
                           shared_record_relay.session_id != command->session_id ||
                           shared_record_relay.total_length != command->length) {
                    err = -ESTALE;
                }
                if (!err && shared_record_relay.relay_enabled) {
                    err = mirror_shared_operation(command, &response.relay_status, NULL);
                    if (err == -EINPROGRESS && !record_complete) {
                        err = 0;
                    } else if (
                        !err &&
                        response.relay_status ==
                            zmk_studio_SettingsRelayStatus_SETTINGS_RELAY_STATUS_LOCAL_ONLY) {
                        shared_record_relay.relay_enabled = false;
                    }
                } else if (!shared_record_relay.relay_enabled) {
                    response.relay_status =
                        zmk_studio_SettingsRelayStatus_SETTINGS_RELAY_STATUS_LOCAL_ONLY;
                }
                if (err || record_complete) {
                    shared_record_relay.active = false;
                }
            }
            response.error = error_from_errno(err);
            response.dirty = dynamic_settings_is_dirty(record_group->id);
            response.generation = dynamic_settings_generation(record_group->scope);
            break;
        }
        LOG_INF("WRITE begin req=%u setting=%08x target=%u length=%u", request->request_id,
                command->setting_id, command->target, command->value.size);
        const struct dynamic_setting_descriptor *descriptor = setting_by_id(command->setting_id);
        const struct dynamic_setting_group_descriptor *group =
            descriptor ? dynamic_settings_find_setting_group(command->setting_id) : NULL;
        if (!descriptor || !group || !target_matches(group, command->target) ||
            command->value.size > sizeof(((struct dynamic_setting_value *)0)->data)) {
            response.error = zmk_studio_SettingsError_SETTINGS_ERROR_INVALID_ARGUMENT;
            LOG_INF("WRITE end req=%u result=error code=%u", request->request_id, response.error);
            break;
        }
        struct dynamic_setting_value value = {
            .type = command->value_type,
            .length = command->value.size,
        };
        memcpy(value.data, command->value.bytes, value.length);
        int err = dynamic_settings_write_temp(command->setting_id, &value);
        if (!err && group->scope == DYNAMIC_SETTING_SCOPE_SHARED) {
            err = mirror_shared_operation(command, &response.relay_status, NULL);
        }
        response.error = error_from_errno(err);
        response.value_type = wire_type(value.type);
        response.dirty = dynamic_settings_is_dirty(group->id);
        response.generation = dynamic_settings_generation(group->scope);
        LOG_INF("WRITE end req=%u result=%s code=%u dirty=%u generation=%u", request->request_id,
                response.error == zmk_studio_SettingsError_SETTINGS_ERROR_NONE ? "ok" : "error",
                response.error, response.dirty, response.generation);
        break;
    }
    case zmk_studio_SettingsOperation_SETTINGS_OPERATION_RESET: {
        const struct dynamic_setting_group_descriptor *group = group_by_id(command->group_id);
        if (!group || !target_matches(group, command->target)) {
            response.error = zmk_studio_SettingsError_SETTINGS_ERROR_NOT_FOUND;
            break;
        }
        response.error = error_from_errno(dynamic_settings_reset(group->id, command->setting_id));
        if (response.error == zmk_studio_SettingsError_SETTINGS_ERROR_NONE &&
            group->scope == DYNAMIC_SETTING_SCOPE_SHARED) {
            response.error = error_from_errno(mirror_shared_operation(command, NULL, NULL));
        }
        response.dirty = dynamic_settings_is_dirty(group->id);
        response.generation = dynamic_settings_generation(group->scope);
        break;
    }
    case zmk_studio_SettingsOperation_SETTINGS_OPERATION_SAVE: {
        LOG_INF("SAVE begin req=%u group=%08x target=%u", request->request_id, command->group_id,
                command->target);
        const struct dynamic_setting_group_descriptor *group = group_by_id(command->group_id);
        if (!group || !target_matches(group, command->target)) {
            response.error = zmk_studio_SettingsError_SETTINGS_ERROR_NOT_FOUND;
            LOG_INF("SAVE end req=%u result=error code=%u", request->request_id, response.error);
            break;
        }
        if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY) {
            response.error = zmk_studio_SettingsError_SETTINGS_ERROR_OFFLINE;
            LOG_INF("SAVE end req=%u result=error code=%u", request->request_id, response.error);
            break;
        }
        if (group->scope == DYNAMIC_SETTING_SCOPE_SHARED) {
            response.error = error_from_errno(dynamic_settings_validate_scope_save(
                group->scope, group->id, command->session_id, command->sequence));
            if (response.error != zmk_studio_SettingsError_SETTINGS_ERROR_NONE) {
                LOG_INF("SAVE end req=%u result=error code=%u", request->request_id,
                        response.error);
                break;
            }
        }
        int save_err = dynamic_settings_save_scope(group->scope);
        if (save_err) {
            LOG_ERR("SAVE failed scope=%u err=%d", group->scope, save_err);
        }
        response.error = error_from_errno(save_err);
        response.dirty = dynamic_settings_scope_is_dirty(group->scope);
        response.persisted = dynamic_settings_scope_has_persisted_value(group->scope);
        response.generation = dynamic_settings_generation(group->scope);
        LOG_INF("SAVE end req=%u result=%s code=%u dirty=%u persisted=%u generation=%u",
                request->request_id,
                response.error == zmk_studio_SettingsError_SETTINGS_ERROR_NONE ? "ok" : "error",
                response.error, response.dirty, response.persisted, response.generation);
        break;
    }
    case zmk_studio_SettingsOperation_SETTINGS_OPERATION_DISCARD: {
        const struct dynamic_setting_group_descriptor *group = group_by_id(command->group_id);
        if (!group || !target_matches(group, command->target)) {
            response.error = zmk_studio_SettingsError_SETTINGS_ERROR_NOT_FOUND;
            break;
        }
        response.error = error_from_errno(dynamic_settings_discard(group->id));
        if (response.error == zmk_studio_SettingsError_SETTINGS_ERROR_NONE &&
            group->scope == DYNAMIC_SETTING_SCOPE_SHARED) {
            response.error =
                error_from_errno(mirror_shared_operation(command, &response.relay_status, NULL));
        }
        response.dirty = dynamic_settings_is_dirty(group->id);
        response.generation = dynamic_settings_generation(group->scope);
        break;
    }
    case zmk_studio_SettingsOperation_SETTINGS_OPERATION_STATUS: {
        const struct dynamic_setting_group_descriptor *group = group_by_id(command->group_id);
        if (!group || !target_matches(group, command->target)) {
            response.error = zmk_studio_SettingsError_SETTINGS_ERROR_NOT_FOUND;
            break;
        }
        if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY) {
            response.error = zmk_studio_SettingsError_SETTINGS_ERROR_OFFLINE;
            break;
        }
        if (group->scope == DYNAMIC_SETTING_SCOPE_SHARED) {
            response.error = error_from_errno(
                mirror_shared_operation(command, &response.relay_status, response.schema_sha256));
            if (response.error != zmk_studio_SettingsError_SETTINGS_ERROR_NONE) {
                break;
            }
        }
        response.dirty = dynamic_settings_is_dirty(group->id);
        response.persisted = dynamic_settings_group_has_persisted_value(group->id);
        response.generation = dynamic_settings_generation(group->scope);
        break;
    }
    default:
        response.error = zmk_studio_SettingsError_SETTINGS_ERROR_UNSUPPORTED;
        break;
    }
    return settings_response(command, response);
}

ZMK_RPC_SUBSYSTEM(settings)

STRUCT_SECTION_ITERABLE(zmk_rpc_subsystem_handler, settings_subsystem_handler_command) = {
    .func = settings_command,
    .subsystem_choice = zmk_studio_Request_settings_tag,
    .request_choice = zmk_studio_SettingsRequest_command_tag,
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
};
