/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2017 Tom van Dijk, Johannes Kepler University Linz
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sylvan/internal.h> // for nodes*, nodes, sylvan_register_quit

#include <inttypes.h>
#include <string.h>

#include "mt_private.h"

/**
 * Handling of custom leaves "registry"
 */

typedef struct
{
    char *name;
    uint64_t cache_id;
    void *context;
    sylvan_mt_descriptor_hash_cb descriptor_hash;
    sylvan_mt_descriptor_equal_cb descriptor_equal;
    sylvan_mt_descriptor_clone_cb descriptor_clone;
    sylvan_mt_descriptor_destroy_cb descriptor_destroy;
    sylvan_mt_descriptor_to_string_cb descriptor_to_string;
    sylvan_mt_descriptor_string_free_cb descriptor_string_free;
    sylvan_mt_hash_cb hash_cb;
    sylvan_mt_equals_cb equals_cb;
    sylvan_mt_create_cb create_cb;
    sylvan_mt_destroy_cb destroy_cb;
    sylvan_mt_to_str_cb to_str_cb;
    sylvan_mt_write_binary_cb write_binary_cb;
    sylvan_mt_read_binary_cb read_binary_cb;
} customleaf_t;

static customleaf_t *cl_registry;
static size_t cl_registry_count;
static size_t cl_registry_size;

/**
 * Implementation of hooks for nodes
 */

/**
 * Internal helper function
 */
static inline customleaf_t*
sylvan_mt_from_node(uint64_t a, uint64_t b)
{
    uint32_t type = (uint32_t)a;
    assert(type < cl_registry_count);
    return cl_registry + type;
    (void)b;
}

static int
_sylvan_create_cb(uint64_t *a, uint64_t *b)
{
    customleaf_t *c = sylvan_mt_from_node(*a, *b);
    if (c->descriptor_clone != NULL) {
        uint64_t result;
        const int status = c->descriptor_clone(c->context, *b, &result);
        if (status != SYLVAN_OK) {
            return status < 0 ? status : SYLVAN_ERR_CALLBACK;
        }
        *b = result;
    } else if (c->create_cb != NULL) {
        c->create_cb(b);
    }
    return SYLVAN_OK;
}

static void
_sylvan_destroy_cb(uint64_t a, uint64_t b)
{
    // for leaf
    customleaf_t *c = sylvan_mt_from_node(a, b);
    if (c->descriptor_destroy != NULL) {
        c->descriptor_destroy(c->context, b);
    } else if (c->destroy_cb != NULL) {
        c->destroy_cb(b);
    }
}

static uint64_t
_sylvan_hash_cb(uint64_t a, uint64_t b, uint64_t seed)
{
    customleaf_t *c = sylvan_mt_from_node(a, b);
    if (c->descriptor_hash != NULL) {
        return c->descriptor_hash(c->context, b, seed ^ a);
    } else if (c->hash_cb != NULL) return c->hash_cb(b, seed ^ a);
    else return sylvan_tabhash16(a, b, seed);
}

static int
_sylvan_equals_cb(uint64_t a, uint64_t b, uint64_t aa, uint64_t bb)
{
    if (a != aa) return 0;
    customleaf_t *c = sylvan_mt_from_node(a, b);
    if (c->descriptor_equal != NULL) {
        return c->descriptor_equal(c->context, b, bb);
    } else if (c->equals_cb != NULL) return c->equals_cb(b, bb);
    else return b == bb ? 1 : 0;
}

static int
sylvan_mt_reserve_type(void)
{
    if (cl_registry_count > UINT32_MAX) return SYLVAN_ERR_OVERFLOW;
    if (cl_registry_count == cl_registry_size) {
        const size_t new_size = cl_registry_size + 8;
        customleaf_t *grown = (customleaf_t*)realloc(
            cl_registry, sizeof(customleaf_t) * new_size);
        if (grown == NULL) return SYLVAN_ERR_OOM;
        cl_registry = grown;
        memset(cl_registry + cl_registry_size, 0,
               sizeof(customleaf_t) * (new_size - cl_registry_size));
        cl_registry_size = new_size;
    }
    return SYLVAN_OK;
}

