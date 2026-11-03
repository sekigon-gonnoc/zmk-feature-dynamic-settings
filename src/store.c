/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zmk/dynamic_settings/store.h>

LOG_MODULE_REGISTER(dynamic_settings, LOG_LEVEL_WRN);

#define DYNAMIC_SETTINGS_STORAGE_PREFIX "ds"
#define DYNAMIC_SETTINGS_RECORD_HEADER_SIZE 17
#define DYNAMIC_SETTINGS_RECORD_MAGIC "DREC"
#define DYNAMIC_SETTINGS_VALUES_MAGIC "DSV2"
static uint32_t scope_generations[3];
K_MUTEX_DEFINE(store_mutex);
K_MUTEX_DEFINE(operation_mutex);
static bool settings_preloading;
static bool settings_callbacks_enabled;
static int settings_preload_status = -EAGAIN;
static struct k_spinlock remote_handler_lock;
static dynamic_setting_remote_fn *remote_handler;
static void *remote_handler_context;
static dynamic_settings_scope_saved_fn *scope_saved_handler;
static void *scope_saved_context;
static dynamic_settings_scope_save_validator_fn *scope_save_validator;
static void *scope_save_validator_context;
static uint8_t record_staging[DYNAMIC_SETTINGS_RECORD_MAX_LENGTH];
static struct {
  const struct dynamic_setting_group_descriptor *group;
  uint32_t session_id;
  uint32_t sequence;
  uint16_t total_length;
  uint16_t received;
  uint16_t last_offset;
  uint8_t last_length;
  uint8_t last_chunk[DYNAMIC_SETTINGS_REMOTE_VALUE_MAX];
  uint32_t last_progress;
  bool active;
} record_rx;

static bool group_is_dirty(uint32_t group_id);
static int
reload_group_locked(const struct dynamic_setting_group_descriptor *group,
                    bool force);

#define lock_store() k_mutex_lock(&store_mutex, K_FOREVER)

int dynamic_settings_register_remote(dynamic_setting_remote_fn *handler,
                                     void *context) {
  if (!handler) {
    return -EINVAL;
  }
  k_spinlock_key_t key = k_spin_lock(&remote_handler_lock);
  if (remote_handler &&
      (remote_handler != handler || remote_handler_context != context)) {
    k_spin_unlock(&remote_handler_lock, key);
    return -EALREADY;
  }
  remote_handler = handler;
  remote_handler_context = context;
  k_spin_unlock(&remote_handler_lock, key);
  return 0;
}

int dynamic_settings_remote_call(
    const struct dynamic_setting_remote_request *request,
    struct dynamic_setting_remote_response *response) {
  if (!request || !response || request->value_length > sizeof(request->value)) {
    return -EINVAL;
  }
  k_spinlock_key_t key = k_spin_lock(&remote_handler_lock);
  dynamic_setting_remote_fn *handler = remote_handler;
  void *context = remote_handler_context;
  k_spin_unlock(&remote_handler_lock, key);
  if (!handler) {
    return -EHOSTUNREACH;
  }
  memset(response, 0, sizeof(*response));
  return handler(request, response, context);
}

int dynamic_settings_register_scope_saved(
    dynamic_settings_scope_saved_fn *handler, void *context) {
  if (!handler) {
    return -EINVAL;
  }
  k_spinlock_key_t key = k_spin_lock(&remote_handler_lock);
  if (scope_saved_handler &&
      (scope_saved_handler != handler || scope_saved_context != context)) {
    k_spin_unlock(&remote_handler_lock, key);
    return -EALREADY;
  }
  scope_saved_handler = handler;
  scope_saved_context = context;
  k_spin_unlock(&remote_handler_lock, key);
  return 0;
}

int dynamic_settings_register_scope_save_validator(
    dynamic_settings_scope_save_validator_fn *validator, void *context) {
  if (!validator) {
    return -EINVAL;
  }
  k_spinlock_key_t key = k_spin_lock(&remote_handler_lock);
  if (scope_save_validator && (scope_save_validator != validator ||
                               scope_save_validator_context != context)) {
    k_spin_unlock(&remote_handler_lock, key);
    return -EALREADY;
  }
  scope_save_validator = validator;
  scope_save_validator_context = context;
  k_spin_unlock(&remote_handler_lock, key);
  return 0;
}

int dynamic_settings_validate_scope_save(enum dynamic_setting_scope scope,
                                         uint32_t group_id, uint32_t session_id,
                                         uint32_t sequence) {
  k_spinlock_key_t key = k_spin_lock(&remote_handler_lock);
  dynamic_settings_scope_save_validator_fn *validator = scope_save_validator;
  void *context = scope_save_validator_context;
  k_spin_unlock(&remote_handler_lock, key);
  return validator ? validator(scope, group_id, session_id, sequence, context)
                   : 0;
}

const struct dynamic_setting_descriptor *
dynamic_settings_find_setting(uint32_t id) {
  for (size_t i = 0; i < dynamic_setting_descriptor_count; i++) {
    if (dynamic_setting_descriptors[i].id == id) {
      return &dynamic_setting_descriptors[i];
    }
  }
  return NULL;
}

const struct dynamic_setting_group_descriptor *
dynamic_settings_find_group(uint32_t id) {
  for (size_t i = 0; i < dynamic_setting_group_count; i++) {
    if (dynamic_setting_groups[i].id == id) {
      return &dynamic_setting_groups[i];
    }
  }
  return NULL;
}

int dynamic_settings_owner_contract_digest(
    uint32_t group_id,
    uint8_t digest[DYNAMIC_SETTINGS_OWNER_CONTRACT_DIGEST_SIZE]) {
  if (!digest) {
    return -EINVAL;
  }

  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  if (!group || group->scope != DYNAMIC_SETTING_SCOPE_OWNER) {
    return -ENOENT;
  }

  memcpy(digest, group->owner_contract_digest,
         DYNAMIC_SETTINGS_OWNER_CONTRACT_DIGEST_SIZE);
  return 0;
}

size_t dynamic_settings_shared_group_count(void) {
  size_t count = 0;
  for (size_t i = 0; i < dynamic_setting_group_count; i++) {
    if (dynamic_setting_groups[i].scope == DYNAMIC_SETTING_SCOPE_SHARED &&
        !(dynamic_setting_groups[i].flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY)) {
      count++;
    }
  }
  return count;
}

const struct dynamic_setting_group_descriptor *
dynamic_settings_shared_group_at(size_t index) {
  for (size_t i = 0; i < dynamic_setting_group_count; i++) {
    const struct dynamic_setting_group_descriptor *group =
        &dynamic_setting_groups[i];
    if (group->scope != DYNAMIC_SETTING_SCOPE_SHARED ||
        (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY)) {
      continue;
    }
    if (!index--) {
      return group;
    }
  }
  return NULL;
}

const struct dynamic_setting_group_descriptor *
dynamic_settings_find_setting_group(uint32_t setting_id) {
  const struct dynamic_setting_descriptor *descriptor =
      dynamic_settings_find_setting(setting_id);
  if (!descriptor) {
    return NULL;
  }

  size_t setting_index = (size_t)(descriptor - dynamic_setting_descriptors);
  for (size_t i = 0; i < dynamic_setting_group_count; i++) {
    const struct dynamic_setting_group_descriptor *group =
        &dynamic_setting_groups[i];
    if (setting_index >= group->first_setting &&
        setting_index < group->first_setting + group->setting_count) {
      return group;
    }
  }
  return NULL;
}

static const struct dynamic_setting_group_descriptor *
descriptor_group(const struct dynamic_setting_descriptor *descriptor) {
  if (!descriptor || descriptor < dynamic_setting_descriptors ||
      descriptor >=
          dynamic_setting_descriptors + dynamic_setting_descriptor_count) {
    return NULL;
  }

  size_t setting_index = (size_t)(descriptor - dynamic_setting_descriptors);
  for (size_t i = 0; i < dynamic_setting_group_count; i++) {
    const struct dynamic_setting_group_descriptor *group =
        &dynamic_setting_groups[i];
    if (setting_index >= group->first_setting &&
        setting_index < group->first_setting + group->setting_count) {
      return group;
    }
  }
  return NULL;
}

