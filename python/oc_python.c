/*
// Copyright (c) 2017-2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
*/

//#define OC_SERVER 

#include "oc_api.h"
#include "oc_core_res.h"
#include "oc_obt.h"
#include "port/oc_clock.h"
#include "security/oc_obt_internal.h"
#include <unistd.h>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#else
#error "Unsupported OS"
#endif
#include <signal.h>
#include <stdio.h>
#include <string.h>



#ifdef OC_SO
#include "oc_streamlined_onboarding.h"
#endif

#define MAX_NUM_DEVICES (50)
#define MAX_NUM_RESOURCES (100)
#define MAX_NUM_RT (50)
#define MAX_URI_LENGTH (30)

/* Structure in app to track currently discovered owned/unowned devices */
typedef struct device_handle_t
{
  struct device_handle_t *next;
  oc_uuid_t uuid;
  char device_name[64];
} device_handle_t;

/* Pool of device handles */
OC_MEMB(device_handles, device_handle_t, MAX_OWNED_DEVICES);
/* List of known owned devices */
OC_LIST(owned_devices);
/* List of known un-owned devices */
OC_LIST(unowned_devices);

/* Diplomat resource information */
#ifdef OC_SO
static char diplomat_uri[MAX_URI_LENGTH];
static oc_endpoint_t *diplomat_ep;
#endif



#if defined(_WIN32)
static HANDLE event_thread;
static CRITICAL_SECTION app_sync_lock;
static CONDITION_VARIABLE cv;
static CRITICAL_SECTION cs;

/* OS specific definition for lock/unlock */
#define otb_mutex_lock(m) EnterCriticalSection(&m)
#define otb_mutex_unlock(m) LeaveCriticalSection(&m)

#elif defined(__linux__)
static pthread_t event_thread;
static pthread_mutex_t app_sync_lock;
static pthread_mutex_t mutex;
static pthread_cond_t cv;

/* OS specific definition for lock/unlock */
#define otb_mutex_lock(m) pthread_mutex_lock(&m)
#define otb_mutex_unlock(m) pthread_mutex_unlock(&m)

static struct timespec ts;
#endif
static int quit = 0;


/**
* callback prototype to inform python layer that the onboarded/unonboarded list have changed
*
*/
typedef void (*changedCB) (char* uuid, char* state, char* event);
typedef void (*diplomatCB) (char* anchor, char* uri, char* state, char* event, char* target, char* target_cred);
typedef void (*resourceCB) (char* anchor, char* uri, char* types, char* interfaces);
typedef void (*clientCB) (char* uuid, char* state, char* event);

/**
* structure with the callback
*
*/
struct py_cb_struct
{
    changedCB changedFCB;
    diplomatCB diplomatFCB;
    resourceCB resourceFCB;
    clientCB clientFCB;
};

/**
* declaration of the callback
*
*/
struct py_cb_struct my_CBFunctions; 

/**
 * Function to return response strings
 *
 */


static inline char *stringFromResponse(int code)
{
    static char *strings[] = {  "STATUS_OK", 
				  "STATUS_CREATED"
				  "STATUS_CHANGED",
				  "STATUS_DELETED",
				  "STATUS_NOT_MODIFIED",
				  "STATUS_BAD_REQUEST",
				  "STATUS_UNAUTHORIZED",
				  "STATUS_BAD_OPTION",
				  "STATUS_FORBIDDEN",
				  "STATUS_NOT_FOUND",
				  "STATUS_METHOD_NOT_ALLOWED",
				  "STATUS_NOT_ACCEPTABLE",
				  "STATUS_REQUEST_ENTITY_TOO_LARGE",
				  "STATUS_UNSUPPORTED_MEDIA_TYPE",
				  "STATUS_INTERNAL_SERVER_ERROR",
				  "STATUS_NOT_IMPLEMENTED",
				  "STATUS_BAD_GATEWAY",
				  "STATUS_SERVICE_UNAVAILABLE",
				  "STATUS_GATEWAY_TIMEOUT",
				  "STATUS_PROXYING_NOT_SUPPORTED",
				  "__NUM_STATUS_CODES__",
				  "IGNORE",
				  "PING_TIMEOUT" };
    return strings[code];
}


/**
* function to install callbacks, called from python
*
*/
void install_changedCB(changedCB changedCB) {
   PRINT("[C]install_changedCB\n");
   my_CBFunctions.changedFCB = changedCB;
}

/**
* function to install diplomat callbacks, called from python
*
*/
void install_diplomatCB(diplomatCB diplomatCB) {
   PRINT("[C]install_diplomatCB\n");
   my_CBFunctions.diplomatFCB = diplomatCB;
}

/**
* function to install resource callbacks, called from python
*
*/
void install_resourceCB(resourceCB resourceCB) {
   PRINT("[C]install_resourceCB\n");
   my_CBFunctions.resourceFCB = resourceCB;
}
/**
* function to install client callbacks, called from python
*
*/
void install_clientCB(clientCB clientCB) {
   PRINT("[C]install_clientCB\n");
   my_CBFunctions.clientFCB = clientCB;
}

/**
* function to call the callback to python.
*
*/
void inform_python(const char* uuid, const char* state, const char* event)
{
  //PRINT("[C]inform_python %p\n",my_CBFunctions.changedFCB);
  if (my_CBFunctions.changedFCB != NULL) {
    my_CBFunctions.changedFCB((char*)uuid,(char*)state,(char*)event);
  }
}

void inform_resource_python(const char* anchor, const char* uri, const char* types, const char* interfaces)
{
  //PRINT("[C]inform_resource_python %p %s %s [%s] [%s]\n",my_CBFunctions.resourceFCB, anchor, uri, types, interfaces);
  if (my_CBFunctions.resourceFCB != NULL) {
    my_CBFunctions.resourceFCB((char*)anchor, (char*)uri, (char*)types, (char*)interfaces);
  }
}

/**
* function to call the callback for diplomats to python.
*
*/
void inform_diplomat_python(const char* anchor, const char* uri, const char* state, const char* event, const char* target, const char* target_cred)
{
  PRINT("[C]inform diplomat python %p\n",my_CBFunctions.diplomatFCB);
  if (my_CBFunctions.diplomatFCB != NULL) {
    my_CBFunctions.diplomatFCB((char*)anchor,(char*)uri,(char*)state,(char*)event,(char*)target,(char*)target_cred);
  }
}

/**
* function to call the callback for clients to python.
*
*/
void inform_client_python(const char* uuid, const char* state, const char* event)
{
  PRINT("[C]inform_python %p\n",my_CBFunctions.clientFCB);
  if (my_CBFunctions.clientFCB != NULL) {
    my_CBFunctions.clientFCB((char*)uuid,(char*)state,(char*)event);
  }
}

/**
* function to print the returned cbor as JSON
*
*/
void
print_rep(oc_rep_t* rep, bool pretty_print)
{
  char* json;
  size_t json_size;
  json_size = oc_rep_to_json(rep, NULL, 0, pretty_print);
  json = (char*)malloc(json_size + 1);
  oc_rep_to_json(rep, json, json_size + 1, pretty_print);
  printf("%s\n", json);
  free(json);
}


/**
* function to convert the uuid to the device handle
*
*/
device_handle_t* py_getdevice_from_uuid(char* uuid, int owned)
{
  device_handle_t *device = NULL;
  if (owned == 1) {
    device = (device_handle_t *)oc_list_head(owned_devices);
  }
  else {
    device = (device_handle_t *)oc_list_head(unowned_devices);
  }

  int i = 0;
  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    if (strcmp(di, uuid) == 0)
    {
      return device;
    }
    i++;
    device = device->next;
  }
  return NULL;
}



#define SCANF(...)                                                             \
  do {                                                                         \
    if (scanf(__VA_ARGS__) <= 0) {                                             \
      PRINT("[C]ERROR Invalid input\n");                                          \
      fflush(stdin);                                                           \
    }                                                                          \
  } while (0)


static int
app_init(void)
{
  //PRINT("[C]app_init\n");
  int ret = oc_init_platform("OCF", NULL, NULL);
  ret |= oc_add_device("/oic/d", "oic.d.dots", "OBT", "ocf.2.2.2",
                       "ocf.res.1.0.0,ocf.sh.1.0.0", NULL, NULL);
  oc_device_bind_resource_type(0, "oic.d.ams");
  oc_device_bind_resource_type(0, "oic.d.cms");
  return ret;
}

/**
* function to initate the obt
* this is a callback function of device init.
*
*/
static void
issue_requests(void)
{
  //PRINT("[C]issue_requests\n");
  int retval = oc_obt_init();
  PRINT("[C]obt initialized! %d\n", retval);
}

/**
* event loop (window/linux) used for the python initated thread.
*
*/
static void
signal_event_loop(void)
{
#if defined(_WIN32)
  WakeConditionVariable(&cv);
#elif defined(__linux__)
  otb_mutex_lock(mutex);
  pthread_cond_signal(&cv);
  otb_mutex_unlock(mutex);
#endif
}

/**
* function to quit the event loop
*
*/
void
python_exit(int signal)
{
  (void)signal;
  quit = 1;
  signal_event_loop();
}

/**
* the event thread (windows or linux)
*
*/
#if defined(_WIN32)
DWORD WINAPI
ocf_event_thread(LPVOID lpParam)
{
  oc_clock_time_t next_event;
  while (quit != 1) {
    otb_mutex_lock(app_sync_lock);
    next_event = oc_main_poll();
    otb_mutex_unlock(app_sync_lock);

    if (next_event == 0) {
      SleepConditionVariableCS(&cv, &cs, INFINITE);
    } else {
      oc_clock_time_t now = oc_clock_time();
      if (now < next_event) {
        SleepConditionVariableCS(
          &cv, &cs, (DWORD)((next_event - now) * 1000 / OC_CLOCK_SECOND));
      }
    }
  }

  oc_main_shutdown();
  oc_obt_shutdown();
  return TRUE;
}
#elif defined(__linux__)
static void *
ocf_event_thread(void *data)
{
  (void)data;
  oc_clock_time_t next_event;
  while (quit != 1) {
    otb_mutex_lock(app_sync_lock);
    next_event = oc_main_poll();
    otb_mutex_unlock(app_sync_lock);

    otb_mutex_lock(mutex);
    if (next_event == 0) {
      pthread_cond_wait(&cv, &mutex);
    } else {
      ts.tv_sec = (next_event / OC_CLOCK_SECOND);
      ts.tv_nsec = (next_event % OC_CLOCK_SECOND) * 1.e09 / OC_CLOCK_SECOND;
      pthread_cond_timedwait(&cv, &mutex, &ts);
    }
    otb_mutex_unlock(mutex);
  }
  oc_main_shutdown();
  oc_obt_shutdown();
  return NULL;
}
#endif


/* App utility functions */
static device_handle_t *
is_device_in_list(oc_uuid_t *uuid, oc_list_t list)
{
  device_handle_t *device = (device_handle_t *)oc_list_head(list);
  while (device != NULL) {
    if (memcmp(device->uuid.id, uuid->id, 16) == 0) {
      return device;
    }
    device = device->next;
  }
  return NULL;
}

static device_handle_t *
get_obt_device(oc_uuid_t *uuid,const char *device_name)
{
     device_handle_t *device;
    device = oc_memb_alloc(&device_handles);
    if (!device) {
      return false;
    }
    memcpy(device->uuid.id, uuid->id, 16);
  if (device_name) {
    size_t len = strlen(device_name);
    len = (len > 63) ? 63 : len;
    strncpy(device->device_name, device_name, len);
    device->device_name[len] = '\0';
  } else {
    device->device_name[0] = '\0';
  }

  return device;
}

