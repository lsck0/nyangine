/* THIS FILE IS GENERATED. DO NYAT TOUCH. */

#include "nyangine/nyangine.h"

#include "genyarated/reflection_engine.h"

/*
 * The server-safe engine reflections: the builtins and the annotated types in modules a
 * headless (NYA_NO_SDL + NYA_SERVER) build compiles. The SDL-bound ones are in
 * reflection_engine.c. Every size and offset is an expression, so the compiler already
 * compiling these structs computes the layout. See src/build/pp/reflection.h.
 */

/* ── primitives ── */

const NYA_TypeReflection _NYA_REFLECT_b8 = { .name = "b8", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(b8), .alignment = alignof(b8), .primitive = NYA_TYPE_B8 };
const NYA_TypeReflection _NYA_REFLECT_b16 = { .name = "b16", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(b16), .alignment = alignof(b16), .primitive = NYA_TYPE_B16 };
const NYA_TypeReflection _NYA_REFLECT_b32 = { .name = "b32", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(b32), .alignment = alignof(b32), .primitive = NYA_TYPE_B32 };
const NYA_TypeReflection _NYA_REFLECT_b64 = { .name = "b64", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(b64), .alignment = alignof(b64), .primitive = NYA_TYPE_B64 };
const NYA_TypeReflection _NYA_REFLECT_u8 = { .name = "u8", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(u8), .alignment = alignof(u8), .primitive = NYA_TYPE_U8 };
const NYA_TypeReflection _NYA_REFLECT_u16 = { .name = "u16", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(u16), .alignment = alignof(u16), .primitive = NYA_TYPE_U16 };
const NYA_TypeReflection _NYA_REFLECT_u32 = { .name = "u32", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(u32), .alignment = alignof(u32), .primitive = NYA_TYPE_U32 };
const NYA_TypeReflection _NYA_REFLECT_u64 = { .name = "u64", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(u64), .alignment = alignof(u64), .primitive = NYA_TYPE_U64 };
const NYA_TypeReflection _NYA_REFLECT_s8 = { .name = "s8", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(s8), .alignment = alignof(s8), .primitive = NYA_TYPE_S8 };
const NYA_TypeReflection _NYA_REFLECT_s16 = { .name = "s16", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(s16), .alignment = alignof(s16), .primitive = NYA_TYPE_S16 };
const NYA_TypeReflection _NYA_REFLECT_s32 = { .name = "s32", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(s32), .alignment = alignof(s32), .primitive = NYA_TYPE_S32 };
const NYA_TypeReflection _NYA_REFLECT_s64 = { .name = "s64", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(s64), .alignment = alignof(s64), .primitive = NYA_TYPE_S64 };
const NYA_TypeReflection _NYA_REFLECT_f32 = { .name = "f32", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(f32), .alignment = alignof(f32), .primitive = NYA_TYPE_F32 };
const NYA_TypeReflection _NYA_REFLECT_f64 = { .name = "f64", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(f64), .alignment = alignof(f64), .primitive = NYA_TYPE_F64 };
const NYA_TypeReflection _NYA_REFLECT_char = { .name = "char", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(char), .alignment = alignof(char), .primitive = NYA_TYPE_CHAR };

const NYA_TypeReflection _NYA_REFLECT_string = { .name = "string", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(char*), .alignment = alignof(char*), .primitive = NYA_TYPE_STRING };

/* ── vectors ── */

const NYA_TypeReflection _NYA_REFLECT_f32x2 = { .name = "f32x2", .kind = NYA_REFLECT_VECTOR, .size = sizeof(f32x2), .alignment = alignof(f32x2), .element = &_NYA_REFLECT_f32, .element_count = 2 };
const NYA_TypeReflection _NYA_REFLECT_f32x3 = { .name = "f32x3", .kind = NYA_REFLECT_VECTOR, .size = sizeof(f32x3), .alignment = alignof(f32x3), .element = &_NYA_REFLECT_f32, .element_count = 3 };
const NYA_TypeReflection _NYA_REFLECT_f32x4 = { .name = "f32x4", .kind = NYA_REFLECT_VECTOR, .size = sizeof(f32x4), .alignment = alignof(f32x4), .element = &_NYA_REFLECT_f32, .element_count = 4 };

