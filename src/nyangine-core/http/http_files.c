#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_logging.h"
#include "nyangine-core/accounts/accounts_user.h"
#include "nyangine-core/db/db_orm.h"
#include "nyangine-core/http/http_accounts.h"
#include "nyangine-core/http/http_cookie.h"
#include "nyangine-core/http/http_files.h"
#include "nyangine-core/http/http_message.h"
#include "nyangine-core/http/http_multipart.h"

// CONSTANTS

/** The record table, one row per uploaded file, opened on the config's database beside the blob store. */
#define FILES_TABLE "files"

/** What a filename with no safe bytes left in it becomes, so a download always has a name to offer. */
#define FILES_DEFAULT_NAME "download"

/** The content type a part with none, or an unusable one, is stored and served as. */
#define FILES_DEFAULT_CONTENT_TYPE "application/octet-stream"

/** The longest decimal a download id may be, terminator included: a u64 is at most twenty digits. */
#define FILES_ID_TEXT 24

// REFLECTION

// The record as a set of ORM columns; built by hand like http_accounts' second-factor row, so no codegen step is needed to store a new type. The
// char[] fields need an array reflection each, because they differ in length.

NYA_INTERNAL const NYA_TypeReflection _NYA_HTTP_FILES_BLOB_ID_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = sizeof(((NYA_HttpFile*)nullptr)->blob_id),
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = sizeof(((NYA_HttpFile*)nullptr)->blob_id),
};

NYA_INTERNAL const NYA_TypeReflection _NYA_HTTP_FILES_NAME_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = NYA_HTTP_FILES_MAX_NAME,
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = NYA_HTTP_FILES_MAX_NAME,
};

NYA_INTERNAL const NYA_TypeReflection _NYA_HTTP_FILES_CONTENT_TYPE_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = NYA_HTTP_FILES_MAX_CONTENT_TYPE,
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = NYA_HTTP_FILES_MAX_CONTENT_TYPE,
};

NYA_INTERNAL const NYA_ReflectField _NYA_HTTP_FILES_FIELDS[] = {
    { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(NYA_HttpFile, id), .is_key = true },
    { .name = "owner", .type = nya_reflect_of(s64), .offset = nya_offsetof(NYA_HttpFile, owner) },
    { .name = "blob_id", .type = &_NYA_HTTP_FILES_BLOB_ID_ARRAY, .offset = nya_offsetof(NYA_HttpFile, blob_id) },
    { .name = "name", .type = &_NYA_HTTP_FILES_NAME_ARRAY, .offset = nya_offsetof(NYA_HttpFile, name) },
    { .name = "content_type", .type = &_NYA_HTTP_FILES_CONTENT_TYPE_ARRAY, .offset = nya_offsetof(NYA_HttpFile, content_type) },
    { .name = "size", .type = nya_reflect_of(s64), .offset = nya_offsetof(NYA_HttpFile, size) },
    { .name = "created", .type = nya_reflect_of(s64), .offset = nya_offsetof(NYA_HttpFile, created) },
};

NYA_INTERNAL const NYA_TypeReflection _NYA_HTTP_FILES_MODEL = {
    .name        = "NYA_HttpFile",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(NYA_HttpFile),
    .alignment   = alignof(NYA_HttpFile),
    .fields      = _NYA_HTTP_FILES_FIELDS,
    .field_count = nya_carray_length(_NYA_HTTP_FILES_FIELDS),
};

// STATE

// One state per process, like http_accounts: handlers are plain function pointers with no user data, so what they read lives at module scope. One
// mount per process.
NYA_INTERNAL struct {
    b8                  open;
    NYA_HttpFilesConfig config;
    NYA_BlobStore*      store;
    NYA_OrmTable*       files;
} _NYA_HTTP_FILES_STATE = { 0 };

// PRIVATE API DECLARATION

