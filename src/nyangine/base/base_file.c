#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HANDLE FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_file_read_string(NYA_File* file, OUT NYA_String* out_content) {
    nya_assert(file != nullptr);
    nya_assert(out_content != nullptr);

    u8  buffer[4096];
    u64 got = 0;

    do {
        NYA_TRY(nya_file_read_bytes(file, buffer, sizeof(buffer), &got));
        if (got > 0) nya_string_extend(out_content, &((NYA_String){ .items = buffer, .length = got }));
    } while (got > 0);

    return NYA_OK;
}

NYA_Error nya_file_write_string(NYA_File* file, const NYA_String* content) __attr_overloaded {
    nya_assert(file != nullptr);
    nya_assert(content != nullptr);

    return nya_file_write_bytes(file, content->items, content->length);
}

NYA_Error nya_file_write_string(NYA_File* file, NYA_ConstCString content) __attr_overloaded {
    nya_assert(file != nullptr);
    nya_assert(content != nullptr);

    return nya_file_write_bytes(file, (const u8*)content, strlen(content));
}

NYA_Error nya_file_append_string(NYA_File* file, const NYA_String* content) __attr_overloaded {
    nya_assert(file != nullptr);
    nya_assert(content != nullptr);

    NYA_TRY(nya_file_seek(file, 0, NYA_FILE_SEEK_END));
    return nya_file_write_string(file, content);
}