int
sylvan_mt_register_type(uint32_t *destination,
                        const sylvan_mt_type_descriptor *descriptor)
{
    if (destination == NULL || descriptor == NULL ||
        descriptor->name == NULL || descriptor->name[0] == '\0' ||
        descriptor->cache_id == 0 ||
        ((descriptor->hash == NULL) != (descriptor->equal == NULL)) ||
        ((descriptor->clone == NULL) != (descriptor->destroy == NULL)) ||
        ((descriptor->to_string == NULL) !=
         (descriptor->string_free == NULL)) ||
        (descriptor->clone != NULL && descriptor->hash == NULL)) {
        return SYLVAN_ERR_INVALID;
    }

    for (size_t i = 3; i < cl_registry_count; i++) {
        customleaf_t *registered = cl_registry + i;
        if ((registered->name != NULL &&
             strcmp(registered->name, descriptor->name) == 0) ||
            registered->cache_id == descriptor->cache_id) {
            return SYLVAN_ERR_INVALID;
        }
    }

    const int reserve_status = sylvan_mt_reserve_type();
    if (reserve_status != SYLVAN_OK) return reserve_status;

    const size_t name_size = strlen(descriptor->name) + 1;
    char *name = (char*)malloc(name_size);
    if (name == NULL) return SYLVAN_ERR_OOM;
    memcpy(name, descriptor->name, name_size);

    customleaf_t *entry = cl_registry + cl_registry_count;
    entry->name = name;
    entry->cache_id = descriptor->cache_id;
    entry->context = descriptor->context;
    entry->descriptor_hash = descriptor->hash;
    entry->descriptor_equal = descriptor->equal;
    entry->descriptor_clone = descriptor->clone;
    entry->descriptor_destroy = descriptor->destroy;
    entry->descriptor_to_string = descriptor->to_string;
    entry->descriptor_string_free = descriptor->string_free;
    *destination = (uint32_t)cl_registry_count++;
    return SYLVAN_OK;
}

const char *
sylvan_mt_type_name(uint32_t type)
{
    return type < cl_registry_count ? cl_registry[type].name : NULL;
}

uint64_t
sylvan_mt_type_cache_id(uint32_t type)
{
    return type < cl_registry_count ? cl_registry[type].cache_id : 0;
}

int
sylvan_mt_find_type(const char *name, uint32_t *destination)
{
    if (name == NULL || destination == NULL) return SYLVAN_ERR_INVALID;
    for (size_t i = 3; i < cl_registry_count; i++) {
        if (cl_registry[i].name != NULL &&
            strcmp(cl_registry[i].name, name) == 0) {
            *destination = (uint32_t)i;
            return SYLVAN_OK;
        }
    }
    return SYLVAN_ERR_INVALID;
}

uint32_t
sylvan_mt_create_type(void)
{
    if (sylvan_mt_reserve_type() != SYLVAN_OK) {
        fprintf(stderr, "sylvan: Unable to create custom terminal type\n");
        exit(1);
    }
    return (uint32_t)cl_registry_count++;
}

static customleaf_t *
sylvan_mt_legacy_entry(uint32_t type)
{
    assert(type < cl_registry_count);
    customleaf_t *entry = cl_registry + type;
    return entry->name == NULL ? entry : NULL;
}

void sylvan_mt_set_hash(uint32_t type, sylvan_mt_hash_cb hash_cb)
{
    customleaf_t *c = sylvan_mt_legacy_entry(type);
    if (c != NULL) c->hash_cb = hash_cb;
}

void sylvan_mt_set_equals(uint32_t type, sylvan_mt_equals_cb equals_cb)
{
    customleaf_t *c = sylvan_mt_legacy_entry(type);
    if (c != NULL) c->equals_cb = equals_cb;
}

void sylvan_mt_set_create(uint32_t type, sylvan_mt_create_cb create_cb)
{
    customleaf_t *c = sylvan_mt_legacy_entry(type);
    if (c != NULL) c->create_cb = create_cb;
}

void sylvan_mt_set_destroy(uint32_t type, sylvan_mt_destroy_cb destroy_cb)
{
    customleaf_t *c = sylvan_mt_legacy_entry(type);
    if (c != NULL) c->destroy_cb = destroy_cb;
}

void sylvan_mt_set_to_str(uint32_t type, sylvan_mt_to_str_cb to_str_cb)
{
    customleaf_t *c = sylvan_mt_legacy_entry(type);
    if (c != NULL) c->to_str_cb = to_str_cb;
}

void sylvan_mt_set_write_binary(uint32_t type, sylvan_mt_write_binary_cb write_binary_cb)
{
    customleaf_t *c = sylvan_mt_legacy_entry(type);
    if (c != NULL) c->write_binary_cb = write_binary_cb;
}

void sylvan_mt_set_read_binary(uint32_t type, sylvan_mt_read_binary_cb read_binary_cb)
{
    customleaf_t *c = sylvan_mt_legacy_entry(type);
    if (c != NULL) c->read_binary_cb = read_binary_cb;
}

int
sylvan_mt_bind_legacy_binary(
    uint32_t type, sylvan_mt_write_binary_cb write_binary,
    sylvan_mt_read_binary_cb read_binary)
{
    if (type < 3 || type >= cl_registry_count ||
        write_binary == NULL || read_binary == NULL) {
        return SYLVAN_ERR_INVALID;
    }

    customleaf_t *entry = cl_registry + type;
    if (entry->write_binary_cb != NULL || entry->read_binary_cb != NULL) {
        return SYLVAN_ERR_INVALID;
    }
    entry->write_binary_cb = write_binary;
    entry->read_binary_cb = read_binary;
    return SYLVAN_OK;
}

void
sylvan_mt_release_legacy_binary_value(uint32_t type, uint64_t value)
{
    assert(type < cl_registry_count);
    customleaf_t *entry = cl_registry + type;
    if (entry->read_binary_cb == NULL) return;

    if (entry->descriptor_destroy != NULL) {
        entry->descriptor_destroy(entry->context, value);
    } else if (entry->destroy_cb != NULL) {
        entry->destroy_cb(value);
    }
}

