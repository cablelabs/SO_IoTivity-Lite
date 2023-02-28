#include "oc_api.h"
#include "port/oc_clock.h"
#include "ocf_dpp.h"
#include "oc_streamlined_onboarding.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static pthread_mutex_t mutex;
static pthread_cond_t cv;
static pthread_t event_loop_thread;
static struct timespec ts;
static oc_resource_t *res = NULL;

static int quit = 0;

static oc_so_info_t *so_info_list = NULL;

FILE *leases_pipe = NULL;

static void
get_diplomat(oc_request_t *request, oc_interface_mask_t iface_mask, void *user_data)
{
  (void) user_data;
  PRINT("GET /diplomat\n");
  if (so_info_list == NULL) {
    PRINT("No updated information to send\n");
    oc_send_response(request, OC_STATUS_OK);
    return;
  }

  oc_rep_start_root_object();
  switch (iface_mask) {
  case OC_IF_BASELINE:
    oc_process_baseline_interface(request->resource);
    /* fall through */
  case OC_IF_R:
    oc_rep_open_array(root, soinfo);
    oc_so_info_t *cur = so_info_list;

    while (cur != NULL) {
      OC_DBG("Setting SO info values with uuid: %s and cred: %s\n", cur->uuid, cur->cred);
      oc_rep_object_array_begin_item(soinfo);
      oc_rep_set_text_string(soinfo, uuid, cur->uuid);
      oc_rep_set_text_string(soinfo, cred, cur->cred);
      oc_rep_object_array_end_item(soinfo);
      cur = cur->next;
    }
    oc_rep_close_array(root, soinfo);
    break;
  default:
    break;
  }
  oc_rep_end_root_object();
  oc_send_response(request, OC_STATUS_OK);
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
process_so_info(oc_so_info_t *new_info)
{
  if (so_info_list == NULL) {
    so_info_list = new_info;
  }
  else {
    oc_so_append_info(so_info_list, new_info);
  }
  
  char read_buffer[12];
  

  while (quit != 1) {
    
    // Open the leases named pipe
    leases_pipe = fopen(getenv("DHCP_NAMED_PIPE"), "r");
    if (!leases_pipe) {
      OC_ERR("Failed to open named pipe for DHCP leases reading\n");
      break;
    }
    size_t read_size = fread(read_buffer, 1, 12, leases_pipe);
    PRINT("Read size: %ld\n", read_size);
    // if (read_size == 12) { // <--- Might need this instead of the below
    if (read_size == 12 && feof(leases_pipe)) {
      OC_DBG("Reached DHCP leases pipe EOF\n");
    }
    PRINT("String read: %s\n", read_buffer);

    if (leases_pipe && fclose(leases_pipe) != 0) {
      OC_ERR("Failed to close UUID pipe\n");
    }
  }
  
  int num_notified = oc_notify_observers(res);
  if (num_notified > 0) {
    PRINT("Notified %d observers\n", num_notified);
    OC_DBG("Freeing so_info_list\n");
    oc_so_info_free(so_info_list);
    so_info_list = NULL;
  }
  return 0;
}

static void
poll_for_so_info(void)
{
  PRINT("Polling for SO info events from hostap\n");
  oc_so_info_t *new_info = NULL;
  while (quit != 1) {
    new_info = dpp_so_info_poll();
    if (new_info) {
      process_so_info(new_info);
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
main(void)
{
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

  if(getenv("WPA_CTRL_IFACE") == NULL){
      PRINT("WPA_CTRL_IFACE environment not set\n");
       return -1;
  }
    if(getenv("DHCP_NAMED_PIPE") == NULL){
      PRINT("DHCP_NAMED_PIPE environment variable not set\n");
       return -1;
  }

  dpp_so_named_pipe_init(getenv("DHCP_NAMED_PIPE"));

  /* Hook into hostapd */
  if (dpp_so_init(getenv("WPA_CTRL_IFACE")) < 0) {
    PRINT("Failed to connect to hostapd");
    return -1;
  }
  PRINT("Connected to hostap socket.\n");

  if (pthread_create(&event_loop_thread, NULL, &ocf_event_thread, NULL) != 0) {
    return -1;
  }

  poll_for_so_info();

  pthread_join(event_loop_thread, NULL);

  dpp_so_teardown();
  oc_main_shutdown();
  oc_so_info_free(so_info_list);
  return 0;
}
