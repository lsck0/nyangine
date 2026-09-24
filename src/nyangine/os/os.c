#include "nyangine/os/os.h"

#if OS_WINDOWS
#include "nyangine/os/os_file_windows.c"
#include "nyangine/os/os_library_windows.c"
#include "nyangine/os/os_page_windows.c"
#include "nyangine/os/os_process_windows.c"
#include "nyangine/os/os_random_windows.c"
#include "nyangine/os/os_socket_windows.c"
#include "nyangine/os/os_thread_windows.c"
#include "nyangine/os/os_time_windows.c"
#elif OS_LINUX
#include "nyangine/os/os_file_linux.c"
#include "nyangine/os/os_library_linux.c"
#include "nyangine/os/os_page_linux.c"
#include "nyangine/os/os_process_linux.c"
#include "nyangine/os/os_random_linux.c"
#include "nyangine/os/os_socket_linux.c"
#include "nyangine/os/os_thread_linux.c"
#include "nyangine/os/os_time_linux.c"
// Last: a weak fill for a libc symbol the vendored deps may reference but an older glibc lacks.
#include "nyangine/os/os_libc_linux.c"
#else
#error "Unsupported OS"
#endif
