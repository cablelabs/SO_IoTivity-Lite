#include "oc_api.h"
#include "port/oc_clock.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>

static pthread_mutex_t mutex;
static pthread_cond_t cv;
static pthread_t event_loop_thread;
struct timespec ts;
static oc_resource_t *res = NULL;

static int quit = 0;
static char *fifopath = NULL;

static oc_so_info_t *so_info = NULL;

static void
get_diplomat(oc_request_t *request, oc_interface_mask_t iface_mask, void *user_data)
{
  (void) user_data;
  (void) request;
  (void) iface_mask;
  PRINT("get_diplomat called\n");
}

static void
*ocf_event_thread(void *data)
{
  (void)data;
  oc_clock_time_t next_event;
  while (quit != 1) {
    next_event = oc_main_poll();

    pthread_mutex_lock(&mutex);
    if (next_event == 0) {
      pthread_cond_wait(&cv, &mutex);
    }
    else {
      ts.tv_sec = (next_event / OC_CLOCK_SECOND);
      ts.tv_nsec = (next_event % OC_CLOCK_SECOND) * 1.e09 / OC_CLOCK_SECOND;
      pthread_cond_timedwait(&cv, &mutex, &ts);
    }
    pthread_mutex_unlock(&mutex);
  }
  return NULL;
}

static int
process_so_info(char *so_info)
{
  // TODO: Update resource values
  // sscanf(so_info, "%s %s", <var>, <var>);
  oc_notify_observers(res);
  return 0;
}

static void
poll_for_uuid(void)
{
  if (fifopath == NULL) {
    OC_ERR("Path to named pipe not set!");
    return;
  }
  if (mkfifo(fifopath, 0666) != 0) {
    OC_WRN("Failed to create named pipe for SO info. Already in place?\n");
  }

  PRINT("Polling for Streamlined Onboarding info from named pipe...\n");

  FILE *so_pipe = NULL;
  char read_buffer[256];

  while (quit != 1) {
    so_pipe = fopen(fifopath, "r");
    if (!so_pipe) {
      PRINT("Failed to open named pipe for SO info reading\n");
      break;
    }
    size_t read_size = fread(read_buffer, 1, 256, so_pipe);
    OC_DBG("Read size: %ld\n", read_size);
    if (read_size != 256 && feof(so_pipe)) {
      OC_DBG("Reached EOF\n");
    }
    OC_DBG("String read: %s\n", read_buffer);

    if (process_so_info(read_buffer) != 0) {
      OC_ERR("Failed to parse data as streamlined onboarding information");
    }

    if (so_pipe && fclose(so_pipe) != 0) {
      PRINT("Failed to close UUID pipe\n");
    }
  }
}

static void
register_resources(void)
{
  res = oc_new_resource(NULL, "/diplomat", 1, 0);
  oc_resource_bind_resource_type(res, "oic.r.diplomat");
  oc_resource_bind_resource_interface(res, OC_IF_R);
  oc_resource_set_default_interface(res, OC_IF_R);
  oc_resource_set_discoverable(res, true);
  oc_resource_set_observable(res, true);
  oc_resource_set_request_handler(res, OC_GET, get_diplomat, NULL);
  oc_add_resource(res);
}

static int
app_init(void)
{
  int ret = oc_init_platform("Linux", NULL, NULL);
  ret |= oc_add_device("/oic/d", "oic.d.diplomat", "DPP Diplomat", "ocf.1.0.0",
                       "ocf.res.1.0.0", NULL, NULL);
  return ret;
}

static void
signal_event_loop(void)
{
  pthread_mutex_lock(&mutex);
  pthread_cond_signal(&cv);
  pthread_mutex_unlock(&mutex);
}

void
handle_signal(int signal)
{
  (void)signal;
  quit = 1;
  signal_event_loop();
}

int
main(int argc, char *argv[])
{
  if (argc < 2) {
    OC_ERR("Must provide a path to a named pipe for SO info reading!");
    return -1;
  }
  fifopath = argv[1];

  int init;
  struct sigaction sa;
  sigfillset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = handle_signal;
  sigaction(SIGINT, &sa, NULL);

  static const oc_handler_t handler = { .init = app_init,
                                        .signal_event_loop = signal_event_loop,
                                        .register_resources = register_resources };

#ifdef OC_STORAGE
  oc_storage_config("./dpp_diplomat_creds");
#endif /* OC_STORAGE */

  init = oc_main_init(&handler);
  if (init < 0)
    return init;

  if (pthread_create(&event_loop_thread, NULL, &ocf_event_thread, NULL) != 0) {
    return -1;
  }

  poll_for_uuid();

  pthread_join(event_loop_thread, NULL);

  oc_main_shutdown();
  return 0;
}