static dynamic_setting_on_change_fn *
descriptor_callback(const struct dynamic_setting_descriptor *descriptor) {
  const struct dynamic_setting_group_descriptor *group =
      descriptor_group(descriptor);
  if (!group || !descriptor) {
    return NULL;
  }
  size_t setting_index = (size_t)(descriptor - dynamic_setting_descriptors);
  size_t offset = setting_index - group->first_setting;
  if (offset >= group->callback_count ||
      group->first_callback + offset >= dynamic_setting_callback_count) {
    return NULL;
  }
  return dynamic_setting_callbacks[group->first_callback + offset];
}

static size_t
setting_offset(const struct dynamic_setting_group_descriptor *group,
               uint32_t id) {
  if (!group || !group->state) {
    return SIZE_MAX;
  }
  for (size_t i = 0; i < group->setting_count; i++) {
    if (dynamic_setting_descriptors[group->first_setting + i].id == id) {
      return i;
    }
  }
  return SIZE_MAX;
}

static uint8_t setting_value_size(uint8_t type) {
  switch (type) {
  case DYNAMIC_SETTING_TYPE_BOOL:
  case DYNAMIC_SETTING_TYPE_U8:
  case DYNAMIC_SETTING_TYPE_S8:
  case DYNAMIC_SETTING_TYPE_ENUM:
    return 1;
  case DYNAMIC_SETTING_TYPE_U16:
  case DYNAMIC_SETTING_TYPE_S16:
    return 2;
  case DYNAMIC_SETTING_TYPE_U32:
  case DYNAMIC_SETTING_TYPE_S32:
    return 4;
  default:
    return 0;
  }
}

static size_t
group_record_capacity(const struct dynamic_setting_group_descriptor *group) {
  if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) {
    return DYNAMIC_SETTINGS_RECORD_HEADER_SIZE + group->record_max_length;
  }
  size_t capacity = DYNAMIC_SETTINGS_RECORD_HEADER_SIZE;
  for (size_t i = 0; i < group->setting_count; i++) {
    const struct dynamic_setting_descriptor *descriptor =
        &dynamic_setting_descriptors[group->first_setting + i];
    if (descriptor->flags & DYNAMIC_SETTING_FLAG_PERSISTENT) {
      capacity += 5 + setting_value_size(descriptor->type);
    }
  }
  return capacity;
}

static uint8_t value_state(const struct dynamic_setting_group_descriptor *group,
                           size_t offset) {
  uint8_t shift = (offset % 4) * 2;
  return (group->state->value_states[offset / 4] >> shift) & 0x03;
}

static void
set_value_state(const struct dynamic_setting_group_descriptor *group,
                size_t offset, uint8_t state) {
  uint8_t shift = (offset % 4) * 2;
  uint8_t *byte = &group->state->value_states[offset / 4];
  *byte = (*byte & (uint8_t) ~(0x03u << shift)) | ((state & 0x03u) << shift);
}

static size_t
value_data_offset(const struct dynamic_setting_group_descriptor *group,
                  size_t offset) {
  size_t data_offset = 0;
  for (size_t i = 0; i < offset; i++) {
    const struct dynamic_setting_descriptor *descriptor =
        &dynamic_setting_descriptors[group->first_setting + i];
    if (!descriptor->runtime_size) {
      data_offset += setting_value_size(descriptor->type);
    }
  }
  return data_offset;
}

static struct k_spinlock *
runtime_lock(const struct dynamic_setting_group_descriptor *group,
             const struct device *device) {
  if (group->runtime_lock_offset == UINT16_MAX) {
    return NULL;
  }
  return (struct k_spinlock *)((uint8_t *)device->data +
                               group->runtime_lock_offset);
}

static bool
read_cached_value(const struct dynamic_setting_group_descriptor *group,
                  size_t offset, struct dynamic_setting_value *value) {
  const struct dynamic_setting_descriptor *descriptor =
      &dynamic_setting_descriptors[group->first_setting + offset];
  uint8_t state = value_state(group, offset);
  if (state == DYNAMIC_SETTING_VALUE_EMPTY) {
    return false;
  }
  value->type = descriptor->type;
  value->length = setting_value_size(descriptor->type);
  if (descriptor->runtime_size) {
    const struct device *device = group->context;
    if (!device || !device->data || descriptor->runtime_size != value->length ||
        descriptor->runtime_offset > group->runtime_target_size ||
        descriptor->runtime_size >
            group->runtime_target_size - descriptor->runtime_offset) {
      return false;
    }
    struct k_spinlock *lock = runtime_lock(group, device);
    if (lock) {
      k_spinlock_key_t key = k_spin_lock(lock);
      memcpy(value->data, (uint8_t *)device->data + descriptor->runtime_offset,
             value->length);
      k_spin_unlock(lock, key);
    } else {
      memcpy(value->data, (uint8_t *)device->data + descriptor->runtime_offset,
             value->length);
    }
    return true;
  }
  memcpy(value->data, &group->state->values[value_data_offset(group, offset)],
         value->length);
  return true;
}

static void
write_cached_value(const struct dynamic_setting_group_descriptor *group,
                   size_t offset, const struct dynamic_setting_value *value,
                   uint8_t state) {
  const struct dynamic_setting_descriptor *descriptor =
      &dynamic_setting_descriptors[group->first_setting + offset];
  if (!descriptor->runtime_size) {
    size_t data_offset = value_data_offset(group, offset);
    memcpy(&group->state->values[data_offset], value->data, value->length);
  }
  set_value_state(group, offset, state);
}

static void
clear_cached_value(const struct dynamic_setting_group_descriptor *group,
                   size_t offset) {
  set_value_state(group, offset, DYNAMIC_SETTING_VALUE_EMPTY);
}

static int validate_value(const struct dynamic_setting_descriptor *descriptor,
                          const struct dynamic_setting_value *value) {
  if (!descriptor || !value || !setting_value_size(descriptor->type) ||
      value->type != descriptor->type ||
      value->length != setting_value_size(descriptor->type) ||
      value->length > sizeof(value->data)) {
    return -EINVAL;
  }
  if (descriptor->type == DYNAMIC_SETTING_TYPE_BOOL && value->data[0] > 1) {
    return -ERANGE;
  }
  return 0;
}

static int default_value(const struct dynamic_setting_descriptor *descriptor,
                         struct dynamic_setting_value *value) {
  const struct dynamic_setting_group_descriptor *group =
      descriptor_group(descriptor);
  if (!descriptor || !group || !group->get_default) {
    return -ENOTSUP;
  }

  value->type = descriptor->type;
  value->length = setting_value_size(descriptor->type);
  memset(value->data, 0, sizeof(value->data));
  int err = group->get_default(descriptor, value, group->context);
  return err ? err : validate_value(descriptor, value);
}

/* Caller holds store_mutex. */
static int staged_value(const struct dynamic_setting_group_descriptor *group,
                        const struct dynamic_setting_descriptor *descriptor,
                        struct dynamic_setting_value *value) {
  size_t offset =
      (size_t)(descriptor - dynamic_setting_descriptors) - group->first_setting;
  if (read_cached_value(group, offset, value)) {
    return 0;
  }
  return default_value(descriptor, value);
}

static bool callback_applies_at_runtime(
    const struct dynamic_setting_descriptor *descriptor) {
  return (descriptor_callback(descriptor) || descriptor->runtime_size) &&
         !(descriptor->flags & DYNAMIC_SETTING_FLAG_REBOOT_REQUIRED);
}

static int apply_setting(const struct dynamic_setting_descriptor *descriptor,
                         const struct dynamic_setting_value *value,
                         enum dynamic_setting_change_reason reason,
                         const void *context) {
  dynamic_setting_on_change_fn *callback = descriptor_callback(descriptor);
  if (callback) {
    return callback(descriptor, value, reason, context);
  }
  if (!descriptor || !value || !descriptor->runtime_size ||
      descriptor->runtime_size != value->length) {
    return -EINVAL;
  }
  const struct dynamic_setting_group_descriptor *group =
      descriptor_group(descriptor);
  const struct device *device = context;
  if (!group || !device || !device->data ||
      descriptor->runtime_offset > group->runtime_target_size ||
      descriptor->runtime_size >
          group->runtime_target_size - descriptor->runtime_offset) {
    return -EINVAL;
  }
  uint8_t previous[DYNAMIC_SETTINGS_VALUE_MAX];
  uint8_t *target = (uint8_t *)device->data + descriptor->runtime_offset;
  struct k_spinlock *runtime_lock_ptr = runtime_lock(group, device);
  if (runtime_lock_ptr) {
    k_spinlock_key_t key = k_spin_lock(runtime_lock_ptr);
    memcpy(previous, target, descriptor->runtime_size);
    memcpy(target, value->data, descriptor->runtime_size);
    k_spin_unlock(runtime_lock_ptr, key);
  } else {
    memcpy(previous, target, descriptor->runtime_size);
    memcpy(target, value->data, descriptor->runtime_size);
  }
  int err = group->runtime_apply
                ? group->runtime_apply(descriptor, value, reason, context)
                : 0;
  if (err) {
    if (runtime_lock_ptr) {
      k_spinlock_key_t key = k_spin_lock(runtime_lock_ptr);
      memcpy(target, previous, descriptor->runtime_size);
      k_spin_unlock(runtime_lock_ptr, key);
    } else {
      memcpy(target, previous, descriptor->runtime_size);
    }
  }
  return err;
}