/** Parses `text[0, size)` as a decimal u64, false on a non-digit, an empty string, or an overflow. */
NYA_INTERNAL b8 _nya_http_files_parse_u64(const char* text, u64 size, OUT u64* out_value) __attr_no_discard;

/** Copies the uploaded `filename` into `out` as letters, digits, dot, dash, underscore and space only; never a path. */
NYA_INTERNAL void _nya_http_files_sanitize_name(const char* filename, u64 filename_size, OUT char out[NYA_HTTP_FILES_MAX_NAME]);

/** Copies a `type/subtype` content type into `out` when it is printable ASCII of that shape, else the octet-stream default. */
NYA_INTERNAL void _nya_http_files_content_type(const char* content_type, u64 content_type_size, OUT char out[NYA_HTTP_FILES_MAX_CONTENT_TYPE]);

/** The upload handler: stores the multipart file part and answers its id. */
NYA_INTERNAL NYA_HttpStatus _nya_http_files_handle_upload(NYA_HttpExchange* exchange);

/** The download handler: serves the file named by `?id=`, if the caller owns it. */
NYA_INTERNAL NYA_HttpStatus _nya_http_files_handle_download(NYA_HttpExchange* exchange);

// ROUTES

// Both handlers touch the one database on the ticking thread, so both routes are MAIN: no route runs on a worker, and two requests never race the
// same rows. Auth is NONE because the caller is resolved from the session cookie inside the handler, exactly as the accounts routes do.
NYA_INTERNAL const NYA_HttpRoute _NYA_HTTP_FILES_ROUTES[] = {
    { .method   = NYA_HTTP_METHOD_POST,
     .path     = NYA_HTTP_FILES_PATH,
     .affinity = NYA_HTTP_AFFINITY_MAIN,
     .handler  = _nya_http_files_handle_upload,
     .summary  = "Stores a multipart/form-data upload and answers its id",
     // 403 is here because dispatch's cross-site check can refuse a state-changing request before the handler runs; see http_router.h.
      .statuses = { NYA_HTTP_STATUS_CREATED,
                    NYA_HTTP_STATUS_BAD_REQUEST,
                    NYA_HTTP_STATUS_UNAUTHORIZED,
                    NYA_HTTP_STATUS_FORBIDDEN,
                    NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE,
                    NYA_HTTP_STATUS_UNSUPPORTED_MEDIA,
                    NYA_HTTP_STATUS_INTERNAL_ERROR } },
    { .method   = NYA_HTTP_METHOD_GET,
     .path     = NYA_HTTP_FILES_PATH,
     .affinity = NYA_HTTP_AFFINITY_MAIN,
     .handler  = _nya_http_files_handle_download,
     .summary  = "Downloads a file by id, if the caller owns it",
     .statuses = { NYA_HTTP_STATUS_OK,
                    NYA_HTTP_STATUS_BAD_REQUEST,
                    NYA_HTTP_STATUS_UNAUTHORIZED,
                    NYA_HTTP_STATUS_FORBIDDEN,
                    NYA_HTTP_STATUS_NOT_FOUND,
                    NYA_HTTP_STATUS_INTERNAL_ERROR } },
};

NYA_INTERNAL const NYA_HttpRouter _NYA_HTTP_FILES_ROUTER = {
    .name        = "files",
    .routes      = _NYA_HTTP_FILES_ROUTES,
    .route_count = nya_carray_length(_NYA_HTTP_FILES_ROUTES),
};

// PUBLIC API IMPLEMENTATION

