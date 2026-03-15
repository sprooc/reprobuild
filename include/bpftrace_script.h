#ifndef BPFTRACE_SCRIPT_H
#define BPFTRACE_SCRIPT_H

#include <string>

namespace BpftraceScript {

const std::string SCRIPT_TEMPLATE = R"(let @tracked = hash(65536);
BEGIN
{
  $pid = *;
  @tracked[$pid] = 1;

}

END
{
  clear(@tracked);
}

tracepoint:sched:sched_process_fork
/@tracked[(int64)args->parent_pid]/
{
  @tracked[(int64)args->child_pid] = 1;
}

tracepoint:syscalls:sys_enter_execve
/@tracked[(int64)pid]/
{
  printf("%d\x80execve %s\x80", pid, str(args->filename));
  $i = 1;
  while ($i < 64 && args->argv[(uint32)$i] != 0) {
    printf("%d\x80 %s\x80", pid, str(args->argv[(uint32)$i]));
    $i++;
  }
  printf("%d\x80\n\x80", pid);
}

tracepoint:syscalls:sys_enter_execveat
/@tracked[(int64)pid]/
{
  printf("%d\x80execveat %s\x80", pid, str(args->filename));
  $i = 1;
  while ($i < 64 && args->argv[(uint32)$i] != 0) {
    printf("%d\x80 %s\x80", pid, str(args->argv[(uint32)$i]));
    $i++;
  }
  printf("%d\x80\n\x80", pid);
}

tracepoint:syscalls:sys_enter_creat
/@tracked[(int64)pid]/
{
  printf("%d\x80creat %s\n\x80", pid, str(args->pathname));
}

tracepoint:syscalls:sys_enter_openat
/@tracked[(int64)pid]/
{
  if (args->filename[0] != 47) {
    
    $d = (struct dentry *)((struct task_struct *)curtask)->fs->pwd.dentry;
    $i = 0;
    while ($i < 64) {
      $fname = $d->d_name.name;
      if ($d == $d->d_parent || *$fname == 0) {
        break;
      }
      @path_parts[pid,$i] = $fname;
      $d = $d->d_parent;
      $i++;
    }
    $i = $i - 1;
    printf("%d\x80openat \x80", pid);
    while ($i >= 0) {
      printf("%d\x80/%s\x80", pid, str(@path_parts[pid,$i]));
      delete(@path_parts[pid,$i]);
      $i--;
    }
    printf("%d\x80/%s\x80", pid, str(args->filename));
  } else {
    printf("%d\x80openat %s\x80", pid, str(args->filename));
  }
  printf("%d\x80 %d\n\x80", pid, args->flags);
}
)";

}  // namespace BpftraceScript

#endif  // BPFTRACE_SCRIPT_H