static int set_dirty(const struct dynamic_setting_group_descriptor *group) {
  if (!group || !group->state) {
    return -ENOTSUP;
  }
  group->state->dirty = true;
  return 0;
}

static bool value_equal(const struct dynamic_setting_value *a,
                        const struct dynamic_setting_value *b) {
  return a->type == b->type && a->length == b->length &&
         memcmp(a->data, b->data, a->length) == 0;
}

int dynamic_settings_read(uint32_t setting_id,
                          struct dynamic_setting_value *value,
                          enum dynamic_setting_value_origin *origin) {
  if (settings_preload_status) {
    return settings_preload_status;
  }

  const struct dynamic_setting_descriptor *descriptor =
      dynamic_settings_find_setting(setting_id);
  const struct dynamic_setting_group_descriptor *group =
      descriptor_group(descriptor);
  if (!descriptor || !value) {
    return -ENOENT;
  }
  if (!group || (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY)) {
    return -EHOSTUNREACH;
  }
  if (!group->state) {
    return -ENOTSUP;
  }
  if ((descriptor->flags & DYNAMIC_SETTING_FLAG_READ_ONLY) &&
      group->runtime_read) {
    int err = group->runtime_read(descriptor, value, group->context);
    if (err == 0) {
      if (origin) {
        *origin = DYNAMIC_SETTING_ORIGIN_TEMP;
      }
      return 0;
    }
    if (err != -ENOTSUP) {
      return err;
    }
  }
  lock_store();
  enum dynamic_setting_value_origin source = DYNAMIC_SETTING_ORIGIN_DEFAULT;
  size_t offset =
      (size_t)(descriptor - dynamic_setting_descriptors) - group->first_setting;
  uint8_t state = value_state(group, offset);
  if (state != DYNAMIC_SETTING_VALUE_EMPTY) {
    read_cached_value(group, offset, value);
    source = state == DYNAMIC_SETTING_VALUE_TEMP
                 ? DYNAMIC_SETTING_ORIGIN_TEMP
                 : DYNAMIC_SETTING_ORIGIN_PERSISTED;
  } else {
    int err = default_value(descriptor, value);
    if (err) {
      k_mutex_unlock(&store_mutex);
      return err;
    }
  }
  if (origin) {
    *origin = source;
  }
  k_mutex_unlock(&store_mutex);
  return 0;
}

int dynamic_settings_record_read(uint32_t group_id, size_t offset,
                                 uint8_t *buffer, size_t size,
                                 size_t *record_length,
                                 enum dynamic_setting_value_origin *origin) {
  if (settings_preload_status) {
    return settings_preload_status;
  }
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  if (!group || !(group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) ||
      !record_length || (!buffer && size)) {
    return -EINVAL;
  }
  if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY) {
    return -EHOSTUNREACH;
  }
  if (!group->state || !group->state->record_data) {
    return -ENOTSUP;
  }

  lock_store();
  const uint8_t *value = group->default_record;
  size_t length = group->default_record_length;
  uint8_t state = group->state->record_value_state;
  if (state != DYNAMIC_SETTING_VALUE_EMPTY) {
    value = group->state->record_data;
    length = group->state->record_length;
  }
  int err = 0;
  if (offset > length || size > length - offset) {
    err = -EINVAL;
  } else {
    if (size) {
      memcpy(buffer, value + offset, size);
    }
    *record_length = length;
    if (origin) {
      *origin = state == DYNAMIC_SETTING_VALUE_TEMP
                    ? DYNAMIC_SETTING_ORIGIN_TEMP
                : state == DYNAMIC_SETTING_VALUE_PERSISTED
                    ? DYNAMIC_SETTING_ORIGIN_PERSISTED
                    : DYNAMIC_SETTING_ORIGIN_DEFAULT;
    }
  }
  k_mutex_unlock(&store_mutex);
  return err;
}

int dynamic_settings_record_acquire(uint32_t group_id, const uint8_t **record,
                                    size_t *length) {
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  if (!group || !(group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) ||
      !record || !length) {
    return -EINVAL;
  }
  if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY) {
    return -EHOSTUNREACH;
  }
  if (!group->state || !group->state->record_data) {
    return -ENOTSUP;
  }

  /* Serialize pinning with record mutations, which hold operation_mutex. */
  k_mutex_lock(&operation_mutex, K_FOREVER);
  lock_store();
  if (group->state->record_readers == UINT8_MAX) {
    k_mutex_unlock(&store_mutex);
    k_mutex_unlock(&operation_mutex);
    return -EOVERFLOW;
  }
  bool use_default =
      group->state->record_value_state == DYNAMIC_SETTING_VALUE_EMPTY;
  *record = use_default ? group->default_record : group->state->record_data;
  *length =
      use_default ? group->default_record_length : group->state->record_length;
  group->state->record_readers++;
  k_mutex_unlock(&store_mutex);
  k_mutex_unlock(&operation_mutex);
  return 0;
}

void dynamic_settings_record_release(uint32_t group_id) {
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  if (!group || !(group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) ||
      !group->state) {
    return;
  }
  k_mutex_lock(&operation_mutex, K_FOREVER);
  lock_store();
  if (group->state->record_readers) {
    group->state->record_readers--;
  }
  k_mutex_unlock(&store_mutex);
  k_mutex_unlock(&operation_mutex);
}

static bool
group_record_is_pinned(const struct dynamic_setting_group_descriptor *group) {
  bool pinned;
  lock_store();
  pinned = group->state && group->state->record_readers;
  k_mutex_unlock(&store_mutex);
  return pinned;
}

static bool sequence_is_newer(uint32_t candidate, uint32_t current) {
  uint32_t distance = candidate - current;
  return distance && distance < 0x80000000u;
}

int dynamic_settings_record_write_temp_chunk(
    uint32_t group_id, uint32_t session_id, uint32_t sequence, size_t offset,
    size_t total_length, const uint8_t *chunk, size_t chunk_length) {
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  if (!group || !(group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) ||
      !session_id || !sequence || !chunk || !chunk_length ||
      chunk_length > DYNAMIC_SETTINGS_REMOTE_VALUE_MAX ||
      total_length > group->record_max_length ||
      total_length < group->default_record_length || offset > total_length ||
      chunk_length > total_length - offset) {
    return -EINVAL;
  }
  if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY) {
    return -EHOSTUNREACH;
  }
  if (!group->state || !group->state->record_data || !group->record_on_change) {
    return -ENOTSUP;
  }

  k_mutex_lock(&operation_mutex, K_FOREVER);
  if (group_record_is_pinned(group)) {
    record_rx.active = false;
    k_mutex_unlock(&operation_mutex);
    return -EBUSY;
  }
  uint32_t now = (uint32_t)k_uptime_get_32();
  if (record_rx.active && (uint32_t)(now - record_rx.last_progress) > 10000u) {
    record_rx.active = false;
    k_mutex_unlock(&operation_mutex);
    return -ETIMEDOUT;
  }
  bool starting = !record_rx.active;
  if (!starting && offset == 0) {
    bool exact_duplicate = record_rx.group == group &&
                           record_rx.session_id == session_id &&
                           record_rx.total_length == total_length &&
                           sequence == record_rx.sequence &&
                           chunk_length == record_rx.last_length &&
                           !memcmp(chunk, record_rx.last_chunk, chunk_length);
    if (exact_duplicate) {
      k_mutex_unlock(&operation_mutex);
      return 0;
    }
    if (record_rx.session_id == session_id &&
        !sequence_is_newer(sequence, record_rx.sequence)) {
      k_mutex_unlock(&operation_mutex);
      return -ESTALE;
    }
    record_rx.active = false;
    starting = true;
  }
  if (starting) {
    if (offset != 0) {
      k_mutex_unlock(&operation_mutex);
      return -ESTALE;
    }
    memset(&record_rx, 0, sizeof(record_rx));
    record_rx.group = group;
    record_rx.session_id = session_id;
    record_rx.total_length = total_length;
    record_rx.last_progress = now;
    record_rx.active = true;
  } else if (record_rx.group != group || record_rx.session_id != session_id ||
             record_rx.total_length != total_length) {
    k_mutex_unlock(&operation_mutex);
    return -EBUSY;
  }

  if (!starting && sequence == record_rx.sequence) {
    bool duplicate = offset == record_rx.last_offset &&
                     chunk_length == record_rx.last_length &&
                     !memcmp(chunk, record_rx.last_chunk, chunk_length);
    k_mutex_unlock(&operation_mutex);
    return duplicate ? 0 : -ESTALE;
  }
  if ((!starting && !sequence_is_newer(sequence, record_rx.sequence)) ||
      offset != record_rx.received) {
    k_mutex_unlock(&operation_mutex);
    return -ESTALE;
  }
  memcpy(record_staging + offset, chunk, chunk_length);
  record_rx.last_offset = offset;
  record_rx.last_length = chunk_length;
  memcpy(record_rx.last_chunk, chunk, chunk_length);
  record_rx.received += chunk_length;
  record_rx.sequence = sequence;
  record_rx.last_progress = now;
  if (record_rx.received != record_rx.total_length) {
    k_mutex_unlock(&operation_mutex);
    return -EINPROGRESS;
  }

  int err = group->record_on_change(group, record_staging, total_length,
                                    DYNAMIC_SETTING_CHANGE_WRITE_TEMP,
                                    group->context);
  if (!err) {
    lock_store();
    memcpy(group->state->record_data, record_staging, total_length);
    group->state->record_length = total_length;
    group->state->record_value_state = DYNAMIC_SETTING_VALUE_TEMP;
    group->state->dirty = true;
    k_mutex_unlock(&store_mutex);
  }
  record_rx.active = false;
  k_mutex_unlock(&operation_mutex);
  return err;
}