/**
 * Initialize and quit functions
 */

static int mt_initialized = 0;

static void
sylvan_mt_quit(void)
{
    if (mt_initialized == 0) return;
    mt_initialized = 0;

    /*
     * Garbage collection destroys unreachable custom values, but live custom
     * leaves also remain owned by Sylvan until shutdown. Clear all marks and
     * release those values before discarding their callback registry.
     */
    nodes_clear(nodes);
    nodes_cleanup_custom(nodes);

    for (size_t i = 0; i < cl_registry_count; i++) {
        free(cl_registry[i].name);
    }
    free(cl_registry);
    cl_registry = NULL;
    cl_registry_count = 0;
    cl_registry_size = 0;
}

void
sylvan_init_mt(void)
{
    if (mt_initialized) return;
    mt_initialized = 1;

    // Register quit handler to free structures
    sylvan_register_quit(sylvan_mt_quit);

    // Tell nodes to use our custom hooks
    nodes_set_custom(nodes, _sylvan_hash_cb, _sylvan_equals_cb, _sylvan_create_cb, _sylvan_destroy_cb);

    // Initialize data structures
    cl_registry_size = 8;
    cl_registry = (customleaf_t *)calloc(cl_registry_size, sizeof(customleaf_t));
    cl_registry_count = 3; // 0, 1, 2 are taken
}

/**
 * Return 1 if the given <type> has a custom hash callback, or 0 otherwise.
 */
int
sylvan_mt_has_custom_hash(uint32_t type)
{
    assert(type < cl_registry_count);
    customleaf_t *c = cl_registry + type;
    return c->descriptor_hash != NULL || c->hash_cb != NULL ? 1 : 0;
}

/**
 * Convert a leaf (possibly complemented) to a string representation.
 * If it does not fit in <buf> of size <buflen>, returns a freshly allocated char* array.
 */
char*
sylvan_mt_to_str(int complement, uint32_t type, uint64_t value, char* buf, size_t buflen)
{
    assert(type < cl_registry_count);
    customleaf_t *c = cl_registry + type;
    if (type == 0) {
        size_t required = (size_t)snprintf(NULL, 0, "%" PRId64, (int64_t)value);
        char *ptr = buf;
        if (buflen < required) {
            ptr = (char*)malloc(required);
            buflen = required;
        }
        if (ptr != NULL) snprintf(ptr, buflen, "%" PRId64, (int64_t)value);
        return ptr;
    } else if (type == 1) {
        size_t required = (size_t)snprintf(NULL, 0, "%f", *(double*)&value);
        char *ptr = buf;
        if (buflen < required) {
            ptr = (char*)malloc(required);
            buflen = required;
        }
        if (ptr != NULL) snprintf(ptr, buflen, "%f", *(double*)&value);
        return ptr;
    } else if (type == 2) {
        int32_t num = (int32_t)(value>>32);
        uint32_t denom = value&0xffffffff;
        size_t required = (size_t)snprintf(NULL, 0, "%" PRId32 "/%" PRIu32, num, denom);
        char *ptr = buf;
        if (buflen < required) {
            ptr = (char*)malloc(required);
            buflen = required;
        }
        if (ptr != NULL) snprintf(ptr, buflen, "%" PRId32 "/%" PRIu32, num, denom);
        return ptr;
    } else if (c->descriptor_to_string != NULL) {
        char *formatted = NULL;
        const int status = c->descriptor_to_string(
            c->context, complement, value, &formatted);
        if (status != SYLVAN_OK || formatted == NULL) return NULL;
        const size_t required = strlen(formatted) + 1;
        char *result = buf;
        if (buflen < required) result = (char*)malloc(required);
        if (result != NULL) memcpy(result, formatted, required);
        c->descriptor_string_free(c->context, formatted);
        return result;
    } else if (c->to_str_cb != NULL) {
        return c->to_str_cb(complement, value, buf, buflen);
    } else {
        return NULL;
    }
}

uint64_t
sylvan_mt_hash(uint32_t type, uint64_t value, uint64_t seed)
{
    assert(type < cl_registry_count);
    customleaf_t *c = cl_registry + type;
    if (c->descriptor_hash != NULL) {
        return c->descriptor_hash(c->context, value, seed);
    } else if (c->hash_cb != NULL) return c->hash_cb(value, seed);
    else return sylvan_tabhash16((uint64_t)type, value, seed);
}

int
sylvan_mt_write_binary(uint32_t type, uint64_t value, FILE *out)
{
    assert(type < cl_registry_count);
    customleaf_t *c = cl_registry + type;
    if (c->write_binary_cb != NULL) return c->write_binary_cb(out, value);
    else return 0;
}

int
sylvan_mt_read_binary(uint32_t type, uint64_t *value, FILE *in)
{
    assert(type < cl_registry_count);
    customleaf_t *c = cl_registry + type;
    if (c->read_binary_cb != NULL) return c->read_binary_cb(in, value);
    else return 0;
}
