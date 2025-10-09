#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  // The echo user program writes each argument to standard output.
  // Expanded abbreviations:
  // - `argc`: argument count supplied by the exec framework.
  // - `argv`: argument vector (array of C strings) provided by the shell.
  //
  // Execution steps:
  // 1. Skip index 0 because it stores the program name.
  // 2. For every argument, emit it with `write` (system call that copies bytes
  //    to the file descriptor). File descriptor 1 always denotes standard
  //    output.
  // 3. Separate adjacent arguments with a single space, and terminate the
  //    sequence with a newline so redirections capture a whole line.
  //
  // Error handling is deferred to the kernel: `write` returns a byte count or
  // `-1`, but xv6 tools ignore short writes in this simple utility.
  int i;

  for(i = 1; i < argc; i++){
    write(1, argv[i], strlen(argv[i]));
    if(i + 1 < argc){
      write(1, " ", 1);
    } else {
      write(1, "\n", 1);
    }
  }
  exit(0);
}