int dynamic_settings_apply_init_required(const struct device *device) {
  if (!device) {
    return -EINVAL;
  }

  for (size_t i = 0; i < dynamic_setting_descriptor_count; i++) {
    const struct dynamic_setting_descriptor *descriptor =
        &dynamic_setting_descriptors[i];
    const struct dynamic_setting_group_descriptor *group =
        descriptor_group(descriptor);
    if (!group || group->context != device ||
        (!descriptor_callback(descriptor) && !descriptor->runtime_size) ||
        !(descriptor->flags & DYNAMIC_SETTING_FLAG_REBOOT_REQUIRED)) {
      continue;
    }
    if (settings_preload_status) {
      return settings_preload_status;
    }

    struct dynamic_setting_value value;
    int err = dynamic_settings_read(descriptor->id, &value, NULL);
    if (err) {
      return err;
    }
    err = apply_setting(descriptor, &value, DYNAMIC_SETTING_CHANGE_BOOT_LOAD,
                        group->context);
    if (err) {
      LOG_ERR("settings init apply failed group=%08x err=%d", group->id, err);
      return err;
    }
  }
  return 0;
}

int dynamic_settings_write_temp(uint32_t setting_id,
                                const struct dynamic_setting_value *value) {
  const struct dynamic_setting_descriptor *descriptor =
      dynamic_settings_find_setting(setting_id);
  const struct dynamic_setting_group_descriptor *group =
      descriptor_group(descriptor);
  int err = validate_value(descriptor, value);
  if (err || !group) {
    return err ? err : -ENOENT;
  }
  if (descriptor->flags & DYNAMIC_SETTING_FLAG_READ_ONLY) {
    return -EACCES;
  }
  if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY) {
    return -EHOSTUNREACH;
  }
  if (!group->state) {
    return -ENOTSUP;
  }
  k_mutex_lock(&operation_mutex, K_FOREVER);
  lock_store();
  struct dynamic_setting_value old_value;
  size_t offset =
      (size_t)(descriptor - dynamic_setting_descriptors) - group->first_setting;
  uint8_t state = value_state(group, offset);
  if (state != DYNAMIC_SETTING_VALUE_EMPTY) {
    read_cached_value(group, offset, &old_value);
  } else {
    err = default_value(descriptor, &old_value);
    if (err) {
      k_mutex_unlock(&store_mutex);
      k_mutex_unlock(&operation_mutex);
      return err;
    }
  }
  k_mutex_unlock(&store_mutex);
  if (!value_equal(&old_value, value) &&
      callback_applies_at_runtime(descriptor)) {
    err = apply_setting(descriptor, value, DYNAMIC_SETTING_CHANGE_WRITE_TEMP,
                        group->context);
  }
  if (!err) {
    lock_store();
    if (offset == SIZE_MAX) {
      err = -ENOSPC;
    } else {
      write_cached_value(group, offset, value, DYNAMIC_SETTING_VALUE_TEMP);
      err = set_dirty(group);
    }
    k_mutex_unlock(&store_mutex);
  }
  k_mutex_unlock(&operation_mutex);
  return err;
}

int dynamic_settings_reset(uint32_t group_id, uint32_t setting_id) {
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  if (!group) {
    return -ENOENT;
  }
  if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY) {
    return -EHOSTUNREACH;
  }
  if (setting_id) {
    const struct dynamic_setting_descriptor *reset_descriptor = NULL;
    for (size_t i = 0; i < group->setting_count; i++) {
      const struct dynamic_setting_descriptor *candidate =
          &dynamic_setting_descriptors[group->first_setting + i];
      if (candidate->id == setting_id) {
        reset_descriptor = candidate;
        break;
      }
    }
    if (!reset_descriptor) {
      return -ENOENT;
    }
    if (reset_descriptor->flags & DYNAMIC_SETTING_FLAG_READ_ONLY) {
      return -EACCES;
    }
  }
  if (!group->state) {
    return -ENOTSUP;
  }
  if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) {
    if (setting_id) {
      return -ENOENT;
    }
    k_mutex_lock(&operation_mutex, K_FOREVER);
    if (group_record_is_pinned(group)) {
      k_mutex_unlock(&operation_mutex);
      return -EBUSY;
    }
    int err = group->record_on_change
                  ? group->record_on_change(group, group->default_record,
                                            group->default_record_length,
                                            DYNAMIC_SETTING_CHANGE_RESET,
                                            group->context)
                  : -ENOTSUP;
    if (!err) {
      lock_store();
      if (group->default_record_length) {
        memcpy(group->state->record_data, group->default_record,
               group->default_record_length);
      }
      group->state->record_length = group->default_record_length;
      group->state->record_value_state = DYNAMIC_SETTING_VALUE_TEMP;
      group->state->dirty = true;
      k_mutex_unlock(&store_mutex);
    }
    k_mutex_unlock(&operation_mutex);
    return err;
  }
  k_mutex_lock(&operation_mutex, K_FOREVER);
  size_t applied_through = 0;
  for (; applied_through < group->setting_count; applied_through++) {
    const struct dynamic_setting_descriptor *descriptor =
        &dynamic_setting_descriptors[group->first_setting + applied_through];
    if ((setting_id && descriptor->id != setting_id) ||
        (descriptor->flags & DYNAMIC_SETTING_FLAG_READ_ONLY)) {
      continue;
    }
    struct dynamic_setting_value target;
    struct dynamic_setting_value previous;
    int err = default_value(descriptor, &target);
    if (err) {
      k_mutex_unlock(&operation_mutex);
      return err;
    }
    lock_store();
    err = staged_value(group, descriptor, &previous);
    k_mutex_unlock(&store_mutex);
    if (err) {
      k_mutex_unlock(&operation_mutex);
      return err;
    }
    if (!value_equal(&previous, &target) &&
        callback_applies_at_runtime(descriptor)) {
      err = apply_setting(descriptor, &target, DYNAMIC_SETTING_CHANGE_RESET,
                          group->context);
      if (err) {
        k_mutex_unlock(&operation_mutex);
        return err;
      }
    }
    lock_store();
    size_t offset = applied_through;
    write_cached_value(group, offset, &target, DYNAMIC_SETTING_VALUE_TEMP);
    err = set_dirty(group);
    if (err) {
      k_mutex_unlock(&store_mutex);
      k_mutex_unlock(&operation_mutex);
      return err;
    }
    k_mutex_unlock(&store_mutex);
  }
  k_mutex_unlock(&operation_mutex);
  return 0;
}

