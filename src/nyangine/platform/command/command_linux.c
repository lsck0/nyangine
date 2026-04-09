#define _XOPEN_SOURCE 700
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "nyangine/nyangine.h"

NYA_Result nya_command_run(NYA_Command* command) {
  nya_assert(command != nullptr);
  nya_assert(command->program != nullptr && strlen(command->program) != 0);

  u64 start_time = nya_clock_get_timestamp_ms();

  s32 stdout_pipe[2];
  s32 stderr_pipe[2];
  if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) return nya_result_from_errno();

  if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
    nya_assert(command->arena != nullptr, "Arena must be provided when capturing output.");
    command->stdout_content = nya_string_create(command->arena);
    command->stderr_content = nya_string_create(command->arena);
  }

  pid_t pid = fork();
  if (pid < 0) return nya_result_from_errno();

  // CHILD
  if (pid == 0) {
    int devnull_fd = -1;
    if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
      // capture output: redirect stdout/stderr to pipe write ends
      dup2(stdout_pipe[1], STDOUT_FILENO);
      dup2(stderr_pipe[1], STDERR_FILENO);
    } else if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_SUPPRESS)) {
      // suppress output: redirect stdout/stderr to /dev/null
      devnull_fd = open("/dev/null", O_WRONLY);
      if (devnull_fd >= 0) {
        dup2(devnull_fd, STDOUT_FILENO);
        dup2(devnull_fd, STDERR_FILENO);
      }
    }

    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    if (devnull_fd >= 0) close(devnull_fd);

    // change working directory
    if (command->working_directory != nullptr && strlen(command->working_directory) != 0) {
      if (chdir(command->working_directory) != 0) {
        perror("chdir");
        exit(1);
      }
    }

    // set environment variables
    for (u32 i = 0; i < nya_carray_length(command->environment); i++) {
      if (command->environment[i] == nullptr) break;
      (void)putenv(command->environment[i]);
    }

    // build argv
    NYA_ConstCString* argv = nya_alloca((nya_carray_length(command->arguments) + 2) * sizeof(NYA_ConstCString));
    argv[0]                = command->program;
    nya_memcpy(argv + 1, command->arguments, nya_carray_length(command->arguments) * sizeof(NYA_ConstCString));
    argv[nya_carray_length(command->arguments) + 1] = nullptr;

    // do the thing
    execvp(command->program, (char* const*)argv);
    perror("execvp");
    exit(1);
  }

  // PARENT
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);

  // read stdout and stderr
  if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
    NYA_TRY(nya_fd_read(stdout_pipe[0], command->stdout_content));
    NYA_TRY(nya_fd_read(stderr_pipe[0], command->stderr_content));
  }
  close(stdout_pipe[0]);
  close(stderr_pipe[0]);

  // read exit code
  s32 status;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status)) {
    command->exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    command->exit_code = 128 + WTERMSIG(status);
  }

  u64 end_time               = nya_clock_get_timestamp_ms();
  command->execution_time_ms = end_time - start_time;

  return NYA_OK;
}

void nya_command_destroy(NYA_Command* command) {
  nya_assert(command != nullptr);

  if (nya_flag_check(command->flags, NYA_COMMAND_FLAG_OUTPUT_CAPTURE)) {
    nya_string_destroy(command->stdout_content);
    nya_string_destroy(command->stderr_content);
  }
}
