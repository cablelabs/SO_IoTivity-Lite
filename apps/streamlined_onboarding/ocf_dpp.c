#include <stdio.h>
#include <wpa_ctrl.h>
#include <errno.h>
#include "oc_api.h"
#include "oc_core_res.h"
#include "oc_streamlined_onboarding.h"
#include <sys/stat.h>

static struct wpa_ctrl *ctrl = NULL;
extern oc_so_info_t self_so_info;

static oc_so_info_t *
parse_wpa_event(char *event_buf)
{
  char *pos = strstr(event_buf, DPP_EVENT_OCF_SO_INFO_RECEIVED);
  if (pos == NULL) {
    return NULL;
  }
  oc_so_info_t *new_info = malloc(sizeof(oc_so_info_t));
  new_info->next = NULL;
  strncpy(new_info->uuid, pos + 16, OC_UUID_LEN - 1);
  new_info->uuid[OC_UUID_LEN - 1] = '\0';

  pos = strstr(pos + 16, " ");
  if (pos == NULL || strlen(pos + 1) > OC_UUID_LEN - 1) {
    OC_ERR("Failed to parse credential from wpa message");
    return NULL;
  }
  strncpy(new_info->cred, pos + 1, OC_SO_MAX_CRED_LEN - 1);
  new_info->cred[OC_SO_MAX_CRED_LEN - 1] = '\0';
  OC_DBG("Parsed UUID: %s and cred: %s\n", new_info->uuid, new_info->cred);
  return new_info;
}

oc_so_info_t *
dpp_so_info_poll(void)
{
  if (ctrl == NULL) {
    return NULL;
  }

  oc_so_info_t *new_info_head = NULL, *cur, *temp;

  char event_buf[4096];
  size_t len = -1;
  fd_set rfds;
  int fd, res;

  fd = wpa_ctrl_get_fd(ctrl);

  FD_ZERO(&rfds);
  FD_SET(fd, &rfds);
  res = select(fd + 1, &rfds, NULL, NULL, NULL);
  if (res < 0 && errno != EINTR) {
    OC_ERR("Select failed");
    return NULL;
  }

  if (!FD_ISSET(fd, &rfds)) {
    OC_WRN("wpa_ctrl socket failed to become ready");
    return NULL;
  }

  while (wpa_ctrl_pending(ctrl)) {
    len = sizeof(event_buf);
    wpa_ctrl_recv(ctrl, event_buf, &len);
    event_buf[len] = '\0';
    OC_DBG("Received event from wpa_ctrl: %s\n", event_buf);
    temp = parse_wpa_event(event_buf);
    if (temp == NULL) {
      continue;
    }
    if (new_info_head == NULL) {
      new_info_head = temp;
      cur = new_info_head;
      continue;
    }
    cur->next = temp;
    cur = cur->next;
  }
  return new_info_head;
}

int
dpp_send_so_info(void)
{
  if (ctrl == NULL) {
    return -1;
  }

  char wpa_command[29 + strlen(self_so_info.uuid) + strlen(self_so_info.cred)];
  sprintf(wpa_command, "DPP_OCF_INFO_ADD uuid=%s cred=%s", self_so_info.uuid, self_so_info.cred);
  OC_DBG("wpa_ctrl command to send: %s\n", wpa_command);

  size_t reply_len = 128;
  char reply_buf[reply_len];
  int ret = wpa_ctrl_request(ctrl, wpa_command, strlen(wpa_command), reply_buf, &reply_len, NULL);
  if (ret == -2) {
    OC_ERR("'%s' command timed out.", wpa_command);
    return -2;
  } else if (ret < 0) {
    OC_ERR("'%s' command failed.", wpa_command);
    return -1;
  }
  OC_DBG("wpa_ctrl response: %s", reply_buf);
  return 0;
}

void
dpp_so_named_pipe_init(char *named_pipe)
{
 // Create the named pipe
    int res = mkfifo(named_pipe, 0666);
    if (res == -1) {
        OC_WRN("Failed to create named pipe; already exists?");
    }
}


int
dpp_so_init(char *ctrl_iface)
{
  if (ctrl_iface == NULL) {
    return -1;
  }
  ctrl = wpa_ctrl_open(ctrl_iface);
  if (ctrl == NULL) {
    OC_ERR("Failed to open wpa_ctrl interface");
    return -1;
  }
  OC_DBG("Opened wpa_ctrl interface");
  return wpa_ctrl_attach(ctrl);
}

int
dpp_so_teardown(void)
{
  if (ctrl != NULL) {
    wpa_ctrl_detach(ctrl);
    wpa_ctrl_close(ctrl);
  }
  return 0;
}