static int
validate_group_record(const struct dynamic_setting_group_descriptor *group,
                      const uint8_t *blob, size_t len) {
  if (group && (group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD)) {
    if (!group->state || !blob || len < DYNAMIC_SETTINGS_RECORD_HEADER_SIZE ||
        len > group_record_capacity(group) ||
        memcmp(blob, DYNAMIC_SETTINGS_RECORD_MAGIC, 4) != 0 ||
        sys_get_le16(blob + 4) != group->format_version ||
        memcmp(blob + 6, group->definition_hash,
               DYNAMIC_SETTINGS_DEFINITION_HASH_SIZE) != 0 ||
        blob[14] != 1 ||
        sys_get_le16(blob + 15) != len - DYNAMIC_SETTINGS_RECORD_HEADER_SIZE ||
        sys_get_le16(blob + 15) > group->record_max_length ||
        sys_get_le16(blob + 15) < group->default_record_length) {
      return -EINVAL;
    }
    return 0;
  }
  if (!group || !group->state || !blob ||
      len < DYNAMIC_SETTINGS_RECORD_HEADER_SIZE ||
      memcmp(blob, DYNAMIC_SETTINGS_VALUES_MAGIC, 4) != 0 ||
      sys_get_le16(blob + 4) != group->format_version ||
      sys_get_le16(blob + 15) != len - DYNAMIC_SETTINGS_RECORD_HEADER_SIZE ||
      len > dynamic_settings_storage_scratch_size) {
    return -EINVAL;
  }

  size_t offset = DYNAMIC_SETTINGS_RECORD_HEADER_SIZE;
  for (uint8_t i = 0; i < blob[14]; i++) {
    if (offset + 5 > len) {
      return -EINVAL;
    }
    uint32_t id = sys_get_le32(blob + offset);
    uint8_t value_len = blob[offset + 4];
    offset += 5;
    if (value_len > DYNAMIC_SETTINGS_VALUE_MAX || offset + value_len > len) {
      return -EINVAL;
    }
    size_t previous_offset = DYNAMIC_SETTINGS_RECORD_HEADER_SIZE;
    for (uint8_t previous = 0; previous < i; previous++) {
      if (sys_get_le32(blob + previous_offset) == id) {
        return -EINVAL;
      }
      previous_offset += 5 + blob[previous_offset + 4];
    }
    const struct dynamic_setting_descriptor *descriptor =
        dynamic_settings_find_setting(id);
    if (!descriptor) {
      offset += value_len;
      continue;
    }
    if (descriptor_group(descriptor) != group ||
        !(descriptor->flags & DYNAMIC_SETTING_FLAG_PERSISTENT)) {
      return -EINVAL;
    }
    struct dynamic_setting_value value = {.type = descriptor->type,
                                          .length = value_len};
    memcpy(value.data, blob + offset, value_len);
    if (validate_value(descriptor, &value)) {
      return -EINVAL;
    }
    offset += value_len;
  }
  return offset == len ? 0 : -EINVAL;
}

static bool record_value(const uint8_t *blob, uint32_t id,
                         struct dynamic_setting_value *value) {
  size_t offset = DYNAMIC_SETTINGS_RECORD_HEADER_SIZE;
  for (uint8_t i = 0; i < blob[14]; i++) {
    uint32_t item_id = sys_get_le32(blob + offset);
    uint8_t value_len = blob[offset + 4];
    offset += 5;
    if (item_id == id) {
      value->length = value_len;
      memcpy(value->data, blob + offset, value_len);
      return true;
    }
    offset += value_len;
  }
  return false;
}

static int load_group(const char *name, size_t len, settings_read_cb read_cb,
                      void *cb_arg) {
  char *end;
  uint32_t group_id = strtoul(name, &end, 16);
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  if (!group || *end || !group->state ||
      len > dynamic_settings_storage_scratch_size ||
      len < DYNAMIC_SETTINGS_RECORD_HEADER_SIZE) {
    return 0;
  }
  uint8_t *blob = dynamic_settings_storage_scratch;
  ssize_t read_len = read_cb(cb_arg, blob, len);
  if (read_len < 0 || (size_t)read_len != len ||
      validate_group_record(group, blob, len)) {
    return 0;
  }

  if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) {
    size_t record_length = sys_get_le16(blob + 15);
    memcpy(group->state->record_data,
           blob + DYNAMIC_SETTINGS_RECORD_HEADER_SIZE, record_length);
    group->state->record_length = record_length;
    group->state->record_value_state = DYNAMIC_SETTING_VALUE_PERSISTED;
    group->state->persisted_count = 1;
    return 0;
  }

  group->state->persisted_count = 0;
  for (size_t i = 0; i < group->setting_count; i++) {
    if (value_state(group, i) == DYNAMIC_SETTING_VALUE_PERSISTED) {
      clear_cached_value(group, i);
    }
  }
  size_t offset = DYNAMIC_SETTINGS_RECORD_HEADER_SIZE;
  for (uint8_t i = 0; i < blob[14]; i++) {
    uint32_t id = sys_get_le32(blob + offset);
    uint8_t value_len = blob[offset + 4];
    offset += 5;
    size_t value_offset = offset;
    offset += value_len;
    size_t index = setting_offset(group, id);
    if (index == SIZE_MAX) {
      continue;
    }
    const struct dynamic_setting_descriptor *descriptor =
        &dynamic_setting_descriptors[group->first_setting + index];
    struct dynamic_setting_value value = {
        .type = descriptor->type,
        .length = value_len,
    };
    memcpy(value.data, blob + value_offset, value_len);
    write_cached_value(group, index, &value, DYNAMIC_SETTING_VALUE_PERSISTED);
    group->state->persisted_count++;
  }
  return 0;
}

struct direct_group_record_context {
  const struct dynamic_setting_group_descriptor *group;
  bool valid;
  int error;
};

static int load_group_direct(const char *name, size_t len,
                             settings_read_cb read_cb, void *cb_arg,
                             void *param) {
  struct direct_group_record_context *context = param;
  char expected_name[9];
  snprintk(expected_name, sizeof(expected_name), "%08x", context->group->id);
  if (!name || strcmp(name, expected_name) != 0) {
    return 0;
  }
  if (len > dynamic_settings_storage_scratch_size ||
      len < DYNAMIC_SETTINGS_RECORD_HEADER_SIZE) {
    return 0;
  }
  uint8_t *blob = dynamic_settings_storage_scratch;
  ssize_t read_len = read_cb(cb_arg, blob, len);
  if (read_len < 0) {
    context->error = (int)read_len;
    return 0;
  }
  if ((size_t)read_len != len) {
    context->error = -EIO;
    return 0;
  }
  context->valid = validate_group_record(context->group, blob, len) == 0;
  return 0;
}

static int
load_saved_group_record(const struct dynamic_setting_group_descriptor *group,
                        bool *valid) {
  struct direct_group_record_context context = {.group = group};
  int err = settings_load_subtree_direct(DYNAMIC_SETTINGS_STORAGE_PREFIX,
                                         load_group_direct, &context);
  if (err) {
    LOG_WRN("settings direct load failed group=%08x err=%d", group->id, err);
    return err;
  }
  if (context.error) {
    LOG_WRN("settings direct read failed group=%08x err=%d", group->id,
            context.error);
    return context.error;
  }
  *valid = context.valid;
  return 0;
}

static int load_generation(const char *name, size_t len,
                           settings_read_cb read_cb, void *cb_arg) {
  static const char *const scope_names[] = {"local", "shared", "owner"};
  uint8_t scope;
  for (scope = 0; scope < ARRAY_SIZE(scope_names); scope++) {
    if (strcmp(name, scope_names[scope]) == 0) {
      break;
    }
  }
  if (scope == ARRAY_SIZE(scope_names) || len != 8) {
    return 0;
  }
  uint8_t record[8];
  ssize_t read_len = read_cb(cb_arg, record, sizeof(record));
  if (read_len < 0 || (size_t)read_len != sizeof(record) ||
      memcmp(record, "ESBM", 4) != 0) {
    return 0;
  }
  scope_generations[scope] = sys_get_le32(record + 4);
  return 0;
}