const NYA_HttpRouter* nya_http_files_open(NYA_HttpFilesConfig config) {
    if (_NYA_HTTP_FILES_STATE.open) {
        nya_log_error("The file routes are already mounted; one process mounts them once.");
        return nullptr;
    }

    if (config.arena == nullptr || config.database == nullptr) {
        nya_log_error("The file routes need both an arena and a database.");
        return nullptr;
    }

    if (!nya_accounts_is_open()) {
        nya_log_error("nya_accounts_open must run before the file routes, whose ownership check reads the caller.");
        return nullptr;
    }

    NYA_BlobStore* store = nullptr;
    if (!nya_blob_store_open(config.arena, config.database, &store).ok) {
        nya_log_error("Could not open the blob store for the file routes.");
        return nullptr;
    }

    NYA_OrmTable* files = nullptr;
    if (!nya_orm_open(config.arena, config.database, &_NYA_HTTP_FILES_MODEL, FILES_TABLE, &files).ok) {
        nya_blob_store_close(store);
        nya_log_error("Could not open the file record table.");
        return nullptr;
    }

    if (!nya_orm_schema_migrate(files).ok) {
        nya_orm_close(files);
        nya_blob_store_close(store);
        nya_log_error("Could not migrate the file record table.");
        return nullptr;
    }

    _NYA_HTTP_FILES_STATE.config = config;
    _NYA_HTTP_FILES_STATE.store  = store;
    _NYA_HTTP_FILES_STATE.files  = files;
    _NYA_HTTP_FILES_STATE.open   = true;

    return &_NYA_HTTP_FILES_ROUTER;
}

void nya_http_files_close(void) {
    if (!_NYA_HTTP_FILES_STATE.open) return;

    nya_orm_close(_NYA_HTTP_FILES_STATE.files);
    nya_blob_store_close(_NYA_HTTP_FILES_STATE.store);
    nya_memset(&_NYA_HTTP_FILES_STATE, 0, sizeof(_NYA_HTTP_FILES_STATE));
}

// PRIVATE API IMPLEMENTATION

b8 _nya_http_files_parse_u64(const char* text, u64 size, OUT u64* out_value) {
    nya_assert(out_value != nullptr);

    *out_value = 0;

    if (text == nullptr || size == 0) return false;

    u64 value = 0;

    for (u64 index = 0; index < size; index++) {
        if (text[index] < '0' || text[index] > '9') return false;

        u64 digit = (u64)(text[index] - '0');

        // Refuse an overflow rather than wrap: a value that would not round-trip is not the id the caller meant.
        if (value > (UINT64_MAX - digit) / 10) return false;

        value = value * 10 + digit;
    }

    *out_value = value;

    return true;
}

void _nya_http_files_sanitize_name(const char* filename, u64 filename_size, OUT char out[NYA_HTTP_FILES_MAX_NAME]) {
    u64 length = 0;

    for (u64 index = 0; index < filename_size && length + 1 < NYA_HTTP_FILES_MAX_NAME; index++) {
        char character = filename[index];

        b8 keep = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
                  character == '.' || character == '-' || character == '_' || character == ' ';

        // A dropped byte — a path separator, a control byte, a quote — becomes an underscore rather than vanishing, so two names that differ only
        // there do not collide into one.
        out[length++] = keep ? character : '_';
    }

    out[length] = '\0';

    // A name that is only dots (".", "..") or empty is not a name a download can offer; give it the default.
    b8 meaningful = false;
    for (u64 index = 0; index < length; index++) {
        if (out[index] != '.' && out[index] != ' ') {
            meaningful = true;
            break;
        }
    }

    if (!meaningful) (void)snprintf(out, NYA_HTTP_FILES_MAX_NAME, "%s", FILES_DEFAULT_NAME);
}