/* NYA_HttpScope, src/nyangine/http/http_auth.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_HttpScope_VARIANTS[] = {
    { .name = "NYA_HTTP_SCOPE_NONE", .value = (s64)(NYA_HTTP_SCOPE_NONE) },
    { .name = "NYA_HTTP_SCOPE_READ", .value = (s64)(NYA_HTTP_SCOPE_READ) },
    { .name = "NYA_HTTP_SCOPE_WRITE", .value = (s64)(NYA_HTTP_SCOPE_WRITE) },
    { .name = "NYA_HTTP_SCOPE_ADMIN", .value = (s64)(NYA_HTTP_SCOPE_ADMIN) },
    { .name = "NYA_HTTP_SCOPE_SECOND_FACTOR", .value = (s64)(NYA_HTTP_SCOPE_SECOND_FACTOR) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpScope = {
    .name = "NYA_HttpScope",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_HttpScope),
    .alignment = alignof(NYA_HttpScope),
    .primitive = (sizeof(NYA_HttpScope) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_HttpScope) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_HttpScope) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_HttpScope_VARIANTS,
    .variant_count = 5,
    .is_bitflags = true,
};

/* NYA_HttpIdentity, src/nyangine/http/http_auth.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpIdentity_subject_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpIdentity*)nullptr)->subject),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_MAX_SUBJECT),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpIdentity_FIELDS[] = {
    { .name = "subject", .type = &_NYA_REFLECT_NYA_HttpIdentity_subject_ARRAY, .offset = nya_offsetof(NYA_HttpIdentity, subject), .hint = NYA_HINT_NONE },
    { .name = "scope", .type = &_NYA_REFLECT_NYA_HttpScope, .offset = nya_offsetof(NYA_HttpIdentity, scope), .hint = NYA_HINT_BITFLAGS },
    { .name = "issued_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpIdentity, issued_at_s), .hint = NYA_HINT_NONE },
    { .name = "expires_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpIdentity, expires_at_s), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpIdentity = {
    .name = "NYA_HttpIdentity",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpIdentity),
    .alignment = alignof(NYA_HttpIdentity),
    .fields = _NYA_REFLECT_NYA_HttpIdentity_FIELDS,
    .field_count = 4,
};

/* NYA_HttpHealthDto, src/nyangine/http/http_health.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpHealthDto_status_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpHealthDto*)nullptr)->status),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (16),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpHealthDto_FIELDS[] = {
    { .name = "status", .type = &_NYA_REFLECT_NYA_HttpHealthDto_status_ARRAY, .offset = nya_offsetof(NYA_HttpHealthDto, status), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpHealthDto = {
    .name = "NYA_HttpHealthDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpHealthDto),
    .alignment = alignof(NYA_HttpHealthDto),
    .fields = _NYA_REFLECT_NYA_HttpHealthDto_FIELDS,
    .field_count = 1,
};

/* NYA_HttpReadyCheckDto, src/nyangine/http/http_health.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpReadyCheckDto_name_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpReadyCheckDto*)nullptr)->name),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_HEALTH_MAX_NAME),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpReadyCheckDto_FIELDS[] = {
    { .name = "name", .type = &_NYA_REFLECT_NYA_HttpReadyCheckDto_name_ARRAY, .offset = nya_offsetof(NYA_HttpReadyCheckDto, name), .hint = NYA_HINT_NONE },
    { .name = "ready", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_HttpReadyCheckDto, ready), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpReadyCheckDto = {
    .name = "NYA_HttpReadyCheckDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpReadyCheckDto),
    .alignment = alignof(NYA_HttpReadyCheckDto),
    .fields = _NYA_REFLECT_NYA_HttpReadyCheckDto_FIELDS,
    .field_count = 2,
};

/* NYA_HttpReadinessDto, src/nyangine/http/http_health.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpReadinessDto_checks_ARRAY = {
    .name = "NYA_HttpReadyCheckDto[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpReadinessDto*)nullptr)->checks),
    .alignment = alignof(NYA_HttpReadyCheckDto),
    .element = &_NYA_REFLECT_NYA_HttpReadyCheckDto, .element_count = (NYA_HTTP_HEALTH_MAX_CHECKS),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpReadinessDto_FIELDS[] = {
    { .name = "ready", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_HttpReadinessDto, ready), .hint = NYA_HINT_NONE },
    { .name = "count", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpReadinessDto, count), .hint = NYA_HINT_NONE },
    { .name = "failed", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpReadinessDto, failed), .hint = NYA_HINT_NONE },
    { .name = "checks", .type = &_NYA_REFLECT_NYA_HttpReadinessDto_checks_ARRAY, .offset = nya_offsetof(NYA_HttpReadinessDto, checks), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpReadinessDto = {
    .name = "NYA_HttpReadinessDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpReadinessDto),
    .alignment = alignof(NYA_HttpReadinessDto),
    .fields = _NYA_REFLECT_NYA_HttpReadinessDto_FIELDS,
    .field_count = 4,
};

/* NYA_HttpLogLevel, src/nyangine/http/http_log.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_HttpLogLevel_VARIANTS[] = {
    { .name = "NYA_HTTP_LOG_SUMMARY", .value = (s64)(NYA_HTTP_LOG_SUMMARY) },
    { .name = "NYA_HTTP_LOG_HEADERS", .value = (s64)(NYA_HTTP_LOG_HEADERS) },
    { .name = "NYA_HTTP_LOG_BODIES", .value = (s64)(NYA_HTTP_LOG_BODIES) },
    { .name = "NYA_HTTP_LOG_LEVEL_COUNT", .value = (s64)(NYA_HTTP_LOG_LEVEL_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpLogLevel = {
    .name = "NYA_HttpLogLevel",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_HttpLogLevel),
    .alignment = alignof(NYA_HttpLogLevel),
    .primitive = (sizeof(NYA_HttpLogLevel) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_HttpLogLevel) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_HttpLogLevel) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_HttpLogLevel_VARIANTS,
    .variant_count = 4,
    .is_bitflags = false,
};

/* NYA_HttpLogAddress, src/nyangine/http/http_log.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_HttpLogAddress_VARIANTS[] = {
    { .name = "NYA_HTTP_LOG_ADDRESS_NETWORK", .value = (s64)(NYA_HTTP_LOG_ADDRESS_NETWORK) },
    { .name = "NYA_HTTP_LOG_ADDRESS_FULL", .value = (s64)(NYA_HTTP_LOG_ADDRESS_FULL) },
    { .name = "NYA_HTTP_LOG_ADDRESS_NONE", .value = (s64)(NYA_HTTP_LOG_ADDRESS_NONE) },
    { .name = "NYA_HTTP_LOG_ADDRESS_COUNT", .value = (s64)(NYA_HTTP_LOG_ADDRESS_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpLogAddress = {
    .name = "NYA_HttpLogAddress",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_HttpLogAddress),
    .alignment = alignof(NYA_HttpLogAddress),
    .primitive = (sizeof(NYA_HttpLogAddress) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_HttpLogAddress) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_HttpLogAddress) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_HttpLogAddress_VARIANTS,
    .variant_count = 4,
    .is_bitflags = false,
};

/* NYA_HttpLogConfig, src/nyangine/http/http_log.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpLogConfig_deny_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpLogConfig*)nullptr)->deny),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_LOG_MAX_DENY_BYTES),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpLogConfig_FIELDS[] = {
    { .name = "level", .type = &_NYA_REFLECT_NYA_HttpLogLevel, .offset = nya_offsetof(NYA_HttpLogConfig, level), .hint = NYA_HINT_NONE },
    { .name = "address", .type = &_NYA_REFLECT_NYA_HttpLogAddress, .offset = nya_offsetof(NYA_HttpLogConfig, address), .hint = NYA_HINT_NONE },
    { .name = "deny", .type = &_NYA_REFLECT_NYA_HttpLogConfig_deny_ARRAY, .offset = nya_offsetof(NYA_HttpLogConfig, deny), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpLogConfig = {
    .name = "NYA_HttpLogConfig",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpLogConfig),
    .alignment = alignof(NYA_HttpLogConfig),
    .fields = _NYA_REFLECT_NYA_HttpLogConfig_FIELDS,
    .field_count = 3,
    .on_apply = _nya_http_log_config_apply,
};

/* NYA_HttpProblem, src/nyangine/http/http_router.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpProblem_error_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpProblem*)nullptr)->error),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (48),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpProblem_detail_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpProblem*)nullptr)->detail),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (192),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpProblem_FIELDS[] = {
    { .name = "status", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpProblem, status), .hint = NYA_HINT_NONE },
    { .name = "error", .type = &_NYA_REFLECT_NYA_HttpProblem_error_ARRAY, .offset = nya_offsetof(NYA_HttpProblem, error), .hint = NYA_HINT_NONE },
    { .name = "detail", .type = &_NYA_REFLECT_NYA_HttpProblem_detail_ARRAY, .offset = nya_offsetof(NYA_HttpProblem, detail), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpProblem = {
    .name = "NYA_HttpProblem",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpProblem),
    .alignment = alignof(NYA_HttpProblem),
    .fields = _NYA_REFLECT_NYA_HttpProblem_FIELDS,
    .field_count = 3,
};

/* NYA_HttpTotpSubmission, src/nyangine/http/http_totp.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpSubmission_code_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpTotpSubmission*)nullptr)->code),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_TOTP_RECOVERY_TEXT_BYTES),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpTotpSubmission_FIELDS[] = {
    { .name = "code", .type = &_NYA_REFLECT_NYA_HttpTotpSubmission_code_ARRAY, .offset = nya_offsetof(NYA_HttpTotpSubmission, code), .hint = NYA_HINT_NONE, .is_redacted = true },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpSubmission = {
    .name = "NYA_HttpTotpSubmission",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpTotpSubmission),
    .alignment = alignof(NYA_HttpTotpSubmission),
    .fields = _NYA_REFLECT_NYA_HttpTotpSubmission_FIELDS,
    .field_count = 1,
};

/* NYA_HttpTotpRecoveryDto, src/nyangine/http/http_totp.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpRecoveryDto_code_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpTotpRecoveryDto*)nullptr)->code),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_TOTP_RECOVERY_TEXT_BYTES),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpTotpRecoveryDto_FIELDS[] = {
    { .name = "code", .type = &_NYA_REFLECT_NYA_HttpTotpRecoveryDto_code_ARRAY, .offset = nya_offsetof(NYA_HttpTotpRecoveryDto, code), .hint = NYA_HINT_NONE, .is_redacted = true },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpRecoveryDto = {
    .name = "NYA_HttpTotpRecoveryDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpTotpRecoveryDto),
    .alignment = alignof(NYA_HttpTotpRecoveryDto),
    .fields = _NYA_REFLECT_NYA_HttpTotpRecoveryDto_FIELDS,
    .field_count = 1,
};

/* NYA_HttpTotpEnrolmentDto, src/nyangine/http/http_totp.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpEnrolmentDto_uri_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpTotpEnrolmentDto*)nullptr)->uri),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_TOTP_URI_BYTES),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpEnrolmentDto_secret_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpTotpEnrolmentDto*)nullptr)->secret),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_TOTP_SECRET_TEXT_BYTES),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpEnrolmentDto_recovery_ARRAY = {
    .name = "NYA_HttpTotpRecoveryDto[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpTotpEnrolmentDto*)nullptr)->recovery),
    .alignment = alignof(NYA_HttpTotpRecoveryDto),
    .element = &_NYA_REFLECT_NYA_HttpTotpRecoveryDto, .element_count = (NYA_HTTP_TOTP_RECOVERY_CODES),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpTotpEnrolmentDto_FIELDS[] = {
    { .name = "uri", .type = &_NYA_REFLECT_NYA_HttpTotpEnrolmentDto_uri_ARRAY, .offset = nya_offsetof(NYA_HttpTotpEnrolmentDto, uri), .hint = NYA_HINT_NONE, .is_redacted = true },
    { .name = "secret", .type = &_NYA_REFLECT_NYA_HttpTotpEnrolmentDto_secret_ARRAY, .offset = nya_offsetof(NYA_HttpTotpEnrolmentDto, secret), .hint = NYA_HINT_NONE, .is_redacted = true },
    { .name = "recovery", .type = &_NYA_REFLECT_NYA_HttpTotpEnrolmentDto_recovery_ARRAY, .offset = nya_offsetof(NYA_HttpTotpEnrolmentDto, recovery), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpEnrolmentDto = {
    .name = "NYA_HttpTotpEnrolmentDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpTotpEnrolmentDto),
    .alignment = alignof(NYA_HttpTotpEnrolmentDto),
    .fields = _NYA_REFLECT_NYA_HttpTotpEnrolmentDto_FIELDS,
    .field_count = 3,
};

/* NYA_Quaternion, src/nyangine/math/math_quaternion.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_Quaternion_FIELDS[] = {
    { .name = "x", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Quaternion, x), .hint = NYA_HINT_NONE },
    { .name = "y", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Quaternion, y), .hint = NYA_HINT_NONE },
    { .name = "z", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Quaternion, z), .hint = NYA_HINT_NONE },
    { .name = "w", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Quaternion, w), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_Quaternion = {
    .name = "NYA_Quaternion",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_Quaternion),
    .alignment = alignof(NYA_Quaternion),
    .fields = _NYA_REFLECT_NYA_Quaternion_FIELDS,
    .field_count = 4,
};

/* NYA_EaseType, src/nyangine/math/math_tween.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_EaseType_VARIANTS[] = {
    { .name = "NYA_EASE_LINEAR", .value = (s64)(NYA_EASE_LINEAR) },
    { .name = "NYA_EASE_QUAD_IN", .value = (s64)(NYA_EASE_QUAD_IN) },
    { .name = "NYA_EASE_QUAD_OUT", .value = (s64)(NYA_EASE_QUAD_OUT) },
    { .name = "NYA_EASE_QUAD_IN_OUT", .value = (s64)(NYA_EASE_QUAD_IN_OUT) },
    { .name = "NYA_EASE_CUBIC_IN", .value = (s64)(NYA_EASE_CUBIC_IN) },
    { .name = "NYA_EASE_CUBIC_OUT", .value = (s64)(NYA_EASE_CUBIC_OUT) },
    { .name = "NYA_EASE_CUBIC_IN_OUT", .value = (s64)(NYA_EASE_CUBIC_IN_OUT) },
    { .name = "NYA_EASE_QUART_IN", .value = (s64)(NYA_EASE_QUART_IN) },
    { .name = "NYA_EASE_QUART_OUT", .value = (s64)(NYA_EASE_QUART_OUT) },
    { .name = "NYA_EASE_QUART_IN_OUT", .value = (s64)(NYA_EASE_QUART_IN_OUT) },
    { .name = "NYA_EASE_QUINT_IN", .value = (s64)(NYA_EASE_QUINT_IN) },
    { .name = "NYA_EASE_QUINT_OUT", .value = (s64)(NYA_EASE_QUINT_OUT) },
    { .name = "NYA_EASE_QUINT_IN_OUT", .value = (s64)(NYA_EASE_QUINT_IN_OUT) },
    { .name = "NYA_EASE_SINE_IN", .value = (s64)(NYA_EASE_SINE_IN) },
    { .name = "NYA_EASE_SINE_OUT", .value = (s64)(NYA_EASE_SINE_OUT) },
    { .name = "NYA_EASE_SINE_IN_OUT", .value = (s64)(NYA_EASE_SINE_IN_OUT) },
    { .name = "NYA_EASE_EXPO_IN", .value = (s64)(NYA_EASE_EXPO_IN) },
    { .name = "NYA_EASE_EXPO_OUT", .value = (s64)(NYA_EASE_EXPO_OUT) },
    { .name = "NYA_EASE_EXPO_IN_OUT", .value = (s64)(NYA_EASE_EXPO_IN_OUT) },
    { .name = "NYA_EASE_CIRC_IN", .value = (s64)(NYA_EASE_CIRC_IN) },
    { .name = "NYA_EASE_CIRC_OUT", .value = (s64)(NYA_EASE_CIRC_OUT) },
    { .name = "NYA_EASE_CIRC_IN_OUT", .value = (s64)(NYA_EASE_CIRC_IN_OUT) },
    { .name = "NYA_EASE_BACK_IN", .value = (s64)(NYA_EASE_BACK_IN) },
    { .name = "NYA_EASE_BACK_OUT", .value = (s64)(NYA_EASE_BACK_OUT) },
    { .name = "NYA_EASE_BACK_IN_OUT", .value = (s64)(NYA_EASE_BACK_IN_OUT) },
    { .name = "NYA_EASE_ELASTIC_IN", .value = (s64)(NYA_EASE_ELASTIC_IN) },
    { .name = "NYA_EASE_ELASTIC_OUT", .value = (s64)(NYA_EASE_ELASTIC_OUT) },
    { .name = "NYA_EASE_ELASTIC_IN_OUT", .value = (s64)(NYA_EASE_ELASTIC_IN_OUT) },
    { .name = "NYA_EASE_BOUNCE_IN", .value = (s64)(NYA_EASE_BOUNCE_IN) },
    { .name = "NYA_EASE_BOUNCE_OUT", .value = (s64)(NYA_EASE_BOUNCE_OUT) },
    { .name = "NYA_EASE_BOUNCE_IN_OUT", .value = (s64)(NYA_EASE_BOUNCE_IN_OUT) },
    { .name = "NYA_EASE_SMOOTHSTEP", .value = (s64)(NYA_EASE_SMOOTHSTEP) },
    { .name = "NYA_EASE_SMOOTHERSTEP", .value = (s64)(NYA_EASE_SMOOTHERSTEP) },
    { .name = "NYA_EASE_COUNT", .value = (s64)(NYA_EASE_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_EaseType = {
    .name = "NYA_EaseType",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_EaseType),
    .alignment = alignof(NYA_EaseType),
    .primitive = (sizeof(NYA_EaseType) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_EaseType) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_EaseType) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_EaseType_VARIANTS,
    .variant_count = 34,
    .is_bitflags = false,
};

/* NYA_NetPeerId, src/nyangine/net/net_types.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_NetPeerId_FIELDS[] = {
    { .name = "index", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_NetPeerId, index), .hint = NYA_HINT_NONE },
    { .name = "generation", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_NetPeerId, generation), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_NetPeerId = {
    .name = "NYA_NetPeerId",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_NetPeerId),
    .alignment = alignof(NYA_NetPeerId),
    .fields = _NYA_REFLECT_NYA_NetPeerId_FIELDS,
    .field_count = 2,
};

/* NYA_SerdeSecretExample, src/nyangine/serde/serde_reflect.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_SerdeSecretExample_label_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_SerdeSecretExample*)nullptr)->label),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (32),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_SerdeSecretExample_password_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_SerdeSecretExample*)nullptr)->password),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (64),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_SerdeSecretExample_api_token_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_SerdeSecretExample*)nullptr)->api_token),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (64),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_SerdeSecretExample_FIELDS[] = {
    { .name = "label", .type = &_NYA_REFLECT_NYA_SerdeSecretExample_label_ARRAY, .offset = nya_offsetof(NYA_SerdeSecretExample, label), .hint = NYA_HINT_NONE },
    { .name = "password", .type = &_NYA_REFLECT_NYA_SerdeSecretExample_password_ARRAY, .offset = nya_offsetof(NYA_SerdeSecretExample, password), .hint = NYA_HINT_NONE, .is_secret = true },
    { .name = "pin", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_SerdeSecretExample, pin), .hint = NYA_HINT_NONE, .is_secret = true },
    { .name = "api_token", .type = &_NYA_REFLECT_NYA_SerdeSecretExample_api_token_ARRAY, .offset = nya_offsetof(NYA_SerdeSecretExample, api_token), .hint = NYA_HINT_NONE, .is_redacted = true },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_SerdeSecretExample = {
    .name = "NYA_SerdeSecretExample",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_SerdeSecretExample),
    .alignment = alignof(NYA_SerdeSecretExample),
    .fields = _NYA_REFLECT_NYA_SerdeSecretExample_FIELDS,
    .field_count = 4,
};

#ifdef NYA_MODULE_DB

/* NYA_AccountAudit, src/nyangine/accounts/accounts_audit.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountAudit_reason_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountAudit*)nullptr)->reason),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_AUDIT_REASON_MAX),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_AccountAudit_FIELDS[] = {
    { .name = "id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountAudit, id), .hint = NYA_HINT_NONE, .is_key = true },
    { .name = "at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountAudit, at_s), .hint = NYA_HINT_NONE },
    { .name = "actor_id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountAudit, actor_id), .hint = NYA_HINT_NONE },
    { .name = "subject_id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountAudit, subject_id), .hint = NYA_HINT_NONE },
    { .name = "action", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_AccountAudit, action), .hint = NYA_HINT_NONE },
    { .name = "reason", .type = &_NYA_REFLECT_NYA_AccountAudit_reason_ARRAY, .offset = nya_offsetof(NYA_AccountAudit, reason), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AccountAudit = {
    .name = "NYA_AccountAudit",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AccountAudit),
    .alignment = alignof(NYA_AccountAudit),
    .fields = _NYA_REFLECT_NYA_AccountAudit_FIELDS,
    .field_count = 6,
};

/* NYA_AccountIdentity, src/nyangine/accounts/accounts_identity.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountIdentity_provider_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountIdentity*)nullptr)->provider),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_MAX_PROVIDER),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountIdentity_subject_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountIdentity*)nullptr)->subject),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_MAX_SUBJECT),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountIdentity_display_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountIdentity*)nullptr)->display),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_MAX_DISPLAY),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_AccountIdentity_FIELDS[] = {
    { .name = "id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountIdentity, id), .hint = NYA_HINT_NONE, .is_key = true },
    { .name = "account_id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountIdentity, account_id), .hint = NYA_HINT_NONE },
    { .name = "provider", .type = &_NYA_REFLECT_NYA_AccountIdentity_provider_ARRAY, .offset = nya_offsetof(NYA_AccountIdentity, provider), .hint = NYA_HINT_NONE },
    { .name = "subject", .type = &_NYA_REFLECT_NYA_AccountIdentity_subject_ARRAY, .offset = nya_offsetof(NYA_AccountIdentity, subject), .hint = NYA_HINT_NONE },
    { .name = "display", .type = &_NYA_REFLECT_NYA_AccountIdentity_display_ARRAY, .offset = nya_offsetof(NYA_AccountIdentity, display), .hint = NYA_HINT_NONE },
    { .name = "linked_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountIdentity, linked_at_s), .hint = NYA_HINT_NONE },
    { .name = "used_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountIdentity, used_at_s), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AccountIdentity = {
    .name = "NYA_AccountIdentity",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AccountIdentity),
    .alignment = alignof(NYA_AccountIdentity),
    .fields = _NYA_REFLECT_NYA_AccountIdentity_FIELDS,
    .field_count = 7,
};

/* NYA_AccountInvite, src/nyangine/accounts/accounts_invite.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountInvite_code_hash_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountInvite*)nullptr)->code_hash),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (72),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_AccountInvite_FIELDS[] = {
    { .name = "id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountInvite, id), .hint = NYA_HINT_NONE, .is_key = true },
    { .name = "code_hash", .type = &_NYA_REFLECT_NYA_AccountInvite_code_hash_ARRAY, .offset = nya_offsetof(NYA_AccountInvite, code_hash), .hint = NYA_HINT_NONE, .is_redacted = true },
    { .name = "created_by", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountInvite, created_by), .hint = NYA_HINT_NONE },
    { .name = "used_by", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountInvite, used_by), .hint = NYA_HINT_NONE },
    { .name = "created_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountInvite, created_at_s), .hint = NYA_HINT_NONE },
    { .name = "expires_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountInvite, expires_at_s), .hint = NYA_HINT_NONE },
    { .name = "used_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountInvite, used_at_s), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AccountInvite = {
    .name = "NYA_AccountInvite",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AccountInvite),
    .alignment = alignof(NYA_AccountInvite),
    .fields = _NYA_REFLECT_NYA_AccountInvite_FIELDS,
    .field_count = 7,
};

/* NYA_AccountPasskey, src/nyangine/accounts/accounts_passkey.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountPasskey_credential_id_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountPasskey*)nullptr)->credential_id),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_PASSKEY_CRED_ID_TEXT),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountPasskey_public_key_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountPasskey*)nullptr)->public_key),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_PASSKEY_PUBLIC_KEY_TEXT),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountPasskey_name_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountPasskey*)nullptr)->name),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_PASSKEY_NAME_TEXT),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_AccountPasskey_FIELDS[] = {
    { .name = "id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountPasskey, id), .hint = NYA_HINT_NONE, .is_key = true },
    { .name = "user_id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountPasskey, user_id), .hint = NYA_HINT_NONE },
    { .name = "credential_id", .type = &_NYA_REFLECT_NYA_AccountPasskey_credential_id_ARRAY, .offset = nya_offsetof(NYA_AccountPasskey, credential_id), .hint = NYA_HINT_NONE },
    { .name = "public_key", .type = &_NYA_REFLECT_NYA_AccountPasskey_public_key_ARRAY, .offset = nya_offsetof(NYA_AccountPasskey, public_key), .hint = NYA_HINT_NONE },
    { .name = "algorithm", .type = &_NYA_REFLECT_s64, .offset = nya_offsetof(NYA_AccountPasskey, algorithm), .hint = NYA_HINT_NONE },
    { .name = "sign_count", .type = &_NYA_REFLECT_s64, .offset = nya_offsetof(NYA_AccountPasskey, sign_count), .hint = NYA_HINT_NONE },
    { .name = "name", .type = &_NYA_REFLECT_NYA_AccountPasskey_name_ARRAY, .offset = nya_offsetof(NYA_AccountPasskey, name), .hint = NYA_HINT_NONE },
    { .name = "created_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountPasskey, created_at_s), .hint = NYA_HINT_NONE },
    { .name = "used_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountPasskey, used_at_s), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AccountPasskey = {
    .name = "NYA_AccountPasskey",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AccountPasskey),
    .alignment = alignof(NYA_AccountPasskey),
    .fields = _NYA_REFLECT_NYA_AccountPasskey_FIELDS,
    .field_count = 9,
};

/* NYA_AccountPasskeyChallenge, src/nyangine/accounts/accounts_passkey.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountPasskeyChallenge_challenge_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountPasskeyChallenge*)nullptr)->challenge),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_PASSKEY_CHALLENGE_TEXT),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_AccountPasskeyChallenge_FIELDS[] = {
    { .name = "id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountPasskeyChallenge, id), .hint = NYA_HINT_NONE, .is_key = true },
    { .name = "user_id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountPasskeyChallenge, user_id), .hint = NYA_HINT_NONE },
    { .name = "purpose", .type = &_NYA_REFLECT_s64, .offset = nya_offsetof(NYA_AccountPasskeyChallenge, purpose), .hint = NYA_HINT_NONE },
    { .name = "challenge", .type = &_NYA_REFLECT_NYA_AccountPasskeyChallenge_challenge_ARRAY, .offset = nya_offsetof(NYA_AccountPasskeyChallenge, challenge), .hint = NYA_HINT_NONE },
    { .name = "created_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountPasskeyChallenge, created_at_s), .hint = NYA_HINT_NONE },
    { .name = "expires_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountPasskeyChallenge, expires_at_s), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AccountPasskeyChallenge = {
    .name = "NYA_AccountPasskeyChallenge",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AccountPasskeyChallenge),
    .alignment = alignof(NYA_AccountPasskeyChallenge),
    .fields = _NYA_REFLECT_NYA_AccountPasskeyChallenge_FIELDS,
    .field_count = 6,
};

/* NYA_AccountRecoveryCode, src/nyangine/accounts/accounts_recovery.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountRecoveryCode_code_hash_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountRecoveryCode*)nullptr)->code_hash),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (72),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_AccountRecoveryCode_FIELDS[] = {
    { .name = "id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountRecoveryCode, id), .hint = NYA_HINT_NONE, .is_key = true },
    { .name = "account_id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountRecoveryCode, account_id), .hint = NYA_HINT_NONE },
    { .name = "code_hash", .type = &_NYA_REFLECT_NYA_AccountRecoveryCode_code_hash_ARRAY, .offset = nya_offsetof(NYA_AccountRecoveryCode, code_hash), .hint = NYA_HINT_NONE, .is_redacted = true },
    { .name = "created_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountRecoveryCode, created_at_s), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AccountRecoveryCode = {
    .name = "NYA_AccountRecoveryCode",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AccountRecoveryCode),
    .alignment = alignof(NYA_AccountRecoveryCode),
    .fields = _NYA_REFLECT_NYA_AccountRecoveryCode_FIELDS,
    .field_count = 4,
};

/* NYA_AccountSession, src/nyangine/accounts/accounts_session.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountSession_token_hash_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountSession*)nullptr)->token_hash),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_TOKEN_HASH_BYTES),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountSession_previous_hash_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountSession*)nullptr)->previous_hash),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_TOKEN_HASH_BYTES),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountSession_address_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountSession*)nullptr)->address),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_MAX_ADDRESS),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountSession_agent_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountSession*)nullptr)->agent),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_MAX_AGENT),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_AccountSession_FIELDS[] = {
    { .name = "id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountSession, id), .hint = NYA_HINT_NONE, .is_key = true },
    { .name = "user_id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountSession, user_id), .hint = NYA_HINT_NONE },
    { .name = "token_hash", .type = &_NYA_REFLECT_NYA_AccountSession_token_hash_ARRAY, .offset = nya_offsetof(NYA_AccountSession, token_hash), .hint = NYA_HINT_NONE, .is_redacted = true },
    { .name = "previous_hash", .type = &_NYA_REFLECT_NYA_AccountSession_previous_hash_ARRAY, .offset = nya_offsetof(NYA_AccountSession, previous_hash), .hint = NYA_HINT_NONE, .is_redacted = true },
    { .name = "address", .type = &_NYA_REFLECT_NYA_AccountSession_address_ARRAY, .offset = nya_offsetof(NYA_AccountSession, address), .hint = NYA_HINT_NONE },
    { .name = "agent", .type = &_NYA_REFLECT_NYA_AccountSession_agent_ARRAY, .offset = nya_offsetof(NYA_AccountSession, agent), .hint = NYA_HINT_NONE },
    { .name = "created_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountSession, created_at_s), .hint = NYA_HINT_NONE },
    { .name = "used_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountSession, used_at_s), .hint = NYA_HINT_NONE },
    { .name = "expires_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountSession, expires_at_s), .hint = NYA_HINT_NONE },
    { .name = "revoked", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_AccountSession, revoked), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AccountSession = {
    .name = "NYA_AccountSession",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AccountSession),
    .alignment = alignof(NYA_AccountSession),
    .fields = _NYA_REFLECT_NYA_AccountSession_FIELDS,
    .field_count = 10,
};

/* NYA_AccountUser, src/nyangine/accounts/accounts_user.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountUser_username_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountUser*)nullptr)->username),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_MAX_USERNAME),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountUser_normalized_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountUser*)nullptr)->normalized),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_MAX_USERNAME),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountUser_display_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountUser*)nullptr)->display),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_MAX_DISPLAY),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_AccountUser_password_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_AccountUser*)nullptr)->password),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_ACCOUNTS_MAX_HASH),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_AccountUser_FIELDS[] = {
    { .name = "id", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountUser, id), .hint = NYA_HINT_NONE, .is_key = true },
    { .name = "username", .type = &_NYA_REFLECT_NYA_AccountUser_username_ARRAY, .offset = nya_offsetof(NYA_AccountUser, username), .hint = NYA_HINT_NONE },
    { .name = "normalized", .type = &_NYA_REFLECT_NYA_AccountUser_normalized_ARRAY, .offset = nya_offsetof(NYA_AccountUser, normalized), .hint = NYA_HINT_NONE },
    { .name = "display", .type = &_NYA_REFLECT_NYA_AccountUser_display_ARRAY, .offset = nya_offsetof(NYA_AccountUser, display), .hint = NYA_HINT_NONE },
    { .name = "password", .type = &_NYA_REFLECT_NYA_AccountUser_password_ARRAY, .offset = nya_offsetof(NYA_AccountUser, password), .hint = NYA_HINT_NONE, .is_redacted = true },
    { .name = "roles", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountUser, roles), .hint = NYA_HINT_NONE },
    { .name = "created_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountUser, created_at_s), .hint = NYA_HINT_NONE },
    { .name = "password_changed_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_AccountUser, password_changed_at_s), .hint = NYA_HINT_NONE },
    { .name = "disabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_AccountUser, disabled), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AccountUser = {
    .name = "NYA_AccountUser",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AccountUser),
    .alignment = alignof(NYA_AccountUser),
    .fields = _NYA_REFLECT_NYA_AccountUser_FIELDS,
    .field_count = 9,
};

#endif // NYA_MODULE_DB
