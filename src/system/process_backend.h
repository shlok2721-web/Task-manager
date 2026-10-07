#ifndef TASK_MANAGER_PROCESS_BACKEND_H
#define TASK_MANAGER_PROCESS_BACKEND_H

#include <glib.h>
#include <sys/types.h>
#include "../app/app.h"

typedef struct _ProcessBackend ProcessBackend;

ProcessBackend *process_backend_new(void);
void process_backend_free(ProcessBackend *backend);
gboolean process_backend_refresh(ProcessBackend *backend);
GPtrArray *process_backend_get_processes(ProcessBackend *backend);
gboolean process_backend_is_running(ProcessBackend *backend, const char *program_name);
gboolean process_backend_sample_pid_for_backend(ProcessBackend *backend, pid_t pid, ProcessSample *out);

#endif
