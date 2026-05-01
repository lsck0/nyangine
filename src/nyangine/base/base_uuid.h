/**
 * @file base_uuid.h
 *
 * UUID v4 (random) and v7 (time-ordered) generation and parsing.
 *
 * Example:
 * ```c
 * NYA_RNG rng = nya_rng_create();
 *
 * // Generate a v4 (random) UUID
 * NYA_Uuid id_v4 = nya_uuid_v4_create(&rng);
 *
 * // Generate a v7 (time-ordered) UUID - recommended for database keys
 * NYA_Uuid id_v7 = nya_uuid_v7_create(&rng);
 *
 * // Parse and format
 * NYA_Uuid parsed;
 * b8 ok = nya_uuid_parse("550e8400-e29b-41d4-a716-446655440000", &parsed);
 *
 * char buffer[NYA_UUID_STRING_BUFFER_SIZE];
 * nya_uuid_format(id_v4, buffer);
 * ```
 * */
#pragma once

#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_random.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define NYA_UUID_STRING_LENGTH      36
#define NYA_UUID_STRING_BUFFER_SIZE 37

typedef u128 NYA_Uuid;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_EXTERN NYA_Uuid nya_uuid_v4_create(NYA_RNG* rng);
NYA_API NYA_EXTERN NYA_Uuid nya_uuid_v7_create(NYA_RNG* rng);

NYA_API NYA_EXTERN b8   nya_uuid_parse(NYA_ConstCString str, NYA_Uuid* out);
NYA_API NYA_EXTERN void nya_uuid_format(NYA_Uuid uuid, char buffer[NYA_UUID_STRING_BUFFER_SIZE]);