static bool
add_device_to_list(oc_uuid_t *uuid, const char *device_name, oc_list_t list)
{
  device_handle_t *device = is_device_in_list(uuid, list);

  if (!device) {
    device = oc_memb_alloc(&device_handles);
    if (!device) {
      return false;
    }
    memcpy(device->uuid.id, uuid->id, 16);
    oc_list_add(list, device);
  }

  if (device_name) {
    size_t len = strlen(device_name);
    len = (len > 63) ? 63 : len;
    strncpy(device->device_name, device_name, len);
    device->device_name[len] = '\0';
  } else {
    device->device_name[0] = '\0';
  }
  return true;
}

void
empty_device_list(oc_list_t list)
{
  device_handle_t *device = (device_handle_t *)oc_list_pop(list);
  while (device != NULL) {
    oc_memb_free(&device_handles, device);
    device = (device_handle_t *)oc_list_pop(list);
  }
}
/* End of app utility functions */

/* App invocations of oc_obt APIs */
/**
* CB function on getting the device data.
* generic callback for owned/unowned devices
*/
static void
get_device(oc_client_response_t *data)
{
  oc_rep_t *rep = data->payload;
  char *di = NULL, *n = NULL;
  size_t di_len = 0, n_len = 0;

  if (oc_rep_get_string(rep, "di", &di, &di_len)) {
    oc_uuid_t uuid;
    oc_str_to_uuid(di, &uuid);
    if (!oc_rep_get_string(rep, "n", &n, &n_len)) {
      n = NULL;
      n_len = 0;
    }

    PRINT("[C] adding device to list.%s.%s\n",di, n);
    add_device_to_list(&uuid, n, data->user_data);

    bool owned = oc_obt_is_owned_device(&uuid);
    char* state = "";
    if (owned){
	state="owned";
    }else{
	state="unowned";
    }
    PRINT("[C] adding device to list...\n");
    inform_python(di,state,NULL);
  }
}

static void
unowned_device_cb(oc_uuid_t *uuid, oc_endpoint_t *eps, void *data)
{
  (void)data;
  (void) uuid;
  //char di[37];
  //oc_uuid_to_str(uuid, di, 37);
  oc_endpoint_t *ep = eps;

  //while (eps != NULL) {
    //PRINTipaddr(*eps);
    //PRINT("[C]\n");
    //eps = eps->next;
  //}

  oc_do_get("/oic/d", ep, NULL, &get_device, HIGH_QOS, unowned_devices);
}

static void
owned_device_cb(oc_uuid_t *uuid, oc_endpoint_t *eps, void *data)
{
  (void)data;
  (void) uuid;
  //(void) eps;
  //char di[37];
  //oc_uuid_to_str(uuid, di, 37);
  oc_endpoint_t *ep = eps;

  //while (eps != NULL) {
    //PRINTipaddr(*eps);
    //PRINT("[C]\n");
  //  eps = eps->next;
  //}

  oc_do_get("/oic/d", ep, NULL, &get_device, HIGH_QOS, owned_devices);
}

void
discover_owned_devices(int scope)
{
  //PRINT("[C]discover_owned_devices: scope %d\n", scope);
  otb_mutex_lock(app_sync_lock);
  if (scope == 0x02) {
    oc_obt_discover_owned_devices(owned_device_cb, NULL);
  } else if (scope == 0x03) {
    oc_obt_discover_owned_devices_realm_local_ipv6(owned_device_cb, NULL);
  } else if (scope == 0x05) {
    oc_obt_discover_owned_devices_site_local_ipv6(owned_device_cb, NULL);
  }
  otb_mutex_unlock(app_sync_lock);
  signal_event_loop();
}
void
discover_unowned_devices(int scope)
{
  //PRINT("[C]discover_unowned_devices: scope %d\n", scope);
  otb_mutex_lock(app_sync_lock);
#ifdef OC_SO
  if (scope == 0x02) {
    oc_obt_discover_unowned_devices(unowned_device_cb, NULL,NULL);
  } else if (scope == 0x03) {
    oc_obt_discover_unowned_devices_realm_local_ipv6(unowned_device_cb, NULL,NULL);
  } else if (scope == 0x05) {
    oc_obt_discover_unowned_devices_site_local_ipv6(unowned_device_cb, NULL,NULL);
  }
#else
  if (scope == 0x02) {
    oc_obt_discover_unowned_devices(unowned_device_cb, NULL);
  } else if (scope == 0x03) {
    oc_obt_discover_unowned_devices_realm_local_ipv6(unowned_device_cb, NULL);
  } else if (scope == 0x05) {
    oc_obt_discover_unowned_devices_site_local_ipv6(unowned_device_cb, NULL);
  }
#endif
  otb_mutex_unlock(app_sync_lock);
  signal_event_loop();
}

void
py_discover_unowned_devices(int scope)
{
  //PRINT("[C]discover_unowned_devices: scope %d\n", scope);
  otb_mutex_lock(app_sync_lock);
#ifdef OC_SO
  if (scope == 0x02) {
    oc_obt_discover_unowned_devices(unowned_device_cb, NULL,NULL);
  } else if (scope == 0x03) {
    oc_obt_discover_unowned_devices_realm_local_ipv6(unowned_device_cb, NULL,NULL);
  } else if (scope == 0x05) {
    oc_obt_discover_unowned_devices_site_local_ipv6(unowned_device_cb, NULL,NULL);
  }
#else
  if (scope == 0x02) {
    oc_obt_discover_unowned_devices(unowned_device_cb, NULL);
  } else if (scope == 0x03) {
    oc_obt_discover_unowned_devices_realm_local_ipv6(unowned_device_cb, NULL);
  } else if (scope == 0x05) {
    oc_obt_discover_unowned_devices_site_local_ipv6(unowned_device_cb, NULL);
  }
#endif
  otb_mutex_unlock(app_sync_lock);
  signal_event_loop();
}

static void
otm_rdp_cb(oc_uuid_t *uuid, int status, void *data)
{
  device_handle_t *device = (device_handle_t *)data;
  memcpy(device->uuid.id, uuid->id, 16);
  char di[37];
  oc_uuid_to_str(uuid, di, 37);

  if (status >= 0) {
    PRINT("[C]\nSuccessfully performed OTM on device %s\n", di);
    oc_list_add(owned_devices, device);
    inform_python(NULL,NULL,NULL);
  } else {
    PRINT("[C]\nERROR performing ownership transfer on device %s\n", di);
    oc_memb_free(&device_handles, device);
  }
}

void
otm_rdp(void)
{
  if (oc_list_length(unowned_devices) == 0) {
    PRINT("[C]\nPlease Re-discover Unowned devices\n");
    return;
  }

  device_handle_t *device = (device_handle_t *)oc_list_head(unowned_devices);
  device_handle_t *devices[MAX_NUM_DEVICES];
  int i = 0, c;

  PRINT("[C]\nUnowned Devices:\n");
  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    devices[i] = device;
    i++;
    device = device->next;
  }
  PRINT("[C]\n\nSelect device: ");
  SCANF("%d", &c);
  if (c < 0 || c >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  unsigned char pin[24];
  PRINT("[C]\nEnter Random PIN: ");
  SCANF("%10s", pin);

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_perform_random_pin_otm(
    &devices[c]->uuid, pin, strlen((const char *)pin), otm_rdp_cb, devices[c]);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to perform Random PIN OTM\n");
    /* Having issued an OTM request, remove this item from the unowned device
     * list
     */
    oc_list_remove(unowned_devices, devices[c]);
  } else {
    PRINT("[C]\nERROR issuing request to perform Random PIN OTM\n");
  }

  otb_mutex_unlock(app_sync_lock);
}

void
py_otm_rdp(char* uuid, char* pin)
{
  device_handle_t *device = (device_handle_t *)oc_list_head(unowned_devices);
  device_handle_t *devices[MAX_NUM_DEVICES];
  int i = 0, c=-1;

  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    devices[i] = device;
    if (strcmp(uuid, di) == 0) {
      c = i;
    }
    i++;
    device = device->next;
  }
  if (c == -1)
  {
    PRINT("[C] ERROR: Invalid uuid\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_perform_random_pin_otm(
    &devices[c]->uuid, (const unsigned char *)pin, strlen((const char *)pin), otm_rdp_cb, devices[c]);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to perform Random PIN OTM\n");
    /* Having issued an OTM request, remove this item from the unowned device
     * list
     */
    oc_list_remove(unowned_devices, devices[c]);
  } else {
    PRINT("[C]\nERROR issuing request to perform Random PIN OTM\n");
  }

  otb_mutex_unlock(app_sync_lock);
}

static void
random_pin_cb(oc_uuid_t *uuid, int status, void *data)
{
  (void)data;
  char di[37];
  oc_uuid_to_str(uuid, di, 37);

  if (status >= 0) {
    PRINT("[C]\nSuccessfully requested device %s to generate a Random PIN\n", di);
    inform_python(di,"unowned","random_pin_request");
  } else {
    PRINT("[C]\nERROR requesting device %s to generate a Random PIN\n", di);
    inform_python(di,"unowned","random_pin_request_error");
  }
}

void
request_random_pin(void)
{
  if (oc_list_length(unowned_devices) == 0) {
    PRINT("[C]\nPlease Re-discover Unowned devices\n");
    return;
  }

  device_handle_t *device = (device_handle_t *)oc_list_head(unowned_devices);
  device_handle_t *devices[MAX_NUM_DEVICES];
  int i = 0, c;

  PRINT("[C]\nUnowned Devices:\n");
  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    devices[i] = device;
    i++;
    device = device->next;
  }
  PRINT("[C]\n\nSelect device: ");
  SCANF("%d", &c);
  if (c < 0 || c >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);

  int ret = oc_obt_request_random_pin(&devices[c]->uuid, random_pin_cb, NULL);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to generate a random PIN\n");
  } else {
    PRINT("[C]\nERROR issuing request to generate random PIN\n");
  }

  otb_mutex_unlock(app_sync_lock);
}

	
void
py_request_random_pin(char* uuid)
{
  device_handle_t *device = (device_handle_t *)oc_list_head(unowned_devices);
  device_handle_t *devices[MAX_NUM_DEVICES];
  int i = 0, c=-1;

  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    devices[i] = device;
    if (strcmp(uuid, di) == 0) {
      c = i;
    }
    i++;
    device = device->next;
  }
  if (c == -1)
  {
    PRINT("[C] ERROR: Invalid uuid\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);

  int ret = oc_obt_request_random_pin(&devices[c]->uuid, random_pin_cb, NULL);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to generate a random PIN\n");
  } else {
    PRINT("[C]\nERROR issuing request to generate random PIN\n");
  }

  otb_mutex_unlock(app_sync_lock);
}

#ifdef OC_PKI
static void
otm_cert_cb(oc_uuid_t *uuid, int status, void *data)
{
  device_handle_t *device = (device_handle_t *)data;
  memcpy(device->uuid.id, uuid->id, 16);
  char di[37];
  oc_uuid_to_str(uuid, di, 37);

  if (status >= 0) {
    PRINT("[C]\nSuccessfully performed OTM on device %s\n", di);
    oc_list_add(owned_devices, device);
    inform_python(NULL,NULL,NULL);
  } else {
    PRINT("[C]\nERROR performing ownership transfer on device %s status %d\n", di,status);
    oc_memb_free(&device_handles, device);
  }
}

void
otm_cert(void)
{
  if (oc_list_length(unowned_devices) == 0) {
    PRINT("[C]\nPlease Re-discover Unowned devices\n");
    return;
  }

  device_handle_t *device = (device_handle_t *)oc_list_head(unowned_devices);
  device_handle_t *devices[MAX_NUM_DEVICES];
  int i = 0, c;

  PRINT("[C]\nUnowned Devices:\n");
  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    devices[i] = device;
    i++;
    device = device->next;
  }
  PRINT("[C]\n\nSelect device: ");
  SCANF("%d", &c);
  if (c < 0 || c >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);

  int ret = oc_obt_perform_cert_otm(&devices[c]->uuid, otm_cert_cb, devices[c]);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to perform ownership transfer\n");
    /* Having issued an OTM request, remove this item from the unowned device
     * list
     */
    oc_list_remove(unowned_devices, devices[c]);
  } else {
    PRINT("[C]\nERROR issuing request to perform ownership transfer\n");
  }

  otb_mutex_unlock(app_sync_lock);
}

