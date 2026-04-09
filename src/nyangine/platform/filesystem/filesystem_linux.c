#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <sys/stat.h>

#include "nyangine/nyangine.h"

b8 nya_filesystem_exists(NYA_ConstCString path) {
  b8 ok;

  struct stat path_stat;
  ok = stat(path, &path_stat) == 0;
  if (!ok) return false;

  return S_ISREG(path_stat.st_mode);
}

NYA_Result nya_filesystem_last_modified(NYA_ConstCString path, OUT u64* out_timestamp) {
  nya_assert(out_timestamp != nullptr);

  struct stat path_stat;
  b8          ok = stat(path, &path_stat) == 0;
  if (!ok) return nya_result_from_errno();

  *out_timestamp = (u64)path_stat.st_mtim.tv_sec * 1000ULL + (u64)(path_stat.st_mtim.tv_nsec / 1000000ULL);

  return NYA_OK;
}

NYA_Result nya_filesystem_move(NYA_ConstCString source, NYA_ConstCString destination) {
  b8 ok = rename(source, destination) == 0;
  if (!ok) return nya_result_from_errno();

  return NYA_OK;
}

NYA_Result nya_filesystem_copy(NYA_ConstCString source, NYA_ConstCString destination) {
  NYA_CLEANUP_WITH(close) s32 source_fd = open(source, O_RDONLY);
  if (source_fd < 0) return nya_result_from_errno();

  NYA_CLEANUP_WITH(close) s32 destination_fd = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (destination_fd < 0) return nya_result_from_errno();

  NYA_CLEANUP_WITH(nya_arena_destroy) NYA_Arena* arena  = nya_arena_create();
  NYA_String*                                  buffer = nya_string_create(arena);

  NYA_TRY(nya_fd_read(source_fd, buffer));
  NYA_TRY(nya_fd_write(destination_fd, buffer));

  return NYA_OK;
}

NYA_Result nya_filesystem_delete(NYA_ConstCString path) {
  b8 ok = remove(path) == 0;
  if (!ok) return nya_result_from_errno();

  return NYA_OK;
}