static void apply_persisted_runtime_settings(void) {
  if (settings_preload_status) {
    return;
  }

  for (size_t group_index = 0; group_index < dynamic_setting_group_count;
       group_index++) {
    const struct dynamic_setting_group_descriptor *group =
        &dynamic_setting_groups[group_index];
    if (!group->state || (group->flags & (DYNAMIC_SETTING_GROUP_FLAG_PROXY |
                                          DYNAMIC_SETTING_GROUP_FLAG_RECORD))) {
      continue;
    }

    bool has_runtime_value = false;
    for (size_t i = 0; i < group->setting_count; i++) {
      const struct dynamic_setting_descriptor *descriptor =
          &dynamic_setting_descriptors[group->first_setting + i];
      has_runtime_value |=
          descriptor->runtime_size &&
          value_state(group, i) == DYNAMIC_SETTING_VALUE_PERSISTED;
    }
    bool has_saved_record = false;
    if (has_runtime_value &&
        load_saved_group_record(group, &has_saved_record) != 0) {
      LOG_WRN("settings runtime reload failed group=%08x", group->id);
      continue;
    }

    for (size_t i = 0; i < group->setting_count; i++) {
      const struct dynamic_setting_descriptor *descriptor =
          &dynamic_setting_descriptors[group->first_setting + i];
      if (!callback_applies_at_runtime(descriptor) ||
          value_state(group, i) != DYNAMIC_SETTING_VALUE_PERSISTED) {
        continue;
      }

      struct dynamic_setting_value value;
      bool found;
      if (descriptor->runtime_size) {
        found =
            has_saved_record && record_value(dynamic_settings_storage_scratch,
                                             descriptor->id, &value);
        value.type = descriptor->type;
      } else {
        found = read_cached_value(group, i, &value);
      }
      int err = found ? apply_setting(descriptor, &value,
                                      DYNAMIC_SETTING_CHANGE_BOOT_LOAD,
                                      group->context)
                      : -ENOENT;
      if (err) {
        LOG_WRN("settings boot apply failed group=%08x err=%d", group->id, err);
        clear_cached_value(group, i);
        if (group->state->persisted_count > 0) {
          group->state->persisted_count--;
        }
      }
    }
  }
}

static int commit_loaded(void) {
  if (!settings_preloading && settings_callbacks_enabled) {
    apply_persisted_runtime_settings();
  }

  for (size_t i = 0; i < dynamic_setting_group_count; i++) {
    const struct dynamic_setting_group_descriptor *group =
        &dynamic_setting_groups[i];
    if (!(group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) || !group->state ||
        group->state->record_value_state != DYNAMIC_SETTING_VALUE_PERSISTED ||
        !group->record_on_change) {
      continue;
    }
    int err = group->record_on_change(
        group, group->state->record_data, group->state->record_length,
        DYNAMIC_SETTING_CHANGE_BOOT_LOAD, group->context);
    if (err) {
      LOG_WRN("record boot apply failed group=%08x err=%d", group->id, err);
      group->state->record_value_state = DYNAMIC_SETTING_VALUE_EMPTY;
      group->state->record_length = group->default_record_length;
      group->state->persisted_count = 0;
    }
  }
  return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dynamic_settings,
                               DYNAMIC_SETTINGS_STORAGE_PREFIX, NULL,
                               load_group, commit_loaded, NULL);
SETTINGS_STATIC_HANDLER_DEFINE(dynamic_settings_meta,
                               DYNAMIC_SETTINGS_STORAGE_PREFIX "/meta", NULL,
                               load_generation, NULL, NULL);

static int dynamic_settings_preload(void) {
  int err = settings_subsys_init();
  if (err) {
    LOG_ERR("settings subsystem init failed: %d", err);
    return err;
  }

  settings_preloading = true;
  err = settings_load_subtree(DYNAMIC_SETTINGS_STORAGE_PREFIX);
  settings_preloading = false;
  settings_preload_status = err;
  if (err) {
    LOG_ERR("settings preload failed: %d", err);
  }
  return err;
}
SYS_INIT(dynamic_settings_preload, POST_KERNEL,
         CONFIG_ZMK_DYNAMIC_SETTINGS_INIT_PRIORITY);

static int dynamic_settings_enable_callbacks(void) {
  settings_callbacks_enabled = true;
  apply_persisted_runtime_settings();
  return 0;
}
SYS_INIT(dynamic_settings_enable_callbacks, APPLICATION, 0);

static bool group_is_dirty(uint32_t group_id) {
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  return group && group->state && group->state->dirty;
}

static int
save_group_record(const struct dynamic_setting_group_descriptor *group) {
  if (!group->state) {
    return -ENOTSUP;
  }
  uint8_t *blob = dynamic_settings_storage_scratch;
  if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) {
    if (group_record_capacity(group) > dynamic_settings_storage_scratch_size ||
        !group->state->record_data || !group->state->record_length) {
      return -ENOSPC;
    }
    size_t record_length = group->state->record_length;
    memcpy(blob, DYNAMIC_SETTINGS_RECORD_MAGIC, 4);
    sys_put_le16(group->format_version, blob + 4);
    memcpy(blob + 6, group->definition_hash,
           DYNAMIC_SETTINGS_DEFINITION_HASH_SIZE);
    blob[14] = 1;
    sys_put_le16(record_length, blob + 15);
    memcpy(blob + DYNAMIC_SETTINGS_RECORD_HEADER_SIZE,
           group->state->record_data, record_length);
    char full_key[sizeof(DYNAMIC_SETTINGS_STORAGE_PREFIX) + 1 + 8];
    snprintk(full_key, sizeof(full_key),
             DYNAMIC_SETTINGS_STORAGE_PREFIX "/%08x", group->id);
    return settings_save_one(
        full_key, blob, DYNAMIC_SETTINGS_RECORD_HEADER_SIZE + record_length);
  }
  size_t offset = DYNAMIC_SETTINGS_RECORD_HEADER_SIZE;
  uint8_t count = 0;
  memcpy(blob, DYNAMIC_SETTINGS_VALUES_MAGIC, 4);
  sys_put_le16(group->format_version, blob + 4);
  memcpy(blob + 6, group->definition_hash,
         DYNAMIC_SETTINGS_DEFINITION_HASH_SIZE);
  for (size_t i = 0; i < group->setting_count; i++) {
    const struct dynamic_setting_descriptor *descriptor =
        &dynamic_setting_descriptors[group->first_setting + i];
    if (!(descriptor->flags & DYNAMIC_SETTING_FLAG_PERSISTENT)) {
      continue;
    }
    size_t setting_index = i;
    struct dynamic_setting_value value;
    if (!read_cached_value(group, setting_index, &value)) {
      continue;
    }
    struct dynamic_setting_value defaulted;
    int err = default_value(descriptor, &defaulted);
    if (err) {
      return err;
    }
    if (value_equal(&value, &defaulted)) {
      continue;
    }
    if (offset + 5 + value.length > group_record_capacity(group) ||
        count == UINT8_MAX) {
      return -ENOSPC;
    }
    sys_put_le32(descriptor->id, blob + offset);
    blob[offset + 4] = value.length;
    offset += 5;
    memcpy(blob + offset, value.data, value.length);
    offset += value.length;
    count++;
  }
  blob[14] = count;
  sys_put_le16(offset - DYNAMIC_SETTINGS_RECORD_HEADER_SIZE, blob + 15);
  char full_key[sizeof(DYNAMIC_SETTINGS_STORAGE_PREFIX) + 1 + 8];
  snprintk(full_key, sizeof(full_key), DYNAMIC_SETTINGS_STORAGE_PREFIX "/%08x",
           group->id);
  return settings_save_one(full_key, blob, offset);
}