void
py_otm_cert(char* uuid)
{
  PRINT("[C] CERT OTM\n");
  device_handle_t *device = (device_handle_t *)oc_list_head(unowned_devices);
  device_handle_t *devices[MAX_NUM_DEVICES];
  int i = 0, c=-1;

  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    devices[i] = device;
    if (strcmp(uuid, di) == 0) {
      c = i;
    }
    i++;
    device = device->next;
  }
  if (c == -1)
  {
    PRINT("[C] ERROR: Invalid uuid\n");
    return;
  }
  otb_mutex_lock(app_sync_lock);

  int ret = oc_obt_perform_cert_otm(&devices[c]->uuid, otm_cert_cb, devices[c]);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to perform ownership transfer\n");
    /* Having issued an OTM request, remove this item from the unowned device
     * list
     */
    oc_list_remove(unowned_devices, devices[c]);
  } else {
    PRINT("[C]\nERROR issuing request to perform ownership transfer\n");
  }

  otb_mutex_unlock(app_sync_lock);
}
#endif /* OC_PKI */

static void
otm_just_works_cb(oc_uuid_t *uuid, int status, void *data)
{
  device_handle_t *device = (device_handle_t *)data;
  memcpy(device->uuid.id, uuid->id, 16);
  char di[37];
  oc_uuid_to_str(uuid, di, 37);

  if (status >= 0) {
    PRINT("[C]\nSuccessfully performed OTM on device with UUID %s\n", di);
    oc_list_add(owned_devices, device);
    inform_python(NULL,NULL,NULL);
  } else {
    oc_memb_free(&device_handles, device);
    PRINT("[C]\nERROR performing ownership transfer on device %s\n", di);
  }
}

// function to list the unowned devices in iotivity (printed in C)
void py_list_unowned_devices(void)
{
  device_handle_t *device = (device_handle_t *)oc_list_head(unowned_devices);
  int i = 0;

  PRINT("[C] py_list_unowned_devices:\n");
  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C] [%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
}

// function to list the owned devices in iotivity (printed in C)
void py_list_owned_devices(void)
{
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0;

  PRINT("[C] py_list_owned_devices:\n");
  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C] [%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
}

void py_otm_just_works(char* uuid)
{
  device_handle_t *device = (device_handle_t *)oc_list_head(unowned_devices);
  device_handle_t *devices[MAX_NUM_DEVICES];
  int i = 0, c=-1;

  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    devices[i] = device;
    if (strcmp(uuid, di) == 0) {
      c = i;
    }
    i++;
    device = device->next;
  }
  if (c == -1)
  {
    PRINT("[C] ERROR: Invalid uuid\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);

  int ret = oc_obt_perform_just_works_otm(&devices[c]->uuid, otm_just_works_cb,
                                          devices[c]);
  if (ret >= 0) {
    PRINT("[C] Successfully issued request to perform ownership transfer\n");
    /* Having issued an OTM request, remove this item from the unowned device
     * list
     */
    oc_list_remove(unowned_devices, devices[c]);
    inform_python(NULL,NULL,NULL);
  } else {
    PRINT("[C] ERROR issuing request to perform ownership transfer\n");
  }

  otb_mutex_unlock(app_sync_lock);
}


void
otm_just_works(void)
{
  if (oc_list_length(unowned_devices) == 0) {
    PRINT("[C] Please Re-discover Unowned devices\n");
    return;
  }

  device_handle_t *device = (device_handle_t *)oc_list_head(unowned_devices);
  device_handle_t *devices[MAX_NUM_DEVICES];
  int i = 0, c;

  PRINT("[C]\nUnowned Devices:\n");
  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    devices[i] = device;
    i++;
    device = device->next;
  }
  PRINT("[C]\n\nSelect device: ");
  SCANF("%d", &c);
  if (c < 0 || c >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);

  int ret = oc_obt_perform_just_works_otm(&devices[c]->uuid, otm_just_works_cb,
                                          devices[c]);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to perform ownership transfer\n");
    /* Having issued an OTM request, remove this item from the unowned device
     * list
     */
    oc_list_remove(unowned_devices, devices[c]);
  } else {
    PRINT("[C]\nERROR issuing request to perform ownership transfer\n");
  }

  otb_mutex_unlock(app_sync_lock);
}

static void
retrieve_acl2_rsrc_cb(oc_sec_acl_t *acl, void *data)
{
  (void)data;
  if (acl) {
    PRINT("[C]\n/oic/sec/acl2:\n");
    oc_sec_ace_t *ac = oc_list_head(acl->subjects);
    PRINT("[C]\n################################################\n");
    while (ac) {
      PRINT("[C]aceid: %d\n", ac->aceid);
      if (ac->subject_type == OC_SUBJECT_UUID) {
        char uuid[37];
        oc_uuid_to_str(&ac->subject.uuid, uuid, 37);
        PRINT("[C]subject: %s\n", uuid);
      } else if (ac->subject_type == OC_SUBJECT_ROLE) {
        PRINT("[C]Roleid_role: %s\n", oc_string(ac->subject.role.role));
        if (oc_string_len(ac->subject.role.authority) > 0) {
          PRINT("[C]Roleid_authority: %s\n",
                oc_string(ac->subject.role.authority));
        }
      } else if (ac->subject_type == OC_SUBJECT_CONN) {
        PRINT("[C]connection type: ");
        if (ac->subject.conn == OC_CONN_AUTH_CRYPT) {
          PRINT("[C]auth-crypt\n");
        } else {
          PRINT("[C]anon-clear\n");
        }
      }
      PRINT("[C]Permissions: ");
      if (ac->permission & OC_PERM_CREATE) {
        PRINT("[C] C ");
      }
      if (ac->permission & OC_PERM_RETRIEVE) {
        PRINT("[C] R ");
      }
      if (ac->permission & OC_PERM_UPDATE) {
        PRINT("[C] U ");
      }
      if (ac->permission & OC_PERM_DELETE) {
        PRINT("[C] D ");
      }
      if (ac->permission & OC_PERM_NOTIFY) {
        PRINT("[C] N ");
      }
      PRINT("[C]\n");
      PRINT("[C]Resources: ");
      oc_ace_res_t *res = oc_list_head(ac->resources);
      while (res) {
        if (oc_string_len(res->href) > 0) {
          PRINT("[C] %s ", oc_string(res->href));
        } else if (res->wildcard != 0) {
          switch (res->wildcard) {
          case OC_ACE_WC_ALL:
            PRINT("[C] * ");
            break;
          case OC_ACE_WC_ALL_SECURED:
            PRINT("[C] + ");
            break;
          case OC_ACE_WC_ALL_PUBLIC:
            PRINT("[C] - ");
            break;
          default:
            break;
          }
        }
        res = res->next;
      }
      ac = ac->next;
      PRINT("[C]\n-----\n");
    }
    PRINT("[C]\n################################################\n");

    /* Freeing the ACL structure */
    oc_obt_free_acl(acl);
  } else {
    PRINT("[C]\nERROR RETRIEving /oic/sec/acl2\n");
  }
}

void
retrieve_acl2_rsrc(void)
{
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *devices[MAX_NUM_DEVICES];
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, c;

  PRINT("[C]\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
  PRINT("[C]\nSelect device: ");
  SCANF("%d", &c);
  if (c < 0 || c >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_retrieve_acl(&devices[c]->uuid, retrieve_acl2_rsrc_cb, NULL);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to RETRIEVE /oic/sec/acl2\n");
  } else {
    PRINT("[C]\nERROR issuing request to RETRIEVE /oic/sec/acl2\n");
  }
  otb_mutex_unlock(app_sync_lock);
}

void
display_cred_rsrc(oc_sec_creds_t *creds)
{
  if (creds) {
    PRINT("[C]\n/oic/sec/cred:\n");
    oc_sec_cred_t *cr = oc_list_head(creds->creds);
    PRINT("[C]\n################################################\n");
    while (cr) {
      char uuid[37];
      oc_uuid_to_str(&cr->subjectuuid, uuid, 37);
      PRINT("[C]credid: %d\n", cr->credid);
      PRINT("[C]subjectuuid: %s\n", uuid);
      PRINT("[C]credtype: %s\n", oc_cred_credtype_string(cr->credtype));
#ifdef OC_PKI
      PRINT("[C]credusage: %s\n", oc_cred_read_credusage(cr->credusage));
      if (oc_string_len(cr->publicdata.data) > 0) {
        PRINT("[C]publicdata_encoding: %s\n",
              oc_cred_read_encoding(cr->publicdata.encoding));
      }
#endif /* OC_PKI */
      PRINT("[C]privatedata_encoding: %s\n",
            oc_cred_read_encoding(cr->privatedata.encoding));
      if (oc_string_len(cr->role.role) > 0) {
        PRINT("[C]roleid_role: %s\n", oc_string(cr->role.role));
      }
      if (oc_string_len(cr->role.authority) > 0) {
        PRINT("[C]roleid_authority: %s\n", oc_string(cr->role.authority));
      }
      PRINT("[C]\n-----\n");
      cr = cr->next;
    }
    PRINT("[C]\n################################################\n");
  }
}

static void
retrieve_cred_rsrc_cb(oc_sec_creds_t *creds, void *data)
{
  (void)data;
  if (creds) {
    display_cred_rsrc(creds);
    /* Freeing the creds structure */
    oc_obt_free_creds(creds);
  } else {
    PRINT("[C]\nERROR RETRIEving /oic/sec/cred\n");
  }
}

void
retrieve_own_creds(void)
{
  otb_mutex_lock(app_sync_lock);
  /* The creds returned by oc_obt_retrieve_own_creds() point to
     internal data structures that store the security context of the OBT.
     DO NOT free them.
  */
  display_cred_rsrc(oc_obt_retrieve_own_creds());
  otb_mutex_unlock(app_sync_lock);
}

void
retrieve_cred_rsrc(void)
{
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *devices[MAX_NUM_DEVICES];
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, c;

  PRINT("[C]\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
  PRINT("[C]\nSelect device: ");
  SCANF("%d", &c);
  if (c < 0 || c >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);
  int ret =
    oc_obt_retrieve_creds(&devices[c]->uuid, retrieve_cred_rsrc_cb, NULL);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to RETRIEVE /oic/sec/cred\n");
  } else {
    PRINT("[C]\nERROR issuing request to RETRIEVE /oic/sec/cred\n");
  }
  otb_mutex_unlock(app_sync_lock);
}

static void
delete_ace_by_aceid_cb(int status, void *data)
{
  (void)data;
  if (status >= 0) {
    PRINT("[C]\nSuccessfully DELETEd ace\n");
  } else {
    PRINT("[C]\nERROR DELETing ace\n");
  }
}

void
delete_ace_by_aceid(void)
{
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *devices[MAX_NUM_DEVICES];
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, c;

  PRINT("[C]\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
  PRINT("[C]\nSelect device: ");
  SCANF("%d", &c);
  if (c < 0 || c >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  PRINT("[C]\nEnter aceid: ");
  int aceid;
  SCANF("%d", &aceid);

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_delete_ace_by_aceid(&devices[c]->uuid, aceid,
                                       delete_ace_by_aceid_cb, NULL);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to DELETE /oic/sec/acl2\n");
  } else {
    PRINT("[C]\nERROR issuing request to DELETE /oic/sec/acl2\n");
  }
  otb_mutex_unlock(app_sync_lock);
}

static void
delete_cred_by_credid_cb(int status, void *data)
{
  (void)data;
  if (status >= 0) {
    PRINT("[C]\nSuccessfully DELETEd cred\n");
  } else {
    PRINT("[C]\nERROR DELETing cred\n");
  }
}

void
delete_own_cred_by_credid(void)
{
  PRINT("[C]\nEnter credid: ");
  int credid;
  SCANF("%d", &credid);

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_delete_own_cred_by_credid(credid);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully DELETED cred\n");
  } else {
    PRINT("[C]\nERROR DELETing cred\n");
  }
  otb_mutex_unlock(app_sync_lock);
}

void
delete_cred_by_credid(void)
{
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *devices[MAX_NUM_DEVICES];
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, c;

  PRINT("[C]\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
  PRINT("[C]\nSelect device: ");
  SCANF("%d", &c);
  if (c < 0 || c >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  PRINT("[C]\nEnter credid: ");
  int credid;
  SCANF("%d", &credid);

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_delete_cred_by_credid(&devices[c]->uuid, credid,
                                         delete_cred_by_credid_cb, NULL);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to DELETE /oic/sec/cred\n");
  } else {
    PRINT("[C]\nERROR issuing request to DELETE /oic/sec/cred\n");
  }
  otb_mutex_unlock(app_sync_lock);
}

/**
* function to handle the reset
*
*/
static void
reset_device_cb(oc_uuid_t *uuid, int status, void *data)
{
  (void)data;
  char di[37];
  char* state = "";
  oc_uuid_to_str(uuid, di, 37);

  oc_memb_free(&device_handles, data);

  if (status >= 0) {
    PRINT("[C]\nSuccessfully performed hard RESET to device %s\n", di);
    state = "reset";
    inform_python(di,state,"reset");
  } else {
    PRINT("[C]\nERROR performing hard RESET to device %s\n", di);
  }
}

/**
* function to retrieve the # owned devices
**
*/
int py_get_nr_owned_devices(void)
{
   return (oc_list_length(owned_devices));
}

/**
* function to retrieve the uuid of the owned/unowned device
*
*/
char xx_di[OC_UUID_LEN];
char* get_uuid(int owned, int index)
{
  device_handle_t *device = NULL;
  if (owned == 1) {
    device = (device_handle_t *)oc_list_head(owned_devices);
  }
  else {
    device = (device_handle_t *)oc_list_head(unowned_devices);
  }

  int i = 0;
  while (device != NULL) {
    oc_uuid_to_str(&device->uuid, xx_di, OC_UUID_LEN);
    if (index == i)
    {
      return xx_di;
    }
    i++;
    device = device->next;
  }
  return " empty ";
}

/**
* function to retrieve the device name of the owned/unowned device
*
*/
char* get_device_name(int owned, int index)
{
  device_handle_t *device = NULL;
  if (owned == 1) {
    device = (device_handle_t *)oc_list_head(owned_devices);
  }
  else {
    device = (device_handle_t *)oc_list_head(unowned_devices);
  }

  int i = 0;
  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    if (index == i)
    {
      return device->device_name;
    }
    i++;
    device = device->next;
  }
  return " empty ";
}

/**
* function to retrieve the device name belonging to the uuid
*
*/
char* get_device_name_from_uuid(char* uuid)
{
  device_handle_t *device = NULL;
  device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0;
  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    if (strcmp(di, uuid) == 0)
    {
      return device->device_name;
    }
    i++;
    device = device->next;
  }

  device = (device_handle_t *)oc_list_head(unowned_devices);
  i = 0;
  while (device != NULL) {
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    if (strcmp(di, uuid) == 0)
    {
      return device->device_name;
    }
    i++;
    device = device->next;
  }
  return " empty ";
}

