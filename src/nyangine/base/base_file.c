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