static int
encode_shared_group_locked(const struct dynamic_setting_group_descriptor *group,
                           size_t *encoded_size) {
  if (!group->state || !encoded_size ||
      group_record_capacity(group) > dynamic_settings_storage_scratch_size) {
    return -ENOTSUP;
  }
  uint8_t *blob = dynamic_settings_storage_scratch;
  if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) {
    size_t record_length =
        group->state->record_value_state == DYNAMIC_SETTING_VALUE_EMPTY
            ? group->default_record_length
            : group->state->record_length;
    const uint8_t *record =
        group->state->record_value_state == DYNAMIC_SETTING_VALUE_EMPTY
            ? group->default_record
            : group->state->record_data;
    memcpy(blob, DYNAMIC_SETTINGS_RECORD_MAGIC, 4);
    sys_put_le16(group->format_version, blob + 4);
    memcpy(blob + 6, group->definition_hash,
           DYNAMIC_SETTINGS_DEFINITION_HASH_SIZE);
    blob[14] = 1;
    sys_put_le16(record_length, blob + 15);
    if (record_length) {
      memcpy(blob + DYNAMIC_SETTINGS_RECORD_HEADER_SIZE, record, record_length);
    }
    *encoded_size = DYNAMIC_SETTINGS_RECORD_HEADER_SIZE + record_length;
    return 0;
  }
  size_t offset = DYNAMIC_SETTINGS_RECORD_HEADER_SIZE;
  uint8_t count = 0;
  memcpy(blob, DYNAMIC_SETTINGS_VALUES_MAGIC, 4);
  sys_put_le16(group->format_version, blob + 4);
  memcpy(blob + 6, group->definition_hash,
         DYNAMIC_SETTINGS_DEFINITION_HASH_SIZE);
  for (size_t i = 0; i < group->setting_count; i++) {
    const struct dynamic_setting_descriptor *descriptor =
        &dynamic_setting_descriptors[group->first_setting + i];
    struct dynamic_setting_value value;
    if (!(descriptor->flags & DYNAMIC_SETTING_FLAG_PERSISTENT) ||
        value_state(group, i) != DYNAMIC_SETTING_VALUE_PERSISTED ||
        !read_cached_value(group, i, &value)) {
      continue;
    }
    struct dynamic_setting_value defaulted;
    int err = default_value(descriptor, &defaulted);
    if (err) {
      return err;
    }
    if (value_equal(&value, &defaulted)) {
      continue;
    }
    if (offset + 5 + value.length > group_record_capacity(group) ||
        count == UINT8_MAX) {
      return -ENOSPC;
    }
    sys_put_le32(descriptor->id, blob + offset);
    blob[offset + 4] = value.length;
    offset += 5;
    memcpy(blob + offset, value.data, value.length);
    offset += value.length;
    count++;
  }
  blob[14] = count;
  sys_put_le16(offset - DYNAMIC_SETTINGS_RECORD_HEADER_SIZE, blob + 15);
  *encoded_size = offset;
  return 0;
}

int dynamic_settings_read_shared_group_record(uint32_t group_id, size_t offset,
                                              uint8_t *buffer, size_t size,
                                              size_t *record_size) {
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  if (!group || group->scope != DYNAMIC_SETTING_SCOPE_SHARED ||
      (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY)) {
    return -ENOENT;
  }
  if ((!buffer && size) || !record_size) {
    return -EINVAL;
  }
  k_mutex_lock(&operation_mutex, K_FOREVER);
  lock_store();
  size_t encoded_size = 0;
  int err = group_is_dirty(group_id) ? -EBUSY : 0;
  if (!err) {
    err = encode_shared_group_locked(group, &encoded_size);
  }
  if (!err && (offset > encoded_size || size > encoded_size - offset)) {
    err = -EINVAL;
  }
  if (!err) {
    if (size) {
      memcpy(buffer, dynamic_settings_storage_scratch + offset, size);
    }
    *record_size = encoded_size;
  }
  k_mutex_unlock(&store_mutex);
  k_mutex_unlock(&operation_mutex);
  return err;
}

static int apply_shared_group(uint32_t group_id, const uint8_t *buffer,
                              size_t length, bool replace_preview) {
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  if (!group || group->scope != DYNAMIC_SETTING_SCOPE_SHARED ||
      (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY)) {
    return -ENOENT;
  }
  if (!buffer || length > dynamic_settings_storage_scratch_size ||
      length < DYNAMIC_SETTINGS_RECORD_HEADER_SIZE) {
    return -EINVAL;
  }
  k_mutex_lock(&operation_mutex, K_FOREVER);
  if ((group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) &&
      group_record_is_pinned(group)) {
    k_mutex_unlock(&operation_mutex);
    return -EBUSY;
  }
  lock_store();
  int err = group_is_dirty(group_id) && !replace_preview ? -EBUSY : 0;
  if (!err) {
    if (buffer != dynamic_settings_storage_scratch) {
      memcpy(dynamic_settings_storage_scratch, buffer, length);
    }
    err =
        validate_group_record(group, dynamic_settings_storage_scratch, length);
  }
  if (!err) {
    char key[32];
    snprintk(key, sizeof(key), DYNAMIC_SETTINGS_STORAGE_PREFIX "/%08x",
             group_id);
    err = ((group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) ||
           dynamic_settings_storage_scratch[14])
              ? settings_save_one(key, dynamic_settings_storage_scratch, length)
              : settings_delete(key);
  }
  k_mutex_unlock(&store_mutex);
  if (!err) {
    err = reload_group_locked(group, true);
  }
  if (!err) {
    lock_store();
    group->state->shared_preview = false;
    k_mutex_unlock(&store_mutex);
  }
  k_mutex_unlock(&operation_mutex);
  return err;
}

int dynamic_settings_apply_shared_group(uint32_t group_id,
                                        const uint8_t *buffer, size_t length) {
  return apply_shared_group(group_id, buffer, length, false);
}

int dynamic_settings_replace_shared_preview(uint32_t group_id,
                                            const uint8_t *buffer,
                                            size_t length) {
  return apply_shared_group(group_id, buffer, length, true);
}

int dynamic_settings_mark_shared_preview(uint32_t group_id, bool active) {
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  if (!group || group->scope != DYNAMIC_SETTING_SCOPE_SHARED || !group->state) {
    return -ENOENT;
  }
  lock_store();
  group->state->shared_preview = active;
  k_mutex_unlock(&store_mutex);
  return 0;
}

bool dynamic_settings_is_shared_preview(uint32_t group_id) {
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  if (!group || group->scope != DYNAMIC_SETTING_SCOPE_SHARED || !group->state) {
    return false;
  }
  lock_store();
  bool active = group->state->shared_preview;
  k_mutex_unlock(&store_mutex);
  return active;
}

bool dynamic_settings_shared_dirty_is_preview(void) {
  bool confirmed = true;
  lock_store();
  for (size_t i = 0; i < dynamic_setting_group_count; i++) {
    const struct dynamic_setting_group_descriptor *group =
        &dynamic_setting_groups[i];
    if (group->scope == DYNAMIC_SETTING_SCOPE_SHARED && group->state &&
        group->state->dirty && !group->state->shared_preview) {
      confirmed = false;
      break;
    }
  }
  k_mutex_unlock(&store_mutex);
  return confirmed;
}

static int save_scope_generation(enum dynamic_setting_scope scope,
                                 uint32_t generation) {
  static const char *const scope_names[] = {"local", "shared", "owner"};
  uint8_t record[8] = {'E', 'S', 'B', 'M'};
  sys_put_le32(generation, record + 4);
  char key[40];
  snprintk(key, sizeof(key), DYNAMIC_SETTINGS_STORAGE_PREFIX "/meta/%s",
           scope_names[scope]);
  return settings_save_one(key, record, sizeof(record));
}

static int
commit_saved_group(const struct dynamic_setting_group_descriptor *group) {
  if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) {
    if (group->state->record_value_state != DYNAMIC_SETTING_VALUE_EMPTY) {
      group->state->record_value_state = DYNAMIC_SETTING_VALUE_PERSISTED;
      group->state->persisted_count = 1;
    }
    group->state->dirty = false;
    return 0;
  }
  uint8_t persisted_count = 0;
  for (size_t i = 0; i < group->setting_count; i++) {
    const struct dynamic_setting_descriptor *descriptor =
        &dynamic_setting_descriptors[group->first_setting + i];
    uint8_t state = value_state(group, i);
    if (!(descriptor->flags & DYNAMIC_SETTING_FLAG_PERSISTENT)) {
      continue;
    }
    struct dynamic_setting_value value;
    bool has_value = read_cached_value(group, i, &value);
    if (state == DYNAMIC_SETTING_VALUE_TEMP) {
      if (!has_value) {
        continue;
      }
    } else if (state != DYNAMIC_SETTING_VALUE_PERSISTED || !has_value) {
      continue;
    }
    struct dynamic_setting_value defaulted;
    int err = default_value(descriptor, &defaulted);
    if (err) {
      return err;
    }
    if (value_equal(&value, &defaulted)) {
      clear_cached_value(group, i);
    } else {
      set_value_state(group, i, DYNAMIC_SETTING_VALUE_PERSISTED);
      persisted_count++;
    }
  }
  group->state->persisted_count = persisted_count;
  group->state->dirty = false;
  return 0;
}

