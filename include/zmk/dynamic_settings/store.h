/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DYNAMIC_SETTINGS_VALUE_MAX 4
#define DYNAMIC_SETTINGS_RECORD_MAX_LENGTH 540
#define DYNAMIC_SETTINGS_REMOTE_VALUE_MAX 12
#define DYNAMIC_SETTINGS_DEFINITION_HASH_SIZE 8
#define DYNAMIC_SETTINGS_OWNER_CONTRACT_DIGEST_SIZE 32

enum dynamic_setting_value_type {
    DYNAMIC_SETTING_TYPE_BOOL = 1,
    DYNAMIC_SETTING_TYPE_U8 = 2,
    DYNAMIC_SETTING_TYPE_U16 = 3,
    DYNAMIC_SETTING_TYPE_U32 = 4,
    DYNAMIC_SETTING_TYPE_S8 = 5,
    DYNAMIC_SETTING_TYPE_S16 = 6,
    DYNAMIC_SETTING_TYPE_S32 = 7,
    DYNAMIC_SETTING_TYPE_ENUM = 8,
};

enum dynamic_setting_scope {
    DYNAMIC_SETTING_SCOPE_LOCAL,
    DYNAMIC_SETTING_SCOPE_SHARED,
    DYNAMIC_SETTING_SCOPE_OWNER,
};

enum dynamic_setting_group_flags {
    DYNAMIC_SETTING_GROUP_FLAG_PROXY = 1u << 0,
    DYNAMIC_SETTING_GROUP_FLAG_RECORD = 1u << 1,
};

enum dynamic_setting_change_reason {
    DYNAMIC_SETTING_CHANGE_BOOT_LOAD,
    DYNAMIC_SETTING_CHANGE_WRITE_TEMP,
    DYNAMIC_SETTING_CHANGE_RESET,
    DYNAMIC_SETTING_CHANGE_DISCARD,
    DYNAMIC_SETTING_CHANGE_PEER_APPLY,
};

struct dynamic_setting_descriptor;
struct dynamic_setting_group_descriptor;
struct dynamic_setting_value;
struct device;
typedef int(dynamic_setting_on_change_fn)(const struct dynamic_setting_descriptor *descriptor,
                                          const struct dynamic_setting_value *new_value,
                                          enum dynamic_setting_change_reason reason,
                                          const void *context);
typedef int(dynamic_setting_default_fn)(const struct dynamic_setting_descriptor *descriptor,
                                        struct dynamic_setting_value *value, const void *context);
typedef int(dynamic_setting_runtime_read_fn)(const struct dynamic_setting_descriptor *descriptor,
                                             struct dynamic_setting_value *value,
                                             const void *context);
typedef int(dynamic_setting_record_on_change_fn)(
    const struct dynamic_setting_group_descriptor *group, const uint8_t *record, size_t length,
    enum dynamic_setting_change_reason reason, const void *context);

enum dynamic_setting_value_origin {
    DYNAMIC_SETTING_ORIGIN_DEFAULT,
    DYNAMIC_SETTING_ORIGIN_PERSISTED,
    DYNAMIC_SETTING_ORIGIN_TEMP,
};

enum dynamic_setting_descriptor_flags {
    DYNAMIC_SETTING_FLAG_READ_ONLY = 1u << 0,
    DYNAMIC_SETTING_FLAG_PERSISTENT = 1u << 1,
    DYNAMIC_SETTING_FLAG_REBOOT_REQUIRED = 1u << 2,
};

struct dynamic_setting_value {
    uint8_t type;
    uint8_t length;
    uint8_t data[DYNAMIC_SETTINGS_VALUE_MAX];
};

/* Proxy groups are described by this device's ROM schema but their values live
 * on the owning keyboard. A transport module may register a synchronous relay.
 * Shared preview operations also use it to confirm peer application when
 * online. */
struct dynamic_setting_remote_request {
    uint8_t operation;
    uint8_t target;
    uint8_t value_type;
    uint8_t value_length;
    uint32_t session_id;
    uint32_t sequence;
    uint32_t group_id;
    uint32_t setting_id;
    uint16_t offset;
    uint16_t total_length;
    /* Transport source selected by a proxy's target. Ignored by local handlers.
     */
    uint8_t target_source;
    uint8_t value[DYNAMIC_SETTINGS_REMOTE_VALUE_MAX];
};

struct dynamic_setting_remote_response {
    uint8_t value_type;
    uint8_t value_length;
    uint8_t origin; /* 0=default, 1=persisted, 2=temp */
    bool dirty;
    bool persisted;
    bool schema_known;
    uint8_t schema_sha256[32];
    uint32_t generation;
    uint16_t total_length;
    uint8_t value[DYNAMIC_SETTINGS_REMOTE_VALUE_MAX];
};

typedef int(dynamic_setting_remote_fn)(const struct dynamic_setting_remote_request *request,
                                       struct dynamic_setting_remote_response *response,
                                       void *context);
typedef void(dynamic_settings_scope_saved_fn)(enum dynamic_setting_scope scope, void *context);
typedef int(dynamic_settings_scope_save_validator_fn)(enum dynamic_setting_scope scope,
                                                      uint32_t group_id, uint32_t session_id,
                                                      uint32_t sequence, void *context);
int dynamic_settings_register_remote(dynamic_setting_remote_fn *handler, void *context);
int dynamic_settings_remote_call(const struct dynamic_setting_remote_request *request,
                                 struct dynamic_setting_remote_response *response);
int dynamic_settings_register_scope_saved(dynamic_settings_scope_saved_fn *handler, void *context);
int dynamic_settings_register_scope_save_validator(
    dynamic_settings_scope_save_validator_fn *validator, void *context);