NYA_Error nya_file_append_string(NYA_File* file, NYA_ConstCString content) __attr_overloaded {
    nya_assert(file != nullptr);
    nya_assert(content != nullptr);

    NYA_TRY(nya_file_seek(file, 0, NYA_FILE_SEEK_END));
    return nya_file_write_string(file, content);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FILE FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * READ
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_file_read(const char* path, OUT NYA_String* out_content) __attr_overloaded {
    nya_assert(path != nullptr);
    nya_assert(out_content != nullptr);

    NYA_File file;
    NYA_TRY(nya_file_open(path, NYA_FILE_MODE_READ, &file));
    defer nya_file_close(&file);

    return nya_file_read_string(&file, out_content);
}

NYA_Error nya_file_read(const NYA_String* path, OUT NYA_String* out_content) __attr_overloaded {
    nya_assert(path != nullptr);
    nya_assert(out_content != nullptr);

    NYA_CString c_path = nya_alloca((path->length + 1) * sizeof(char));
    memcpy((void*)c_path, path->items, path->length);
    c_path[path->length] = '\0';

    return nya_file_read(c_path, out_content);
}

/*
 * ─────────────────────────────────────────────────────────
 * WRITE
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_file_write(const char* path, const NYA_String* content) __attr_overloaded {
    nya_assert(path != nullptr);
    nya_assert(content != nullptr);

    NYA_File file;
    NYA_TRY(nya_file_open(path, NYA_FILE_MODE_WRITE | NYA_FILE_MODE_TRUNCATE, &file));
    defer nya_file_close(&file);

    return nya_file_write_string(&file, content);
}

NYA_Error nya_file_write(const NYA_String* path, const NYA_String* content) __attr_overloaded {
    nya_assert(path != nullptr);
    nya_assert(content != nullptr);

    NYA_CString c_path = nya_alloca((path->length + 1) * sizeof(char));
    memcpy((void*)c_path, path->items, path->length);
    c_path[path->length] = '\0';

    return nya_file_write(c_path, content);
}

NYA_Error nya_file_write(const char* path, NYA_ConstCString content) __attr_overloaded {
    nya_assert(path != nullptr);
    nya_assert(content != nullptr);

    return nya_file_write(
        path,
        &((NYA_String){
            .items  = (u8*)content,
            .length = strlen(content),
        })
    );
}

NYA_Error nya_file_write(const NYA_String* path, NYA_ConstCString content) __attr_overloaded {
    nya_assert(path != nullptr);
    nya_assert(content != nullptr);

    NYA_CString c_path = nya_alloca((path->length + 1) * sizeof(char));
    memcpy((void*)c_path, path->items, path->length);
    c_path[path->length] = '\0';

    return nya_file_write(
        c_path,
        &((NYA_String){
            .items  = (u8*)content,
            .length = strlen(content),
        })
    );
}

/*
 * ─────────────────────────────────────────────────────────
 * APPEND
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_file_append(const char* path, const NYA_String* content) __attr_overloaded {
    nya_assert(path != nullptr);
    nya_assert(content != nullptr);

    NYA_File file;
    NYA_TRY(nya_file_open(path, NYA_FILE_MODE_APPEND, &file));
    defer nya_file_close(&file);

    return nya_file_write_string(&file, content);
}

NYA_Error nya_file_append(const NYA_String* path, const NYA_String* content) __attr_overloaded {
    nya_assert(path != nullptr);
    nya_assert(content != nullptr);

    NYA_CString c_path = nya_alloca((path->length + 1) * sizeof(char));
    memcpy((void*)c_path, path->items, path->length);
    c_path[path->length] = '\0';

    return nya_file_append(c_path, content);
}

NYA_Error nya_file_append(const char* path, NYA_ConstCString content) __attr_overloaded {
    nya_assert(path != nullptr);
    nya_assert(content != nullptr);

    return nya_file_append(
        path,
        &((NYA_String){
            .items  = (u8*)content,
            .length = strlen(content),
        })
    );
}

NYA_Error nya_file_append(const NYA_String* path, NYA_ConstCString content) __attr_overloaded {
    nya_assert(path != nullptr);
    nya_assert(content != nullptr);

    NYA_CString c_path = nya_alloca((path->length + 1) * sizeof(char));
    memcpy((void*)c_path, path->items, path->length);
    c_path[path->length] = '\0';

    return nya_file_append(
        c_path,
        &((NYA_String){
            .items  = (u8*)content,
            .length = strlen(content),
        })
    );
}

/*
 * ─────────────────────────────────────────────────────────
 * ATOMIC WRITE
 * ─────────────────────────────────────────────────────────
 */

/** Shared by every thread, so two writers in one process never pick the same temp name. */
NYA_INTERNAL _Atomic u32 _nya_file_atomic_counter = 0;

#ifdef NYA_TESTING

NYA_INTERNAL thread_local NYA_FileAtomicStep  _nya_file_atomic_fault_step = NYA_FILE_ATOMIC_STEP_COUNT;
NYA_INTERNAL thread_local NYA_FileAtomicFault _nya_file_atomic_fault      = NYA_FILE_ATOMIC_FAULT_NONE;

void nya_file_write_atomic_fault_set(NYA_FileAtomicStep step, NYA_FileAtomicFault fault) {
    nya_assert(step < NYA_FILE_ATOMIC_STEP_COUNT);
    nya_assert(fault < NYA_FILE_ATOMIC_FAULT_COUNT);

    _nya_file_atomic_fault_step = step;
    _nya_file_atomic_fault      = fault;
}

/** The fault armed for `step`, disarmed as it is taken so one arm is one fault. */
NYA_INTERNAL NYA_FileAtomicFault _nya_file_atomic_fault_take(NYA_FileAtomicStep step) {
    if (_nya_file_atomic_fault_step != step) return NYA_FILE_ATOMIC_FAULT_NONE;

    NYA_FileAtomicFault fault   = _nya_file_atomic_fault;
    _nya_file_atomic_fault_step = NYA_FILE_ATOMIC_STEP_COUNT;
    _nya_file_atomic_fault      = NYA_FILE_ATOMIC_FAULT_NONE;

    return fault;
}

#endif // NYA_TESTING

/**
 * The file a write to `path` should land on. A symlink is resolved so the link survives; renaming over
 * it would replace the link with a plain file.
 * */
NYA_INTERNAL NYA_ConstCString _nya_file_atomic_target(NYA_Arena* arena, NYA_ConstCString path) {
    NYA_FileInfo info = { 0 };
    if (!nya_filesystem_info(path, &info).ok || info.type != NYA_FILE_TYPE_SYMLINK) return path;

    // a dangling link resolves to nothing, and replacing the link itself is the only write left.
    NYA_String* resolved = nullptr;
    if (!nya_filesystem_absolute(arena, path, &resolved).ok) return path;

    return nya_string_to_cstring(arena, resolved);
}

/** Creates a temp file beside `target` that nobody else can be holding, and opens it for writing. */
NYA_INTERNAL NYA_Error _nya_file_atomic_open_temporary(NYA_Arena* arena, NYA_ConstCString target, OUT NYA_File* out_file, OUT NYA_CString* out_path) {
    u32       process_id = nya_os_process_id();
    NYA_Error error      = NYA_OK;

    // exclusive, so a stale temp left by a dead process with a recycled pid is stepped over, not reused.
    for (u32 attempt = 0; attempt < NYA_FILE_ATOMIC_ATTEMPTS_MAX; attempt++) {
        u32         counter = atomic_fetch_add(&_nya_file_atomic_counter, 1);
        NYA_CString path    = nya_string_to_cstring(arena, nya_string_sprintf(arena, "%s." FMTu32 "." FMTu32 ".tmp", target, process_id, counter));

        error = nya_file_open(path, NYA_FILE_MODE_WRITE | NYA_FILE_MODE_EXCLUSIVE, out_file);
        if (error.ok) {
            *out_path = path;
            return NYA_OK;
        }
        if (error.kind != NYA_ERROR_ALREADY_EXISTS) return error;
    }

    return error;
}

NYA_Error nya_file_write_atomic(const char* path, const NYA_String* content) __attr_overloaded {
    nya_assert(path != nullptr);
    nya_assert(content != nullptr);

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "file_write_atomic");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_ConstCString target    = _nya_file_atomic_target(&scratch, path);
    NYA_CString      temporary = nullptr;
    NYA_File         file      = { 0 };
    defer            nya_file_close(&file);

    // the steps as a sequence, so a test can stop it between any two of them the same way.
    NYA_Error error = NYA_OK;
    for (NYA_FileAtomicStep step = 0; step < NYA_FILE_ATOMIC_STEP_COUNT && error.ok; step++) {
#ifdef NYA_TESTING
        NYA_FileAtomicFault fault = _nya_file_atomic_fault_take(step);
        // nothing cleaned up, which is the state a real crash here leaves on disk.
        if (fault == NYA_FILE_ATOMIC_FAULT_CRASH) return nya_error(NYA_ERROR_IO, "simulated crash writing '%s'", path);
        if (fault == NYA_FILE_ATOMIC_FAULT_FAIL) {
            error = nya_error(NYA_ERROR_IO, "simulated failure writing '%s'", path);
            break;
        }
#endif

        switch (step) {
            case NYA_FILE_ATOMIC_STEP_OPEN: error = _nya_file_atomic_open_temporary(&scratch, target, &file, &temporary); break;
            case NYA_FILE_ATOMIC_STEP_WRITE:
                // an empty string may have no buffer at all, and writing nothing needs none.
                error = content->length == 0 ? NYA_OK : nya_file_write_bytes(&file, content->items, content->length);
                break;
            case NYA_FILE_ATOMIC_STEP_SYNC: error = nya_file_flush(&file); break;
            case NYA_FILE_ATOMIC_STEP_REPLACE:
                // closed first: Windows will not rename a file that is still open.
                nya_file_close(&file);
                error = nya_filesystem_replace(temporary, target);
                break;
            default: nya_unreachable();
        }
    }

    if (!error.ok && temporary != nullptr) {
        nya_file_close(&file);
        // already gone when the rename went through and only the directory sync after it failed.
        if (nya_filesystem_exists(temporary)) (void)nya_filesystem_delete(temporary);
    }

    return error;
}

NYA_Error nya_file_write_atomic(const char* path, NYA_ConstCString content) __attr_overloaded {
    nya_assert(path != nullptr);
    nya_assert(content != nullptr);

    return nya_file_write_atomic(path, &((NYA_String){ .items = (u8*)content, .length = strlen(content) }));
}