void _nya_http_files_content_type(const char* content_type, u64 content_type_size, OUT char out[NYA_HTTP_FILES_MAX_CONTENT_TYPE]) {
    // Absent, too long, or not printable ASCII: store the neutral default rather than a type a browser might act on.
    b8 usable = content_type != nullptr && content_type_size > 0 && content_type_size < NYA_HTTP_FILES_MAX_CONTENT_TYPE;

    b8 has_slash = false;
    for (u64 index = 0; usable && index < content_type_size; index++) {
        u8 byte = (u8)content_type[index];

        // Printable ASCII only, and no separator a header value or a Content-Type parser splits on unexpectedly; the parser already excluded CR and
        // LF by keeping a value to one line, this keeps the rest boring.
        if (byte < 0x20 || byte >= 0x7F || byte == '"' || byte == ',') {
            usable = false;
            break;
        }
        if (content_type[index] == '/') has_slash = true;
    }

    if (!usable || !has_slash) {
        (void)snprintf(out, NYA_HTTP_FILES_MAX_CONTENT_TYPE, "%s", FILES_DEFAULT_CONTENT_TYPE);
        return;
    }

    memcpy(out, content_type, content_type_size);
    out[content_type_size] = '\0';
}

NYA_HttpStatus _nya_http_files_handle_upload(NYA_HttpExchange* exchange) {
    nya_assert(exchange != nullptr);
    nya_assert(_NYA_HTTP_FILES_STATE.open, "the file routes' handler ran with no mount");

    NYA_AccountUser caller = { 0 };
    if (!nya_http_accounts_caller(exchange, &caller)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    // Reject an oversize upload on the announced length, before the body is looked at. The server buffered it already (its own smaller ceiling is the
    // real bound; see the header), so this is the per-route limit on top.
    NYA_ConstCString length_header = nya_http_request_header(exchange->request, "content-length");
    if (length_header != nullptr) {
        u64 announced = 0;
        if (_nya_http_files_parse_u64(length_header, strlen(length_header), &announced) && announced > NYA_HTTP_FILES_MAX_UPLOAD_BYTES) {
            return nya_http_response_problem(exchange, NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE, "the upload is larger than this route accepts");
        }
    }

    NYA_ConstCString content_type = nya_http_request_header(exchange->request, "content-type");

    NYA_HttpMultipartReader reader = { 0 };
    if (!nya_http_multipart_reader_init(&reader, exchange->request->body, exchange->request->body_size, content_type).ok) {
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_UNSUPPORTED_MEDIA, "expected a multipart/form-data body with a boundary");
    }

    // The first part that carries a filename is the file; a plain field (no filename) is walked past. A body that is malformed part-way is a bad
    // request, not a stored half.
    NYA_HttpMultipartPart part  = { 0 };
    NYA_HttpMultipartPart file  = { 0 };
    b8                    found = false;

    for (NYA_HttpMultipartStep step; (step = nya_http_multipart_next(&reader, &part)) != NYA_HTTP_MULTIPART_DONE;) {
        if (step == NYA_HTTP_MULTIPART_MALFORMED) {
            return nya_http_response_problem(exchange, NYA_HTTP_STATUS_BAD_REQUEST, "the multipart body is malformed");
        }

        if (part.filename == nullptr) continue;

        if (part.body_size > NYA_HTTP_FILES_MAX_UPLOAD_BYTES) {
            return nya_http_response_problem(exchange, NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE, "the file is larger than this route accepts");
        }

        file  = part;
        found = true;
        break;
    }

    if (!found) return nya_http_response_problem(exchange, NYA_HTTP_STATUS_BAD_REQUEST, "the upload carried no file part");

    NYA_BlobId blob = { 0 };
    if (!nya_blob_put(_NYA_HTTP_FILES_STATE.store, file.body, file.body_size, &blob).ok) {
        nya_log_error("Could not store an uploaded file in the blob store.");
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    NYA_HttpFile record = {
        .owner   = (s64)caller.id,
        .size    = (s64)file.body_size,
        .created = (s64)exchange->now_s,
    };

    (void)snprintf(record.blob_id, sizeof(record.blob_id), "%s", blob.hex);
    _nya_http_files_sanitize_name(file.filename, file.filename_size, record.name);
    _nya_http_files_content_type(file.content_type, file.content_type_size, record.content_type);

    if (!nya_orm_insert(_NYA_HTTP_FILES_STATE.files, &record).ok) {
        nya_log_error("Could not record an uploaded file.");
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    // The id and the content address in a small JSON body; both are numbers and hex, so nothing here needs escaping.
    if (!nya_http_response_printf(
             exchange->response,
             NYA_HTTP_MEDIA_JSON,
             "{\"id\":%lld,\"blob\":\"%s\",\"size\":%lld}",
             (long long)record.id,
             record.blob_id,
             (long long)record.size
        )
             .ok) {
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    return NYA_HTTP_STATUS_CREATED;
}

NYA_HttpStatus _nya_http_files_handle_download(NYA_HttpExchange* exchange) {
    nya_assert(exchange != nullptr);
    nya_assert(_NYA_HTTP_FILES_STATE.open, "the file routes' handler ran with no mount");

    NYA_AccountUser caller = { 0 };
    if (!nya_http_accounts_caller(exchange, &caller)) return NYA_HTTP_STATUS_UNAUTHORIZED;

    char id_text[FILES_ID_TEXT] = { 0 };
    if (!nya_http_request_query_param(exchange->request, "id", id_text, sizeof(id_text))) {
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_BAD_REQUEST, "a download names a file with ?id=");
    }

    u64 id = 0;
    if (!_nya_http_files_parse_u64(id_text, strlen(id_text), &id)) {
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_BAD_REQUEST, "the id is not a number");
    }

    NYA_HttpFile record = { 0 };
    NYA_Error    found  = nya_orm_find(_NYA_HTTP_FILES_STATE.files, exchange->arena, nya_sql_s64((s64)id), &record);
    if (found.kind == NYA_ERROR_NOT_FOUND) return nya_http_response_problem(exchange, NYA_HTTP_STATUS_NOT_FOUND, "no such file");
    if (!found.ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // The ownership check: an id is not a capability, so knowing it is not permission. A file that is not the caller's is a 403, decided before a
    // byte of the blob is read.
    if (record.owner != (s64)caller.id) return nya_http_response_problem(exchange, NYA_HTTP_STATUS_FORBIDDEN, "this file is not yours");

    NYA_BlobId blob = { 0 };
    if (!nya_blob_id_parse(record.blob_id, &blob).ok) {
        nya_log_error("A file record holds a blob id that is not a valid content address.");
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    u8*       bytes = nullptr;
    u64       size  = 0;
    NYA_Error read  = nya_blob_get(_NYA_HTTP_FILES_STATE.store, blob, exchange->arena, &bytes, &size);
    if (read.kind == NYA_ERROR_NOT_FOUND) return nya_http_response_problem(exchange, NYA_HTTP_STATUS_NOT_FOUND, "no such file");
    if (!read.ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // Served as OTHER so the head writes no Content-Type from the enum, then the exact stored type as a header: the round trip of whatever was
    // uploaded, without the closed media enum in the way.
    if (!nya_http_response_bytes(exchange->response, bytes, size, NYA_HTTP_MEDIA_OTHER).ok) {
        nya_log_error("An uploaded file does not fit the response buffer, which the upload limit should have prevented.");
        return NYA_HTTP_STATUS_INTERNAL_ERROR;
    }

    if (!nya_http_response_header(exchange->response, "Content-Type", record.content_type).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // Always an attachment: the name is already reduced to safe bytes, so it cannot break the header, and the browser saves the file rather than
    // rendering it — which, with nosniff (a default security header), is what neutralises an uploaded text/html.
    char disposition[NYA_HTTP_FILES_MAX_NAME + 32] = { 0 };
    (void)snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", record.name);
    if (!nya_http_response_header(exchange->response, "Content-Disposition", disposition).ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    // A user's file is not for a shared cache to keep.
    if (!nya_http_response_header(exchange->response, "Cache-Control", "private, no-store").ok) return NYA_HTTP_STATUS_INTERNAL_ERROR;

    return NYA_HTTP_STATUS_OK;
}