int dynamic_settings_validate_scope_save(enum dynamic_setting_scope scope, uint32_t group_id,
                                         uint32_t session_id, uint32_t sequence);

enum dynamic_setting_value_state {
    DYNAMIC_SETTING_VALUE_EMPTY,
    DYNAMIC_SETTING_VALUE_PERSISTED,
    DYNAMIC_SETTING_VALUE_TEMP,
};

struct __packed dynamic_setting_descriptor {
    uint32_t id;
    const char *name;
    uint8_t type;
    uint8_t flags;
    uint16_t runtime_offset;
    uint8_t runtime_size;
};

struct dynamic_setting_group_state {
    uint8_t *values;
    uint8_t *value_states;
    uint8_t *record_data;
    uint16_t record_length;
    uint8_t record_value_state;
    uint8_t record_readers;
    uint8_t persisted_count;
    bool dirty;
    bool shared_preview;
};

extern uint8_t dynamic_settings_storage_scratch[];
extern const size_t dynamic_settings_storage_scratch_size;

struct dynamic_setting_group_descriptor {
    uint32_t id;
    uint16_t format_version;
    uint8_t slot_index;
    /* Reserved for future descriptor metadata; generated as zero. */
    uint8_t reserved;
    uint8_t scope;
    uint8_t flags;
    /* Optional Nemu-Link source index for a proxy group; UINT8_MAX if unset. */
    uint8_t target_source;
    uint8_t definition_hash[DYNAMIC_SETTINGS_DEFINITION_HASH_SIZE];
    uint8_t owner_contract_digest[DYNAMIC_SETTINGS_OWNER_CONTRACT_DIGEST_SIZE];
    uint16_t first_setting;
    uint16_t setting_count;
    uint16_t record_max_length;
    uint16_t default_record_length;
    uint16_t runtime_target_size;
    uint16_t runtime_lock_offset;
    uint16_t first_callback;
    uint16_t callback_count;
    dynamic_setting_on_change_fn *runtime_apply;
    dynamic_setting_default_fn *get_default;
    dynamic_setting_runtime_read_fn *runtime_read;
    dynamic_setting_record_on_change_fn *record_on_change;
    const uint8_t *default_record;
    const void *context;
    struct dynamic_setting_group_state *state;
};

extern const struct dynamic_setting_group_descriptor dynamic_setting_groups[];
extern const size_t dynamic_setting_group_count;
extern const struct dynamic_setting_descriptor dynamic_setting_descriptors[];
extern const size_t dynamic_setting_descriptor_count;
extern dynamic_setting_on_change_fn *const dynamic_setting_callbacks[];
extern const size_t dynamic_setting_callback_count;

const struct dynamic_setting_group_descriptor *dynamic_settings_find_group(uint32_t group_id);
int dynamic_settings_owner_contract_digest(
    uint32_t group_id, uint8_t digest[DYNAMIC_SETTINGS_OWNER_CONTRACT_DIGEST_SIZE]);
const struct dynamic_setting_group_descriptor *
dynamic_settings_find_setting_group(uint32_t setting_id);
const struct dynamic_setting_descriptor *dynamic_settings_find_setting(uint32_t setting_id);
size_t dynamic_settings_shared_group_count(void);
const struct dynamic_setting_group_descriptor *dynamic_settings_shared_group_at(size_t index);
/* C2 saved-domain transport helpers. Buffers contain one validated ESBS group
 * record. */
int dynamic_settings_read_shared_group_record(uint32_t group_id, size_t offset, uint8_t *buffer,
                                              size_t size, size_t *record_size);
int dynamic_settings_apply_shared_group(uint32_t group_id, const uint8_t *buffer, size_t length);
int dynamic_settings_replace_shared_preview(uint32_t group_id, const uint8_t *buffer,
                                            size_t length);
int dynamic_settings_mark_shared_preview(uint32_t group_id, bool active);
bool dynamic_settings_is_shared_preview(uint32_t group_id);
bool dynamic_settings_shared_dirty_is_preview(void);

/* Read the effective value after Nemu-link settings preload has completed. */
int dynamic_settings_read(uint32_t setting_id, struct dynamic_setting_value *value,
                          enum dynamic_setting_value_origin *origin);
int dynamic_settings_record_read(uint32_t group_id, size_t offset, uint8_t *buffer, size_t size,
                                 size_t *record_length, enum dynamic_setting_value_origin *origin);
int dynamic_settings_record_acquire(uint32_t group_id, const uint8_t **record, size_t *length);
void dynamic_settings_record_release(uint32_t group_id);
int dynamic_settings_record_write_temp_chunk(uint32_t group_id, uint32_t session_id,
                                             uint32_t sequence, size_t offset, size_t total_length,
                                             const uint8_t *chunk, size_t chunk_length);
/* Apply reboot-required descriptors owned by a device during its init. */
int dynamic_settings_apply_init_required(const struct device *device);
int dynamic_settings_write_temp(uint32_t setting_id, const struct dynamic_setting_value *value);
int dynamic_settings_reset(uint32_t group_id, uint32_t setting_id);
int dynamic_settings_save_scope(enum dynamic_setting_scope scope);
int dynamic_settings_discard(uint32_t group_id);
bool dynamic_settings_is_dirty(uint32_t group_id);
bool dynamic_settings_group_has_persisted_value(uint32_t group_id);
bool dynamic_settings_scope_is_dirty(enum dynamic_setting_scope scope);
bool dynamic_settings_scope_has_persisted_value(enum dynamic_setting_scope scope);
uint32_t dynamic_settings_generation(enum dynamic_setting_scope scope);