/**
* function to retrieve the number of unowned device
*
*/
int py_get_nr_unowned_devices(void)
{
   return (oc_list_length(unowned_devices));
}


/**
* function to reset the owned device 
*
*/
void py_reset_device(char* uuid)
{
  device_handle_t *device = py_getdevice_from_uuid(uuid, 1);

  if (device == NULL)
  {
    PRINT("[C]ERROR: Invalid uuid\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);
  int ret =
    oc_obt_device_hard_reset(&device->uuid, reset_device_cb, device);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to perform hard RESET\n");
    oc_list_remove(owned_devices, device);
  } else {
    PRINT("[C]\nERROR issuing request to perform hard RESET\n");
  }
  otb_mutex_unlock(app_sync_lock);
}

void
reset_device(void)
{
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *devices[MAX_NUM_DEVICES];
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, c;

  PRINT("[C]\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
  PRINT("[C]\nSelect device: ");
  SCANF("%d", &c);
  if (c < 0 || c >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);
  int ret =
    oc_obt_device_hard_reset(&devices[c]->uuid, reset_device_cb, devices[c]);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to perform hard RESET\n");
    oc_list_remove(owned_devices, devices[c]);
  } else {
    PRINT("[C]\nERROR issuing request to perform hard RESET\n");
  }
  otb_mutex_unlock(app_sync_lock);
}

#ifdef OC_PKI
static void
provision_id_cert_cb(int status, void *data)
{
  (void)data;
  if (status >= 0) {
    PRINT("[C]\nSuccessfully provisioned identity certificate\n");
  } else {
    PRINT("[C]\nERROR provisioning identity certificate\n");
  }
}

void
provision_id_cert(void)
{
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *devices[MAX_NUM_DEVICES];
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, c;

  PRINT("[C]\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
  PRINT("[C]\nSelect device: ");
  SCANF("%d", &c);
  if (c < 0 || c >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_provision_identity_certificate(&devices[c]->uuid,
                                                  provision_id_cert_cb, NULL);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to provision identity certificate\n");
  } else {
    PRINT("[C]\nERROR issuing request to provision identity certificate\n");
  }
  otb_mutex_unlock(app_sync_lock);
}

void
py_provision_id_cert(char* uuid)
{
  device_handle_t *device = py_getdevice_from_uuid(uuid, 1);

  if (device == NULL)
  {
    PRINT("[C]py_provision_id_cert ERROR: Invalid uuid\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_provision_identity_certificate(&device->uuid,
                                                  provision_id_cert_cb, NULL);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to provision identity certificate\n");
  } else {
    PRINT("[C]\nERROR issuing request to provision identity certificate\n");
  }
  otb_mutex_unlock(app_sync_lock);
}

static void
provision_role_cert_cb(int status, void *data)
{
  (void)data;
  if (status >= 0) {
    PRINT("[C]\nSuccessfully provisioned role certificate\n");
  } else {
    PRINT("[C]\nERROR provisioning role certificate\n");
  }
}


void
py_provision_role_cert(char* uuid, char* role, char* auth)
{
  device_handle_t *device = py_getdevice_from_uuid(uuid, 1);

  if (device == NULL)
  {
    PRINT("[C]py_provision_role_cert ERROR: Invalid uuid\n");
    return;
  }
  PRINT("[C]py_provision_role_cert: %s %s %s \n", uuid, role, auth);

  oc_role_t *roles = NULL;
  if (auth != NULL) {
    roles = oc_obt_add_roleid(roles, role, auth);
  }
  else {
    roles = oc_obt_add_roleid(roles, role, NULL);
  }

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_provision_role_certificate(roles, &device->uuid,
                                              provision_role_cert_cb, NULL);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to provision role certificate\n");
  } else {
    PRINT("[C]\nERROR issuing request to provision role certificate\n");
  }
  otb_mutex_unlock(app_sync_lock);
}


void
provision_role_cert(void)
{
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *devices[MAX_NUM_DEVICES];
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, c;

  PRINT("[C]\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
  PRINT("[C]\nSelect device: ");
  SCANF("%d", &c);
  if (c < 0 || c >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  oc_role_t *roles = NULL;
  do {
    char role[64];
    PRINT("[C]\nEnter role: ");
    SCANF("%63s", role);
    PRINT("[C]\nAuthority? [0-No, 1-Yes]: ");
    SCANF("%d", &i);
    if (i == 1) {
      char authority[64];
      PRINT("[C]\nEnter Authority: ");
      SCANF("%63s", authority);
      roles = oc_obt_add_roleid(roles, role, authority);
    } else {
      roles = oc_obt_add_roleid(roles, role, NULL);
    }
    PRINT("[C]\nMore Roles? [0-No, 1-Yes]: ");
    SCANF("%d", &i);
  } while (i == 1);

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_provision_role_certificate(roles, &devices[c]->uuid,
                                              provision_role_cert_cb, NULL);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to provision role certificate\n");
  } else {
    PRINT("[C]\nERROR issuing request to provision role certificate\n");
  }
  otb_mutex_unlock(app_sync_lock);
}

static void
provision_role_wildcard_ace_cb(oc_uuid_t *uuid, int status, void *data)
{
  (void)data;
  char di[37];
  oc_uuid_to_str(uuid, di, 37);

  if (status >= 0) {
    PRINT("[C]\nSuccessfully provisioned rold * ACE to device %s\n", di);
  } else {
    PRINT("[C]\nERROR provisioning ACE to device %s\n", di);
  }
}

void
provision_role_wildcard_ace(void)
{
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *devices[MAX_NUM_DEVICES];
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, dev;

  PRINT("[C]\nProvision role * ACE\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }

  if (i == 0) {
    PRINT("[C]\nNo devices to provision.. Please Re-Discover Owned devices.\n");
    return;
  }

  PRINT("[C]\n\nSelect device for provisioning: ");
  SCANF("%d", &dev);
  if (dev < 0 || dev >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  char role[64], authority[64];
  PRINT("[C]\nEnter role: ");
  SCANF("%63s", role);
  int d;
  PRINT("[C]\nAuthority? [0-No, 1-Yes]: ");
  SCANF("%d", &d);
  if (d == 1) {
    char authority[64];
    PRINT("[C]\nEnter Authority: ");
    SCANF("%63s", authority);
  }

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_provision_role_wildcard_ace(
    &devices[dev]->uuid, role, (d == 1) ? authority : NULL,
    provision_role_wildcard_ace_cb, NULL);
  otb_mutex_unlock(app_sync_lock);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to provision role * ACE\n");
  } else {
    PRINT("[C]\nERROR issuing request to provision role * ACE\n");
  }
}
#endif /* OC_PKI */

#ifdef OC_OSCORE
static void
provision_group_context_cb(oc_uuid_t *uuid, int status, void *data)
{
  (void)data;
  char di[37];
  oc_uuid_to_str(uuid, di, 37);

  if (status >= 0) {
    PRINT("[C]\nSuccessfully provisioned group OSCORE context to device %s\n", di);
  } else {
    PRINT("[C]\nERROR provisioning group OSCORE context to device %s\n", di);
  }
}

void
provision_server_group_oscore_context(void)
{
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *devices[MAX_NUM_DEVICES];
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, dev, subject;

  PRINT("[C]\nProvision server group OSCORE context\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }

  if (i == 0) {
    PRINT("[C]\nNo devices to provision.. Please Re-Discover Owned devices.\n");
    return;
  }

  PRINT("[C]\n\nSelect Server device for provisioning: ");
  SCANF("%d", &dev);
  if (dev < 0 || dev >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  PRINT("[C]\n\nSelect Client with secure multicast capability: ");
  SCANF("%d", &subject);
  if (subject < 0 || subject >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_provision_server_group_oscore_context(
    &devices[dev]->uuid, &devices[subject]->uuid, NULL,
    provision_group_context_cb, NULL);
  otb_mutex_unlock(app_sync_lock);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to provision server group OSCORE "
          "context\n");
  } else {
    PRINT("[C]\nERROR issuing request to provision server group OSCORE context\n");
  }
}

void
provision_client_group_oscore_context(void)
{
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *devices[MAX_NUM_DEVICES];
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, dev;

  PRINT("[C]\nProvision client group OSCORE context\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }

  if (i == 0) {
    PRINT("[C]\nNo devices to provision.. Please Re-Discover Owned devices.\n");
    return;
  }

  PRINT("[C]\n\nSelect device for provisioning: ");
  SCANF("%d", &dev);
  if (dev < 0 || dev >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_provision_client_group_oscore_context(
    &devices[dev]->uuid, NULL, provision_group_context_cb, NULL);
  otb_mutex_unlock(app_sync_lock);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to provision client group OSCORE "
          "context\n");
  } else {
    PRINT("[C]\nERROR issuing request to provision client group OSCORE context\n");
  }
}

static void
provision_oscore_contexts_cb(int status, void *data)
{
  (void)data;
  if (status >= 0) {
    PRINT("[C]\nSuccessfully provisioned pairwise OSCORE contexts\n");
  } else {
    PRINT("[C]\nERROR provisioning pairwise OSCORE contexts\n");
  }
}

void
provision_oscore_contexts(void)
{
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *devices[MAX_NUM_DEVICES];
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, c1, c2;

  PRINT("[C]\nProvision pairwise OSCORE contexts\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
  PRINT("[C]\nSelect device 1: ");
  SCANF("%d", &c1);
  if (c1 < 0 || c1 >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }
  PRINT("[C]Select device 2: ");
  SCANF("%d", &c2);
  if (c2 < 0 || c2 >= i || c2 == c1) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_provision_pairwise_oscore_contexts(
    &devices[c1]->uuid, &devices[c2]->uuid, provision_oscore_contexts_cb, NULL);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to provision OSCORE contexts\n");
  } else {
    PRINT("[C]\nERROR issuing request to provision OSCORE contexts\n");
  }
  otb_mutex_unlock(app_sync_lock);
}
#endif /* OC_OSCORE */

static void provision_credentials_cb(int status, void *data)
{
  (void)data;
  if (status >= 0) {
    PRINT("[C]\nSuccessfully provisioned pairwise credentials\n");
  } else {
    PRINT("[C]\nERROR provisioning pairwise credentials\n");
  }
}



void
py_provision_pairwise_credentials(char* uuid1, char* uuid2)
{
  PRINT("[C] Source %s, Target %s",uuid1,uuid2);
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *device1 = py_getdevice_from_uuid(uuid1, 1);
  device_handle_t *device2 = py_getdevice_from_uuid(uuid2, 1);
  if (device1 == NULL)
  {
    PRINT("[C]py_provision_role_cert ERROR: Invalid uuid1 %s \n",uuid1);
    return;
  }
  if (device2 == NULL)
 {
    PRINT("[C]py_provision_role_cert ERROR: Invalid uuid2 %s \n",uuid2);
    return;
  }

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_provision_pairwise_credentials(
    &device1->uuid, &device2->uuid, provision_credentials_cb, NULL);
  PRINT("[C]Provisioning Pariwise\n");
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to provision credentials\n");
  } else {
    PRINT("[C]\nERROR issuing request to provision credentials\n");
  }
  otb_mutex_unlock(app_sync_lock);
  
}

static void
provision_authcrypt_wildcard_ace_cb(oc_uuid_t *uuid, int status, void *data)
{
  (void)data;
  char di[37];
  oc_uuid_to_str(uuid, di, 37);

  if (status >= 0) {
    PRINT("[C]\nSuccessfully provisioned auth-crypt * ACE to device %s\n", di);
  } else {
    PRINT("[C]\nERROR provisioning ACE to device %s\n", di);
  }
}

void
provision_authcrypt_wildcard_ace(void)
{
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *devices[MAX_NUM_DEVICES];
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, dev;

  PRINT("[C]\nProvision auth-crypt * ACE\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }

  if (i == 0) {
    PRINT("[C]\nNo devices to provision.. Please Re-Discover Owned devices.\n");
    return;
  }

  PRINT("[C]\n\nSelect device for provisioning: ");
  SCANF("%d", &dev);
  if (dev < 0 || dev >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);
  int ret = oc_obt_provision_auth_wildcard_ace(
    &devices[dev]->uuid, provision_authcrypt_wildcard_ace_cb, NULL);
  otb_mutex_unlock(app_sync_lock);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to provision auth-crypt * ACE\n");
  } else {
    PRINT("[C]\nERROR issuing request to provision auth-crypt * ACE\n");
  }
}

static void
provision_ace2_cb(oc_uuid_t *uuid, int status, void *data)
{
  (void)data;
  char di[37];
  oc_uuid_to_str(uuid, di, 37);

  if (status >= 0) {
    PRINT("[C]\nSuccessfully provisioned ACE to device %s\n", di);
  } else {
    PRINT("[C]\nERROR provisioning ACE to device %s\n", di);
  }
}

void py_provision_ace2(char* target, char* subject, char* href, char* crudn ) 
{
  PRINT("[C] Provision ACE2: %s,%s,%s,%s\n",target,subject,href,crudn); 
  device_handle_t *device = py_getdevice_from_uuid(target, 1);
  device_handle_t *subject_device = py_getdevice_from_uuid(subject, 1);

    /*check if subject is OBT device*/	  
    oc_uuid_t *obt_uuid = oc_core_get_device_id(0);
    char di[OC_UUID_LEN];
    oc_uuid_to_str(obt_uuid, di, OC_UUID_LEN);
    if(strncmp(di,subject,OC_UUID_LEN)==0){
	subject_device = get_obt_device(obt_uuid,"OBT");
	if (subject_device == NULL){
		    PRINT("[C]py_provision_ace_access ERROR: Invalid OBT subject uuid\n");
		    return;
	  }
     }
  
  if (device == NULL){
    PRINT("[C]py_provision_ace_access ERROR: Invalid uuid\n");
    return;
  }
  if (subject_device == NULL){
	  
    PRINT("[C]py_provision_ace_access ERROR: Invalid subject uuid\n");
    return;
  }
  if (crudn[0] == '\0'){
    PRINT("[C]py_provision_ace_access ERROR: No CRUDN provided\n");
    return;
  }
  if (href[0] == '\0'){
    PRINT("[C]py_provision_ace_access ERROR: No resource href provided\n");
    return;
  }
  PRINT("[C] py_provision_ace: name = %s  href = %s",device->device_name,href,crudn);

  oc_sec_ace_t *ace = NULL;
  ace = oc_obt_new_ace_for_subject(&subject_device->uuid);

  oc_ace_res_t *res = oc_obt_ace_new_resource(ace);
  oc_obt_ace_resource_set_href(res, href);
  oc_obt_ace_resource_set_wc(res, OC_ACE_NO_WC);
  char* crudn_array = strtok(crudn,"|");

  while(crudn_array != NULL){
	PRINT("- %s\n", crudn_array);
	if (strcmp(crudn_array,"create") ==0){
	  	oc_obt_ace_add_permission(ace, OC_PERM_CREATE);
	}
	if (strcmp(crudn_array,"retrieve") ==0){
		oc_obt_ace_add_permission(ace, OC_PERM_RETRIEVE);
	}
	if (strcmp(crudn_array,"update") ==0){
		oc_obt_ace_add_permission(ace, OC_PERM_UPDATE);
	}
	if (strcmp(crudn_array,"delete") ==0){
	  	oc_obt_ace_add_permission(ace, OC_PERM_DELETE);
	}
	if (strcmp(crudn_array,"notify") ==0){
	  	oc_obt_ace_add_permission(ace, OC_PERM_NOTIFY);
	}
	crudn_array = strtok(NULL,"|");
  }


  otb_mutex_lock(app_sync_lock);
  int ret =
    oc_obt_provision_ace(&device->uuid, ace, provision_ace2_cb, NULL);
  otb_mutex_unlock(app_sync_lock);
  if (ret >= 0) {
    PRINT("[C] Successfully issued request to provision ACE\n");
  } else {
    PRINT("[C] ERROR issuing request to provision ACE\n");
  }
}

void py_provision_ace_cloud_access(char* uuid ) 
{
  
  device_handle_t *device = py_getdevice_from_uuid(uuid, 1);
  
  if (device == NULL){
    PRINT("[C]py_provision_ace_cloud_access ERROR: Invalid uuid\n");
    return;
  }
  PRINT("[C] py_provision_ace: name = %s ",device->device_name);

  oc_sec_ace_t *ace = NULL;
  ace = oc_obt_new_ace_for_connection(OC_CONN_AUTH_CRYPT);

  oc_ace_res_t *res = oc_obt_ace_new_resource(ace);
  oc_obt_ace_resource_set_href(res, "/CoapCloudConfResURI");
  oc_obt_ace_resource_set_wc(res, OC_ACE_NO_WC);

  oc_obt_ace_add_permission(ace, OC_PERM_RETRIEVE);
  oc_obt_ace_add_permission(ace, OC_PERM_UPDATE);

  otb_mutex_lock(app_sync_lock);
  int ret =
    oc_obt_provision_ace(&device->uuid, ace, provision_ace2_cb, NULL);
  otb_mutex_unlock(app_sync_lock);
  if (ret >= 0) {
    PRINT("[C] Successfully issued request to provision ACE\n");
  } else {
    PRINT("[C] ERROR issuing request to provision ACE\n");
  }
}

void
provision_ace2(void)
{
  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  const char *conn_types[2] = { "anon-clear", "auth-crypt" };
  int num_resources = 0;
  char di[OC_UUID_LEN];

  device_handle_t *devices[MAX_NUM_DEVICES];
  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, dev, sub;

  PRINT("[C]\nProvision ACL2\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }

  if (i == 0) {
    PRINT("[C]\nNo devices to provision.. Please Re-Discover Owned devices.\n");
    return;
  }

  PRINT("[C]\n\nSelect device for provisioning: ");
  SCANF("%d", &dev);
  if (dev < 0 || dev >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  PRINT("[C]\nSubjects:");
  device = (device_handle_t *)oc_list_head(owned_devices);
  PRINT("[C]\n[0]: %s\n", conn_types[0]);
  PRINT("[C][1]: %s\n", conn_types[1]);
  PRINT("[C][2]: Role\n");
  PRINT("[C][3]: Cloud\n");
  i = 0;
  while (device != NULL) {
    //char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i + 4, di, device->device_name);
    i++;
    device = device->next;

    if (!device) {
      oc_uuid_to_str(oc_core_get_device_id(0), di, OC_UUID_LEN);
      PRINT("[C][%d]: %s - (OBT)\n", i + 4, di);
      i++;
    }
  }
  PRINT("[C]\nSelect subject: ");
  SCANF("%d", &sub);

  if ((sub > (i + 3)) || (sub < 0)) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  oc_sec_ace_t *ace = NULL;
  if (sub == 0) {
    ace = oc_obt_new_ace_for_connection(OC_CONN_ANON_CLEAR);
  }
  else if (sub == 1) {
    ace = oc_obt_new_ace_for_connection(OC_CONN_AUTH_CRYPT);
  }
  else if (sub == 2) {
    char role[64];
    PRINT("[C]\nEnter role: ");
    SCANF("%63s", role);
    int d;
    PRINT("[C]\nAuthority? [0-No, 1-Yes]: ");
    SCANF("%d", &d);
    if (d == 1) {
      char authority[64];
      PRINT("[C]\nEnter Authority: ");
      SCANF("%63s", authority);
      ace = oc_obt_new_ace_for_role(role, authority);
    }
    else {
      ace = oc_obt_new_ace_for_role(role, NULL);
    }
  } else  {
    if (sub == 3 ) {   
      PRINT("[C]\nEnter Cloud sid: ");
      SCANF("%63s", di);
      oc_uuid_t uuid_di;
      oc_str_to_uuid(di, &uuid_di);
      ace = oc_obt_new_ace_for_subject(&uuid_di);
    } else if (sub == (i + 3)) {
      ace = oc_obt_new_ace_for_subject(oc_core_get_device_id(0));
    } else {
      ace = oc_obt_new_ace_for_subject(&devices[sub - 4]->uuid);
    }
  } 

  if (!ace) {
    PRINT("[C]\nERROR: Could not create ACE\n");
    return;
  }

  while (num_resources <= 0 || num_resources > MAX_NUM_RESOURCES) {
    if (num_resources != 0) {
      PRINT("[C]\n\nERROR: Enter valid number\n\n");
    }
    PRINT("[C]\nEnter number of resources in this ACE: ");
    SCANF("%d", &num_resources);
  }

  int c;
  PRINT("[C]\nResource properties\n");
  i = 0;
  while (i < num_resources) {
    oc_ace_res_t *res = oc_obt_ace_new_resource(ace);

    if (!res) {
      PRINT("[C]\nERROR: Could not allocate new resource for ACE\n");
      oc_obt_free_ace(ace);
      return;
    }

    PRINT("[C]Have resource href? [0-No, 1-Yes]: ");
    SCANF("%d", &c);
    if (c == 1) {
      PRINT("[C]Enter resource href (eg. /a/light): ");
      char href[64];
      SCANF("%63s", href);

      oc_obt_ace_resource_set_href(res, href);
      oc_obt_ace_resource_set_wc(res, OC_ACE_NO_WC);
    } else {
      PRINT("[C]\nSet wildcard resource? [0-No, 1-Yes]: ");
      SCANF("%d", &c);
      if (c == 1) {
        PRINT("[C][1]: All NCRs '*' \n"
              "[2]: All NCRs with >=1 secured endpoint '+'\n"
              "[3]: All NCRs with >=1 unsecured endpoint '-'\n"
              "\nSelect wildcard resource: ");
        SCANF("%d", &c);
        switch (c) {
        case 1:
          oc_obt_ace_resource_set_wc(res, OC_ACE_WC_ALL);
          break;
        case 2:
          oc_obt_ace_resource_set_wc(res, OC_ACE_WC_ALL_SECURED);
          break;
        case 3:
          oc_obt_ace_resource_set_wc(res, OC_ACE_WC_ALL_PUBLIC);
          break;
        default:
          break;
        }
      }
    }
    i++;
  }

  PRINT("[C]\nSet ACE2 permissions\n");
  PRINT("[C]CREATE [0-No, 1-Yes]: ");
  SCANF("%d", &c);
  if (c == 1) {
    oc_obt_ace_add_permission(ace, OC_PERM_CREATE);
  }
  PRINT("[C]RETRIEVE [0-No, 1-Yes]: ");
  SCANF("%d", &c);
  if (c == 1) {
    oc_obt_ace_add_permission(ace, OC_PERM_RETRIEVE);
  }
  PRINT("[C]UPDATE [0-No, 1-Yes]: ");
  SCANF("%d", &c);
  if (c == 1) {
    oc_obt_ace_add_permission(ace, OC_PERM_UPDATE);
  }
  PRINT("[C]DELETE [0-No, 1-Yes]: ");
  SCANF("%d", &c);
  if (c == 1) {
    oc_obt_ace_add_permission(ace, OC_PERM_DELETE);
  }
  PRINT("[C]NOTIFY [0-No, 1-Yes]: ");
  SCANF("%d", &c);
  if (c == 1) {
    oc_obt_ace_add_permission(ace, OC_PERM_NOTIFY);
  }

  otb_mutex_lock(app_sync_lock);
  int ret =
    oc_obt_provision_ace(&devices[dev]->uuid, ace, provision_ace2_cb, NULL);
  otb_mutex_unlock(app_sync_lock);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued request to provision ACE\n");
  } else {
    PRINT("[C]\nERROR issuing request to provision ACE\n");
  }
}

#if defined(OC_SECURITY) && defined(OC_PKI)
int
read_pem(const char *file_path, char *buffer, size_t *buffer_len)
{
  FILE *fp = fopen(file_path, "r");
  if (fp == NULL) {
    PRINT("[C]ERROR: unable to read PEM\n");
    return -1;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    PRINT("[C]ERROR: unable to read PEM\n");
    fclose(fp);
    return -1;
  }
  long pem_len = ftell(fp);
  if (pem_len < 0) {
    PRINT("[C]ERROR: could not obtain length of file\n");
    fclose(fp);
    return -1;
  }
  if (pem_len > (long)*buffer_len) {
    PRINT("[C]ERROR: buffer provided too small\n");
    fclose(fp);
    return -1;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    PRINT("[C]ERROR: unable to read PEM\n");
    fclose(fp);
    return -1;
  }
  if (fread(buffer, 1, pem_len, fp) < (size_t)pem_len) {
    PRINT("[C]ERROR: unable to read PEM\n");
    fclose(fp);
    return -1;
  }
  fclose(fp);
  buffer[pem_len] = '\0';
  *buffer_len = (size_t)pem_len;
  return 0;
}
#endif /* OC_SECURITY && OC_PKI */

#ifdef OC_PKI
void
install_trust_anchor(void)
{
  char cert[8192];
  size_t cert_len = 0;
  PRINT("[C]\nPaste certificate here, then hit <ENTER> and type \"done\": ");
  int c;
  while ((c = getchar()) == '\n' || c == '\r')
    ;
  for (; (cert_len < 4 ||
          (cert_len >= 4 && memcmp(&cert[cert_len - 4], "done", 4) != 0));
       c = getchar()) {
    if (c == EOF) {
      PRINT("[C]ERROR processing input.. aborting\n");
      return;
    }
    cert[cert_len] = (char)c;
    cert_len++;
  }

  while (cert[cert_len - 1] != '-' && cert_len > 1) {
    cert_len--;
  }
  cert[cert_len] = '\0';

  int rootca_credid =
    oc_pki_add_mfg_trust_anchor(0, (const unsigned char *)cert, strlen(cert));
  if (rootca_credid < 0) {
    PRINT("[C]ERROR installing root cert\n");
    return;
  }
}
#endif /* OC_PKI */

void
set_sd_info()
{
  char name[64] = { 0 };
  int priv = 0;
  PRINT("[C]\n\nEnter security domain name: ");
  SCANF("%63s", name);
  PRINT("[C]\n\nChoose security domain priv[0-No, 1-Yes]: ");
  SCANF("%d", &priv);
  oc_obt_set_sd_info(name, priv);
}

#ifdef OC_CLOUD

void
post_response_cloud_config(oc_client_response_t* data)
{
  PRINT("[C]post_response_cloud_config:\n");
  if (data->code == OC_STATUS_CHANGED)
    PRINT("[C]POST response: CHANGED\n");
  else if (data->code == OC_STATUS_CREATED)
    PRINT("[C]POST response: CREATED\n");
  else
    PRINT("[C]POST response code %d\n", data->code);

  if (data->payload != NULL) {
    print_rep(data->payload, false);
  }
}


void
set_cloud_info(void)
{
  char url[64] = "/CoapCloudConfResURI";  // url of the coap cloud config url
  char cis[64] = "coaps+tcp://127.0.0.1:5683";
  char at[64] = "test";
  char sid[64] = "00000000-0000-0000-0000-000000000001";
  char apn[64] = "plgd";
  char di[OC_UUID_LEN];
  oc_uuid_t device_uuid;

  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  int i = 0, c1;

  PRINT("[C]\nMy Devices:\n");
  while (device != NULL) {
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
  PRINT("[C]\nSelect device to configure: ");
  SCANF("%d", &c1);
  if (c1 < 0 || c1 >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  i = 0;
  device = (device_handle_t*)oc_list_head(owned_devices);
  while (device != NULL) {
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    oc_str_to_uuid(di, &device_uuid);
    if (c1 == i) {
      PRINT("[C]configuring: [%d]: %s - %s\n", i, di, device->device_name);
      break;
    }
    i++;
    device = device->next;
  }

  PRINT("[C]\nEnter url of cloudconfig resource (/CoapCloudConfResURI) : ");
  SCANF("%63s", url);
  PRINT("[C]\nPayload\n");
  PRINT("[C]\nEnter access token 'at' ('test') :");
  SCANF("%63s", at);
  PRINT("[C]\nEnter apn ('plgd'): ");
  SCANF("%63s", apn);
  PRINT("[C]\nEnter cis ('coaps+tcp://127.0.0.1:5684'):");
  SCANF("%63s", cis);
  PRINT("[C]\nEnter sid ('00000000-0000-0000-0000-000000000001'):");
  SCANF("%63s", sid);

  otb_mutex_lock(app_sync_lock);
 
    oc_obt_update_cloud_conf_device(&device_uuid, url,
      at, apn, cis, sid,
      post_response_cloud_config, NULL);
  
  otb_mutex_unlock(app_sync_lock);
}


void
get_cloud_info(void)
{
  char di[OC_UUID_LEN];
  oc_uuid_t device_uuid;
  char url[64] = "/CoapCloudConfResURI";  // url of the coap cloud config url

  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t* device = (device_handle_t*)oc_list_head(owned_devices);
  int i = 0, c1;

  PRINT("[C]\nMy Devices:\n");
  while (device != NULL) {
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
  PRINT("[C]\nSelect device to retrieve Cloud config from: ");
  SCANF("%d", &c1);
  if (c1 < 0 || c1 >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  i = 0;
  device = (device_handle_t*)oc_list_head(owned_devices);
  while (device != NULL) {
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    oc_str_to_uuid(di, &device_uuid);
    if (c1 == i) {
      PRINT("[C]retrieving: [%d]: %s - %s\n", i, di, device->device_name);
      break;
    }
    i++;
    device = device->next;
  }
  PRINT("[C]\nEnter url of cloudconfig resource (/CoapCloudConfResURI) : ");
  SCANF("%63s", url);

  PRINT("[C]\nretrieving data from %s :\n", url);

  otb_mutex_lock(app_sync_lock);
  oc_obt_retrieve_cloud_conf_device(&device_uuid, url,
    post_response_cloud_config, NULL);
  otb_mutex_unlock(app_sync_lock);
}


void trustanchorcb(int status, void* data)
{
  (void)data;
  if (status >= 0) {
    PRINT("[C]\nSuccessfully installed trust anchor for cloud\n");
  }
  else {
    PRINT("[C]\nERROR installing trust anchor %d\n", status);
  }
}


void
set_cloud_trust_anchor(void)
{
  char di[OC_UUID_LEN];
  oc_uuid_t device_uuid;
  char sid[64] = "00000000-0000-0000-0000-000000000001";

  if (oc_list_length(owned_devices) == 0) {
    PRINT("[C]\n\nPlease Re-Discover Owned devices\n");
    return;
  }

  device_handle_t* device = (device_handle_t*)oc_list_head(owned_devices);
  int i = 0, c1;

  PRINT("[C]\nMy Devices:\n");
  while (device != NULL) {
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
  PRINT("[C]\nSelect device to set cloud trust anchor: ");
  SCANF("%d", &c1);
  if (c1 < 0 || c1 >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  i = 0;
  device = (device_handle_t*)oc_list_head(owned_devices);
  while (device != NULL) {
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    oc_str_to_uuid(di, &device_uuid);
    if (c1 == i) {
      PRINT("[C]setting trust anchor on: [%d]: %s - %s\n", i, di, device->device_name);
      break;
    }
    i++;
    device = device->next;
  }

  PRINT("[C]\nEnter subject ('00000000-0000-0000-0000-000000000001'):");
  SCANF("%63s", sid);

  char cert[8192];
  size_t cert_len = 0;
  PRINT("[C]\nPaste certificate here, then hit <ENTER> and type \"done\": ");
  int c;
  while ((c = getchar()) == '\n' || c == '\r')
    ;
  for (; (cert_len < 4 ||
    (cert_len >= 4 && memcmp(&cert[cert_len - 4], "done", 4) != 0));
    c = getchar()) {
    if (c == EOF) {
      PRINT("[C]ERROR processing input.. aborting\n");
      return;
    }
    cert[cert_len] = (char)c;
    cert_len++;
  }

  while (cert[cert_len - 1] != '-' && cert_len > 1) {
    cert_len--;
  }
  cert[cert_len] = '\0';

  otb_mutex_lock(app_sync_lock);
  int retcode = oc_obt_provision_trust_anchor(cert, cert_len, sid, &device_uuid,
    trustanchorcb, NULL);
  PRINT("[C]sending message: %d\n", retcode);

  otb_mutex_unlock(app_sync_lock);

}


#endif /* OC_CLOUD */

void
factory_presets_cb(size_t device, void *data)
{
  (void)device;
  (void)data;
  oc_obt_shutdown();
  empty_device_list(owned_devices);
  empty_device_list(unowned_devices);
  oc_obt_init();
#if defined(OC_SECURITY) && defined(OC_PKI)
  char cert[8192];
  size_t cert_len = 8192;

  cert_len = 8192;
  if (read_pem("pki_certs/rootca1.pem", cert, &cert_len) < 0) {
    PRINT("[C]ERROR: unable to read certificates\n");
    return;
  }

  int rootca_credid =
    oc_pki_add_mfg_trust_anchor(0, (const unsigned char *)cert, cert_len);
  if (rootca_credid < 0) {
    PRINT("[C]ERROR installing root cert\n");
    return;
  }

  cert_len = 8192;
  if (read_pem("pki_certs/rootca2.pem", cert, &cert_len) < 0) {
    PRINT("[C]ERROR: unable to read certificates\n");
    return;
  }

  rootca_credid =
    oc_pki_add_mfg_trust_anchor(0, (const unsigned char *)cert, cert_len);
  if (rootca_credid < 0) {
    PRINT("[C]ERROR installing root cert\n");
    return;
  }
#endif /* OC_SECURITY && OC_PKI */
}

static oc_discovery_flags_t
resource_discovery(const char *anchor, const char *uri, oc_string_array_t types,
                   oc_interface_mask_t iface_mask, oc_endpoint_t *endpoint,
                   oc_resource_properties_t bm, bool more, void *user_data)
{
  (void)user_data;
  (void)iface_mask;
  (void)bm;
  (void)types;
  (void)endpoint;
  char strtypes[200] = " ";
  char strinterfaces[200]=" ";
  char json[1024]="";

  strcat(json, "{\"uri\" : \"");
  strcat(json,uri);
  strcat(json,"\",");

  strcat(json,"\"types\": [");
  int array_size = (int)oc_string_array_get_allocated_size(types);
  for (int i = 0; i < array_size; i++) {
    char *t = oc_string_array_get_item(types,i);
    strcat(strtypes,"\"");
    strcat(strtypes,t);
    strcat(strtypes,"\"");
    if (i < array_size-1) {
      strcat(strtypes,",");
    }
  }
  strcat(json, strtypes);
  strcat(json,"],");

  strcat(json,"\"if\": [");
  bool comma= false;

  //PRINT ("  %d", if)
  if ((iface_mask & OC_IF_BASELINE) == OC_IF_BASELINE) {
    strcat(strinterfaces,"\"oic.r.baseline\"");
    comma = true;
  }
  if ((iface_mask & OC_IF_RW) == OC_IF_RW) {
    if (comma) strcat(strinterfaces,",");
    strcat(strinterfaces,"\"oic.r.rw\"");
    comma = true;
  }
  if ((iface_mask & OC_IF_R) == OC_IF_R) {
    if (comma) strcat(strinterfaces,",");
    strcat(strinterfaces,"\"oic.r.r\"");
    comma = true;
  }
  if ((iface_mask & OC_IF_S) == OC_IF_S) {
    if (comma) strcat(strinterfaces,",");
    strcat(strinterfaces,"\"oic.r.s\"");
    comma = true;
  }
  if ((iface_mask & OC_IF_A) == OC_IF_A ) {
    if (comma) strcat(strinterfaces,",");
    strcat(strinterfaces,"\"oic.r.a\"");
    comma = true;
  }
  if ((iface_mask & OC_IF_CREATE) == OC_IF_CREATE ) {
    if (comma) strcat(strinterfaces,",");
    strcat(strinterfaces,"\"oic.r.create\"");
    comma = true;
  }
  if ((iface_mask & OC_IF_LL) == OC_IF_LL ) {
    if (comma) strcat(strinterfaces,",");
    strcat(strinterfaces,"\"oic.r.ll\"");
    comma = true;
  }
  if ((iface_mask & OC_IF_B) == OC_IF_B ) {
    if (comma) strcat(strinterfaces,",");
    strcat(strinterfaces,"\"oic.r.b\"");
    comma = true;
  }
  strcat(json, strinterfaces);
  strcat(json,"]");
  strcat(json,"}");

  PRINT("[C]anchor %s, uri : %s\n", anchor, uri);
  inform_resource_python(anchor, uri, strtypes, json);
  if (!more) {
    PRINT("[C]----End of discovery response---\n");
    uri = "";
    inform_resource_python(anchor, uri, strtypes, json);
    return OC_STOP_DISCOVERY;
  }
  return OC_CONTINUE_DISCOVERY;
}

void py_discover_resources(char* uuid) 
{
  device_handle_t *device = py_getdevice_from_uuid(uuid, 1);
  if (device == NULL) {
    device = py_getdevice_from_uuid(uuid, 0);
  }
  if (device == NULL) {
    PRINT("[C]py_discover_resources ERROR: Invalid uuid\n");
    return;
  }
  PRINT("[C] py_discover_resources: name = %s ",device->device_name);

  otb_mutex_lock(app_sync_lock);
  int ret =
    oc_obt_discover_all_resources(&device->uuid, resource_discovery, NULL);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued resource discovery request\n");
  } else {
    PRINT("[C]\nERROR issuing resource discovery request\n");
  }
  otb_mutex_unlock(app_sync_lock);

}


void
discover_resources(void)
{
  if (oc_list_length(unowned_devices) == 0 &&
      oc_list_length(owned_devices) == 0) {
    PRINT("[C]\nPlease Re-discover devices\n");
    return;
  }

  device_handle_t *devices[MAX_NUM_DEVICES];
  int i = 0, c;

  device_handle_t *device = (device_handle_t *)oc_list_head(owned_devices);
  PRINT("[C]\nMy Devices:\n");
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }
  PRINT("[C]\n\nUnowned Devices:\n");
  device = (device_handle_t *)oc_list_head(unowned_devices);
  while (device != NULL) {
    devices[i] = device;
    char di[OC_UUID_LEN];
    oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
    PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
    i++;
    device = device->next;
  }

  PRINT("[C]\nSelect device: ");
  SCANF("%d", &c);
  if (c < 0 || c >= i) {
    PRINT("[C]ERROR: Invalid selection\n");
    return;
  }

  otb_mutex_lock(app_sync_lock);
  int ret =
    oc_obt_discover_all_resources(&devices[c]->uuid, resource_discovery, NULL);
  if (ret >= 0) {
    PRINT("[C]\nSuccessfully issued resource discovery request\n");
  } else {
    PRINT("[C]\nERROR issuing resource discovery request\n");
  }
  otb_mutex_unlock(app_sync_lock);
}


void py_post(char* uri, int value){
  PRINT("[C] POST_light: %s-> %d\n",uri,value);
  //int uri_len = strlen(uri);
  
  //static oc_endpoint_t *light_server;
  /*
  if (oc_init_post(a_light, light_server, NULL, &post2_light, LOW_QOS, NULL)) {
    oc_rep_start_root_object();
    oc_rep_set_boolean(root, state, true);
    oc_rep_set_int(root, power, 55);
    oc_rep_end_root_object();
    if (oc_do_post())
      PRINT("Sent POST request\n");
    else
      PRINT("Could not send POST request\n");
  } else
    PRINT("Could not init POST request\n");
    */
}

void
display_device_uuid()
{
  char buffer[OC_UUID_LEN];
  oc_uuid_to_str(oc_core_get_device_id(0), buffer, sizeof(buffer));

  PRINT("[C] OBT Started device with ID: %s\n", buffer);
}

char*
py_get_obt_uuid()
{
  char buffer[OC_UUID_LEN];
  oc_uuid_to_str(oc_core_get_device_id(0), buffer, sizeof(buffer));

  char *uuid = malloc (sizeof (char) * OC_UUID_LEN);
  strncpy(uuid,buffer,OC_UUID_LEN);
  return uuid;
}

void test_print(void)
{
	PRINT("[C] test_print\n");
}

#ifdef OC_SO

static void
so_otm_cb(oc_uuid_t *uuid, int status, void *data)
{
  (void)data;
  char di[37];
  oc_uuid_to_str(uuid, di, OC_UUID_LEN);

  if (status >= 0) {
    PRINT("\nSuccessfully performed OTM on device with UUID %s\n", di);
    inform_diplomat_python("","","","so_otm:true",di,"");
    // oc_list_add(owned_devices, device);
  } else {
    // oc_memb_free(&device_handles, device);
    PRINT("\nERROR performing ownership transfer on device %s\n", di);
    inform_diplomat_python("","","","so_otm:false",di,"");
  }
}
static void
streamlined_onboarding_discovery_cb(oc_uuid_t *uuid, oc_endpoint_t *eps, void *data)
{
  (void)eps;
  char di[OC_UUID_LEN];
  oc_uuid_to_str(uuid, di, OC_UUID_LEN);
  PRINT("Discovered device with uuid %s\n", di);
  if (data == NULL) {
    return;
  }
  // TODO: This should first prompt for user confirmation before onboarding
  int count =0;
  struct timespec onboarding_wait = { .tv_sec = 5, .tv_nsec = 0 };
  int ret =0;
  while (ret <=0 && count < 7){
	  ret = oc_obt_perform_streamlined_otm(uuid, (const unsigned char *)data, strlen(data), so_otm_cb, NULL);
	  if (ret >= 0) {
	    PRINT("[C] Successfully issued request to perform Streamlined Onboarding OTM\n");
	    break;
	  }
        PRINT("[C] Waiting for station to associate\n");
  	nanosleep(&onboarding_wait, &onboarding_wait);
	count++;
  }
}
static void
perform_streamlined_discovery(oc_so_info_t *so_info)
{
  if (so_info != NULL) {
    char *cred = calloc(OC_SO_MAX_CRED_LEN, 1);
    PRINT("Onboarding device with UUID %s and cred %s\n", so_info->uuid, so_info->cred);
    memcpy(cred, so_info->cred, strlen(so_info->cred));
    PRINT("After Memcopy\n");

    struct timespec discovery_wait = { .tv_sec = 15, .tv_nsec = 0 };
    nanosleep(&discovery_wait, &discovery_wait);
    int count =0;
    struct timespec onboarding_wait = { .tv_sec = 5, .tv_nsec = 0 };
    int ret =0;
    device_handle_t *device = (device_handle_t *)oc_list_head(unowned_devices);
    int i = 0;

    while (device != NULL) {
      char di[OC_UUID_LEN];
      oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
      PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
      i++;
      device = device->next;
    }
    PRINT("[C] Trying discovery\n");
    while (device == NULL || count < 7){
	  oc_obt_discover_unowned_devices(streamlined_onboarding_discovery_cb, so_info->uuid, cred);
	  if (ret <= 0) {
	    PRINT("[C] Successfully discovered station device\n");
	    break;
	  }
	    while (device != NULL) {
	      char di[OC_UUID_LEN];
	      oc_uuid_to_str(&device->uuid, di, OC_UUID_LEN);
	      PRINT("[C][%d]: %s - %s\n", i, di, device->device_name);
	      i++;
	      device = device->next;
	    }
        PRINT("[C] Waiting for station to associate\n");
  	nanosleep(&onboarding_wait, &onboarding_wait);
	count++;
  }
    //so_info = so_info->next;
  }
  oc_so_info_free(so_info);
}
static void
observe_diplomat_cb(oc_client_response_t *data)
{
  PRINT("Observe Diplomat: CODE %d\n",data->code);
  if (data->code > 4) {
    PRINT("Observe GET failed with code %d\n", data->code);
    //char* c = (char *) data->code;
    char code[40];
    snprintf(code,sizeof(code),"observe_fail:%d",data->code);
    inform_diplomat_python("","","",code,"","");
    return;
  }
  oc_rep_t *rep = data->payload;
  oc_rep_t *so_info_rep_array = NULL;
  if(rep == NULL){
	char* error = "observe_fail:nopayload";
        inform_diplomat_python("","","",error,"","");
	return;
  }
  while (rep != NULL) {
    PRINT("key %s", oc_string(rep->name));
    switch (rep->type) {
    case OC_REP_OBJECT_ARRAY:
      if (oc_rep_get_object_array(rep, "soinfo", &so_info_rep_array)) {
        oc_so_info_t *so_info = oc_so_parse_rep_array(so_info_rep_array);
        PRINT("Onboarding device with UUID %s and cred %s\n", so_info->uuid, so_info->cred);
	char target_uuid[OC_UUID_LEN];
        snprintf(target_uuid,sizeof(target_uuid),"%s",so_info->uuid);
	inform_diplomat_python("","","","",target_uuid,"");
        perform_streamlined_discovery(so_info);
	break;
      }
      break;
    default:
      PRINT("NOT an OC_REP\n");
      break;
    }
    rep = rep->next;
  }
}

static oc_discovery_flags_t
diplomat_discovery(const char *anchor, const char *uri, oc_string_array_t types,
          oc_interface_mask_t iface_mask, oc_endpoint_t *endpoint,
          oc_resource_properties_t bm, void *user_data)
{
  PRINT("[C] Diplomat discovery requested\n");	
  (void)anchor;
  (void)iface_mask;
  (void)bm;
  (void)user_data;
  int uri_len = strlen(uri);
  uri_len = (uri_len >= MAX_URI_LENGTH) ? MAX_URI_LENGTH - 1 : uri_len;

  for (int i = 0; i < (int)oc_string_array_get_allocated_size(types); i++) {
    char *t = oc_string_array_get_item(types, i);
    if (strlen(t) == 14 && strncmp(t, "oic.r.diplomat", 14) == 0) {
      oc_endpoint_list_copy(&diplomat_ep, endpoint);
      strncpy(diplomat_uri, uri, uri_len);
      diplomat_uri[uri_len] = '\0';

    PRINT("Resource %s anchor: %s hosted at endpoints:\n", diplomat_uri,anchor);

    char di[OC_UUID_LEN];
    strncpy(di, anchor+6,OC_UUID_LEN);
    oc_uuid_t uuid;
    oc_str_to_uuid(di, &uuid);

    bool owned = oc_obt_is_owned_device(&uuid);
    char* state = "";
    if (owned){
	state="owned";
    }else{
	state="unowned";
    }

    inform_diplomat_python(anchor,diplomat_uri,state,NULL,NULL,NULL);

      oc_endpoint_t *ep = endpoint;
      while (ep != NULL) {
        PRINTipaddr(*ep);
        PRINT("\n");
        ep = ep->next;
      }
      oc_do_observe(diplomat_uri, diplomat_ep, NULL, &observe_diplomat_cb, HIGH_QOS, NULL);
      PRINT("[C] Sent OBSERVE request\n");
      return OC_STOP_DISCOVERY;
    }
  }
  return OC_CONTINUE_DISCOVERY;
}

void
discover_diplomat_for_observe(void)
{
  otb_mutex_lock(app_sync_lock);
  if (!oc_do_ip_discovery("oic.r.diplomat", &diplomat_discovery, NULL)) {
    PRINT("Failed to discover diplomat Devices\n");
  }
  otb_mutex_unlock(app_sync_lock);
}


void 
py_diplomat_set_observe(char* state)
{
   PRINT("[C] %s",state);
}


void
py_diplomat_stop_observe(char* uuid)
{
  (void)uuid;
  PRINT("Stopping OBSERVE\n");
  //oc_stop_observe(a_light, light_server);
}

void
py_discover_diplomat_for_observe(void)
{
  otb_mutex_lock(app_sync_lock);
  oc_do_ip_discovery("oic.r.diplomat", &diplomat_discovery, NULL); 
  otb_mutex_unlock(app_sync_lock);
}
#endif /* OC_SO */

#ifdef OC_CLIENT

static char a_light[MAX_URI_LENGTH];
static oc_endpoint_t *light_server;

static bool state;
static int power;
static oc_string_t name;
static bool discovered;


static void
post_light_response_cb(oc_client_response_t *data)
{
  if (data->code > OC_STATUS_CHANGED) {
    OC_ERR("POST returned unexpected response code %d\n", data->code);
  }
  //external_cb(&my_state);
  //my_state.error_state = false;
}
static void
get_light_cb(oc_client_response_t *data)
{
  PRINT("GET_light:\n");
  oc_rep_t *rep = data->payload;
  
  if (data->code > 4) {
    PRINT("GET failed with code %d\n", data->code);
    //char* c = (char *) data->code;
    char code[40];
    snprintf(code,sizeof(code),"observe_fail:%d",data->code);
    return;
  }
  while (rep != NULL) {
    PRINT("key %s, value ", oc_string(rep->name));
    switch (rep->type) {
    case OC_REP_BOOL:
      PRINT("%d\n", rep->value.boolean);
      state = rep->value.boolean;
      break;
    case OC_REP_INT:
      PRINT("%lld\n", rep->value.integer);
      power = (int)rep->value.integer;
      break;
    case OC_REP_STRING:
      PRINT("%s\n", oc_string(rep->value.string));
      oc_free_string(&name);
    //  oc_new_string(&name, oc_string(rep->value.string), oc_string_len(rep->value.string));
      break;
    default:
      break;
    }
    rep = rep->next;
  }
}
static oc_discovery_flags_t
discovery_cb(const char *anchor, const char *uri, oc_string_array_t types,
          oc_interface_mask_t iface_mask, oc_endpoint_t *endpoint,
          oc_resource_properties_t bm, void *user_data)
{
  (void)anchor;
  (void)user_data;
  (void)iface_mask;
  (void)bm;
  int i;
  int uri_len = strlen(uri);
  uri_len = (uri_len >= MAX_URI_LENGTH) ? MAX_URI_LENGTH - 1 : uri_len;
  for (i = 0; i < (int)oc_string_array_get_allocated_size(types); i++) {
    char *t = oc_string_array_get_item(types, i);
    if (strlen(t) == 10 && strncmp(t, "core.light", 10) == 0) {
      oc_endpoint_list_copy(&light_server, endpoint);
      strncpy(a_light, uri, uri_len);
      a_light[uri_len] = '\0';

      PRINT("Resource %s hosted at endpoints:\n", a_light);
      discovered = true;
      oc_endpoint_t *ep = endpoint;
      while (ep != NULL) {
        PRINTipaddr(*ep);
        PRINT("\n");
        ep = ep->next;
      }


      return OC_STOP_DISCOVERY;
    }
  }
  return OC_CONTINUE_DISCOVERY;
}


static oc_discovery_flags_t
doxm_discovery_cb(const char *anchor, const char *uri, oc_string_array_t types,
          oc_interface_mask_t iface_mask, oc_endpoint_t *endpoint,
          oc_resource_properties_t bm, void *user_data)
{

  (void)anchor;
  (void)iface_mask;
  (void)bm;
  (void)user_data;
  (void)types;
  (void)endpoint;
  int uri_len = strlen(uri);
  uri_len = (uri_len >= MAX_URI_LENGTH) ? MAX_URI_LENGTH - 1 : uri_len;
  PRINT("DOXM CB\n");
      oc_endpoint_t *ep = endpoint;
      while (ep != NULL) {
        PRINTipaddr(*ep);
        PRINT("\n");
        ep = ep->next;
      }
  /*
  if (oc_rep_get_int_array(data->payload, "oxms", &oxms, &oxms_len)) {
    size_t i;
    for (i = 0; i < oxms_len; i++) {
     PRINT("[C] %d \n",oxms[i]);
    }
  }
  */
  return OC_STOP_DISCOVERY;
}



void 
discover_doxm( void )
{
  otb_mutex_lock(app_sync_lock);
  if (!oc_do_ip_discovery("oic.r.doxm", &doxm_discovery_cb, NULL)) {
    PRINT("Failed to discover DOXM\n");
  }
  otb_mutex_unlock(app_sync_lock);

  /*
  PRINT("[C] Discover Doxm %s\n",uuid);	
  if (oc_do_get("/oic/sec/doxm", ep, NULL, &doxm_discovery_cb, HIGH_QOS, NULL)) {
	  PRINT("[C] doxm return\n");
  }
  */
}

void
discover_resource(char *rt, char* uuid)
{
  PRINT("[C] rt:%s uuid:%s\n",rt,uuid);
  oc_do_ip_discovery(rt, &discovery_cb, NULL);
  oc_do_get(a_light, light_server, NULL, &get_light_cb, LOW_QOS, NULL);
  PRINT("[C] rt:%s uuid:%s\n",rt,uuid);
}

void
change_light(int value)
{
  PRINT("[C] POST_light: %d\n",value);
  //(void)value;
  bool light_cmd;
  if (value==1){
	  light_cmd=true;
  }else{
	  light_cmd=false;
  }
  /*
  PRINT("SETTING LIGHT\n");
    otb_mutex_lock(app_sync_lock);
	if (!oc_do_ip_discovery("core.light", &discovery_cb, NULL)) {
	PRINT("Failed to discover Devices\n");
	}
	else{
	  PRINT("Discovered device\n");
	}
	if(!discovered){
		return;
	}
    otb_mutex_unlock(app_sync_lock);
    */
  if (oc_init_post(a_light, light_server, NULL, &post_light_response_cb, LOW_QOS, NULL)) {
    oc_rep_start_root_object();
    oc_rep_set_boolean(root, state, light_cmd);
    //oc_rep_set_int(root, power, 55);
    oc_rep_end_root_object();
    if (oc_do_post())
      PRINT("Sent POST request\n");
    else
      PRINT("Could not send POST request\n");
 } else{
    PRINT("Could not init POST request\n");
 }
}
#endif /*OC Client*/

int
python_main(void)
{
#if defined(_WIN32)
  InitializeCriticalSection(&cs);
  InitializeConditionVariable(&cv);
  InitializeCriticalSection(&app_sync_lock);
#elif defined(__linux__)
  struct sigaction sa;
  sigfillset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = python_exit;
  sigaction(SIGINT, &sa, NULL);
#endif

#ifdef OC_SERVER
     PRINT("[C]OC_SERVER\n");
#endif
#ifdef OC_CLIENT
      PRINT("[C]OC_CLIENT\n");
#endif

  int init;

  static const oc_handler_t handler = { .init = app_init,
                                        .signal_event_loop = signal_event_loop,
#ifdef OC_SERVER
                                        .register_resources = NULL, 
#endif
#ifdef OC_CLIENT
                                        .requests_entry = issue_requests
#endif
};

#ifdef OC_STORAGE
  oc_storage_config("./onboarding_tool_creds");
#endif /* OC_STORAGE */
  oc_set_factory_presets_cb(factory_presets_cb, NULL);
  oc_set_con_res_announced(false);
  oc_set_max_app_data_size(16384);  
  
  init = oc_main_init(&handler);
  if (init < 0)
    return init;

#if defined(_WIN32)
  event_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ocf_event_thread,
                              NULL, 0, NULL);
  if (NULL == event_thread) {
    return -1;
  }
#elif defined(__linux__)
  if (pthread_create(&event_thread, NULL, &ocf_event_thread, NULL) != 0) {
    return -1;
  }
#endif


  display_device_uuid();


  while (quit != 1) {
    sleep(5);
  }  

#if defined(_WIN32)
  WaitForSingleObject(event_thread, INFINITE);
#elif defined(__linux__)
  pthread_join(event_thread, NULL);
#endif

  /* Free all device_handle_t objects allocated by this application */
  device_handle_t *device = (device_handle_t *)oc_list_pop(owned_devices);
  while (device) {
    oc_memb_free(&device_handles, device);
    device = (device_handle_t *)oc_list_pop(owned_devices);
  }
  device = (device_handle_t *)oc_list_pop(unowned_devices);
  while (device) {
    oc_memb_free(&device_handles, device);
    device = (device_handle_t *)oc_list_pop(unowned_devices);
  }

  return 0;
}