int dynamic_settings_save_scope(enum dynamic_setting_scope scope) {
  if (scope < DYNAMIC_SETTING_SCOPE_LOCAL ||
      scope > DYNAMIC_SETTING_SCOPE_OWNER) {
    return -EINVAL;
  }
  k_mutex_lock(&operation_mutex, K_FOREVER);
  lock_store();
  bool any_dirty = false;
  for (size_t i = 0; i < dynamic_setting_group_count; i++) {
    const struct dynamic_setting_group_descriptor *group =
        &dynamic_setting_groups[i];
    if (group->scope == scope &&
        !(group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY) &&
        group_is_dirty(group->id)) {
      any_dirty = true;
      break;
    }
  }
  int err = 0;
  if (any_dirty && scope_generations[scope] == UINT32_MAX) {
    err = -EOVERFLOW;
  }
  for (size_t i = 0; !err && any_dirty && i < dynamic_setting_group_count;
       i++) {
    const struct dynamic_setting_group_descriptor *group =
        &dynamic_setting_groups[i];
    if (group->scope == scope &&
        !(group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY) &&
        group_is_dirty(group->id)) {
      err = save_group_record(group);
    }
  }
  if (!err && any_dirty) {
    uint32_t next_generation = scope_generations[scope] + 1;
    err = save_scope_generation(scope, next_generation);
    if (!err) {
      scope_generations[scope] = next_generation;
      for (size_t i = 0; i < dynamic_setting_group_count; i++) {
        const struct dynamic_setting_group_descriptor *group =
            &dynamic_setting_groups[i];
        if (group->scope == scope &&
            !(group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY) &&
            group_is_dirty(group->id)) {
          err = commit_saved_group(group);
          if (err) {
            break;
          }
        }
      }
    }
  }
  k_mutex_unlock(&store_mutex);
  k_mutex_unlock(&operation_mutex);
  if (!err && any_dirty) {
    k_spinlock_key_t key = k_spin_lock(&remote_handler_lock);
    dynamic_settings_scope_saved_fn *handler = scope_saved_handler;
    void *context = scope_saved_context;
    k_spin_unlock(&remote_handler_lock, key);
    if (handler) {
      handler(scope, context);
    }
  }
  return err;
}

static int
reload_group_locked(const struct dynamic_setting_group_descriptor *group,
                    bool force) {
  if ((group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) &&
      group_record_is_pinned(group)) {
    return -EBUSY;
  }
  bool has_saved_record = false;
  int load_err = load_saved_group_record(group, &has_saved_record);
  if (load_err) {
    return load_err;
  }
  if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_RECORD) {
    size_t target_length =
        has_saved_record ? sys_get_le16(dynamic_settings_storage_scratch + 15)
                         : group->default_record_length;
    const uint8_t *target = has_saved_record
                                ? dynamic_settings_storage_scratch +
                                      DYNAMIC_SETTINGS_RECORD_HEADER_SIZE
                                : group->default_record;
    if (group->record_on_change) {
      int err =
          group->record_on_change(group, target, target_length,
                                  force ? DYNAMIC_SETTING_CHANGE_PEER_APPLY
                                        : DYNAMIC_SETTING_CHANGE_DISCARD,
                                  group->context);
      if (err) {
        return err;
      }
    }
    lock_store();
    if (target_length) {
      memcpy(group->state->record_data, target, target_length);
    }
    if (has_saved_record) {
      group->state->record_length = target_length;
      group->state->record_value_state = DYNAMIC_SETTING_VALUE_PERSISTED;
      group->state->persisted_count = 1;
    } else {
      group->state->record_length = group->default_record_length;
      group->state->record_value_state = DYNAMIC_SETTING_VALUE_EMPTY;
      group->state->persisted_count = 0;
    }
    group->state->dirty = false;
    group->state->shared_preview = false;
    k_mutex_unlock(&store_mutex);
    return 0;
  }
  lock_store();
  group->state->persisted_count =
      has_saved_record ? dynamic_settings_storage_scratch[14] : 0;
  k_mutex_unlock(&store_mutex);
  for (size_t i = 0; i < group->setting_count; i++) {
    const struct dynamic_setting_descriptor *descriptor =
        &dynamic_setting_descriptors[group->first_setting + i];
    struct dynamic_setting_value target;
    lock_store();
    uint8_t state = value_state(group, i);
    struct dynamic_setting_value temp_value;
    if (state != DYNAMIC_SETTING_VALUE_TEMP &&
        !(force && (descriptor->flags & DYNAMIC_SETTING_FLAG_PERSISTENT))) {
      k_mutex_unlock(&store_mutex);
      continue;
    }
    bool has_previous = staged_value(group, descriptor, &temp_value) == 0;
    bool has_saved_value =
        has_saved_record &&
        record_value(dynamic_settings_storage_scratch, descriptor->id, &target);
    if (has_saved_value) {
      target.type = descriptor->type;
    } else {
      int err = default_value(descriptor, &target);
      if (err) {
        k_mutex_unlock(&store_mutex);
        return err;
      }
    }
    k_mutex_unlock(&store_mutex);
    if ((!has_previous || !value_equal(&temp_value, &target) || force) &&
        callback_applies_at_runtime(descriptor)) {
      int err = apply_setting(descriptor, &target,
                              force ? DYNAMIC_SETTING_CHANGE_PEER_APPLY
                                    : DYNAMIC_SETTING_CHANGE_DISCARD,
                              group->context);
      if (err) {
        return err;
      }
    }
    lock_store();
    if (has_saved_value) {
      write_cached_value(group, i, &target, DYNAMIC_SETTING_VALUE_PERSISTED);
    } else {
      clear_cached_value(group, i);
    }
    k_mutex_unlock(&store_mutex);
  }

  lock_store();
  group->state->dirty = false;
  group->state->shared_preview = false;
  k_mutex_unlock(&store_mutex);
  return 0;
}

int dynamic_settings_discard(uint32_t group_id) {
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  if (!group) {
    return -ENOENT;
  }
  if (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY) {
    return -EHOSTUNREACH;
  }
  if (!group->state) {
    return -ENOTSUP;
  }
  k_mutex_lock(&operation_mutex, K_FOREVER);
  int err = reload_group_locked(group, false);
  if (!err) {
    lock_store();
    group->state->shared_preview = false;
    k_mutex_unlock(&store_mutex);
  }
  k_mutex_unlock(&operation_mutex);
  return err;
}

bool dynamic_settings_is_dirty(uint32_t group_id) {
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  lock_store();
  bool dirty = group && group->state && group->state->dirty;
  k_mutex_unlock(&store_mutex);
  return dirty;
}

bool dynamic_settings_group_has_persisted_value(uint32_t group_id) {
  const struct dynamic_setting_group_descriptor *group =
      dynamic_settings_find_group(group_id);
  if (!group || !group->state) {
    return false;
  }
  lock_store();
  bool has_persisted = group->state->persisted_count != 0;
  k_mutex_unlock(&store_mutex);
  return has_persisted;
}

bool dynamic_settings_scope_is_dirty(enum dynamic_setting_scope scope) {
  if (scope < DYNAMIC_SETTING_SCOPE_LOCAL ||
      scope > DYNAMIC_SETTING_SCOPE_OWNER) {
    return false;
  }
  lock_store();
  bool dirty = false;
  for (size_t i = 0; i < dynamic_setting_group_count; i++) {
    const struct dynamic_setting_group_descriptor *group =
        &dynamic_setting_groups[i];
    if (group->scope == scope &&
        !(group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY) &&
        group_is_dirty(group->id)) {
      dirty = true;
      break;
    }
  }
  k_mutex_unlock(&store_mutex);
  return dirty;
}

bool dynamic_settings_scope_has_persisted_value(
    enum dynamic_setting_scope scope) {
  if (scope < DYNAMIC_SETTING_SCOPE_LOCAL ||
      scope > DYNAMIC_SETTING_SCOPE_OWNER) {
    return false;
  }
  lock_store();
  bool has_persisted = false;
  for (size_t i = 0; i < dynamic_setting_group_count; i++) {
    const struct dynamic_setting_group_descriptor *group =
        &dynamic_setting_groups[i];
    if (group->scope != scope || !group->state ||
        (group->flags & DYNAMIC_SETTING_GROUP_FLAG_PROXY)) {
      continue;
    }
    if (group->state->persisted_count != 0) {
      has_persisted = true;
      break;
    }
  }
  k_mutex_unlock(&store_mutex);
  return has_persisted;
}

uint32_t dynamic_settings_generation(enum dynamic_setting_scope scope) {
  if (scope < DYNAMIC_SETTING_SCOPE_LOCAL ||
      scope > DYNAMIC_SETTING_SCOPE_OWNER) {
    return 0;
  }
  lock_store();
  uint32_t generation = scope_generations[scope];
  k_mutex_unlock(&store_mutex);
  return generation;
}
