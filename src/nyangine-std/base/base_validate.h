/**
 * @file base_validate.h
 *
 * Declarative validation of a value against the rules its own type carries. The reflection table already
 * records every `@name(args)` annotation a field wears (see base_reflection.h); this reads the handful of
 * them that describe what a well formed value is, so a DTO says its constraints beside its fields instead
 * of every handler that receives one repeating the same `if` twice.
 *
 * ```c
 * // @reflect
 * typedef struct SignupRequest {
 *     char username[33];   // @required @len(3, 32) @pattern([a-z0-9_]*)
 *     char email[255];     // @required @email
 *     u32  age;            // @min(13) @max(120)
 * } SignupRequest;
 *
 * SignupRequest request = ...;
 * NYA_Error valid = nya_validate(nya_reflect_of(SignupRequest), &request);
 * if (!valid.ok) return valid;   // the message names the field and the rule it broke
 * ```
 *
 * ── the rules ──
 *
 *   @required        a non-zero number, a non-empty string, a non-null pointer, a struct not all zero.
 *   @min(n) @max(n)  a number is at least / at most `n`; applies only to a numeric or enum field.
 *   @len(min, max)   a string's length is within [min, max] inclusive; applies only to a string field.
 *   @email           a string is a syntactic email address, by base_newtype.h's own rule.
 *   @pattern(glob)   a string matches a tiny glob: `*` any run, `?` any one byte, everything else literal.
 *
 * An attribute the validator has no rule for is ignored, not an error: the table holds annotations for
 * every consumer, and `@label` or an ORM's `@unique` is none of validation's business. A rule that names a
 * shape the field is not — `@min` on a string, `@email` on an integer — does not apply and is skipped
 * rather than failing, since the annotation and the field disagreeing is a source mistake, not bad data.
 *
 * ── first failure, no allocation ──
 *
 * The walk stops at the first field that breaks a rule and returns it; the returned NYA_Error's message is
 * the report, naming the field and the rule. There is no arena to thread through: the args string is parsed
 * in place and the check reads the value where it sits, so the valid path allocates nothing. A nested
 * struct or union field is validated to its leaves; pointers other than a string are not followed.
 * */
#pragma once

#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_reflection.h"

// FUNCTIONS

/**
 * Checks `value` against the validation attributes on `type`'s fields, returning the first broken rule.
 *
 * `type` must be a struct or union; anything else validates trivially, since only a field carries a rule.
 * The returned error is NYA_ERROR_INVALID_ARGUMENT, which the http error mapper turns into 400, with a
 * message naming the offending field and what it wanted. NYA_OK when every field's rules hold.
 * */
NYA_API NYA_Error nya_validate(const NYA_TypeReflection* type, const void* value) __attr_no_discard;
