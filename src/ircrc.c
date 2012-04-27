#define _GNU_SOURCE
#include <stdlib.h>
#include <time.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_network.h>
#include <vlc_playlist.h>

#define MODULE_STRING "irc"
#define MAX_LINE 8096
#define SEND_BUFFER_LEN 8096

struct intf_sys_t
{
  int fd;
  int timeout;
  
  input_thread_t *input;
  vlc_thread_t irc_thread;

  char *line;
  int line_loc;

  char *send_buffer;
  int send_buffer_len;
  int send_buffer_loc;
  int send_buffer_sent;

  playlist_t *playlist;

  char *server, *channel, *nick;
};

struct irc_msg_t {
  char *prefix;
  char *command;
  char *params;
  char *trailing;
};

static int Open(vlc_object_t *);
static void Close(vlc_object_t *);
void EventLoop(int, void *);
int HandleRead(void *);
int HandleWrite(void *);
void LineReceived(void *, char *);
void irc_PING(void *, struct irc_msg_t *);
void irc_PRIVMSG(void *handle, struct irc_msg_t *irc_msg);
void SendBufferAppend(void *, char *);
void ResizeSendBuffer(void *, int);
struct irc_msg_t *ParseIRC(char *);
static void Run(intf_thread_t *intf);

vlc_module_begin()
    set_shortname("IRC")
    set_description("IRC interface")
    set_capability("interface", 0)
    set_callbacks(Open, Close)
    set_category(CAT_INTERFACE)
    add_string("server", NULL, "server", "IRC server", true)
    add_string("channel", NULL, "channel", "IRC channel", true)
    add_string("nick", NULL, "nick", "IRC nickname", true)
vlc_module_end ()

static int Open(vlc_object_t *obj)
{
    intf_thread_t *intf = (intf_thread_t *)obj;

    intf_sys_t *sys = malloc(sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;
    intf->p_sys = sys;

    sys->server = var_InheritString(intf, "server");
    sys->channel = var_InheritString(intf, "channel");
    sys->nick = var_InheritString(intf, "nick");

    if(sys->server == NULL) {
      msg_Err(intf, "No server specified, use --server");
      return VLC_SUCCESS;
    }
    
    if(sys->channel == NULL) {
      msg_Err(intf, "No channel specified, use --channel");
      return VLC_SUCCESS;
    }

    if(sys->nick == NULL) {
      msg_Err(intf, "No nickname specified, use --nick");
      return VLC_SUCCESS;
    }

    intf->pf_run = Run;

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    intf_thread_t *intf = (intf_thread_t *)obj;
    intf_sys_t *sys = intf->p_sys;

    msg_Dbg(intf, "Unloading IRC interface");

    free(sys);
}

static void Run(intf_thread_t *intf)
{
  intf_sys_t *sys = intf->p_sys;
  int fd;

  msg_Info(intf, "Creating IRC connection...");

  fd = net_ConnectTCP(VLC_OBJECT(intf), sys->server, 6667);

  if(fd == -1) {
    msg_Err(intf, "Error connecting to server");
    return;
  }

  msg_Info(intf, "Connected to server");
    
  /* initialize context */
  sys->fd = fd;
  sys->send_buffer_len = SEND_BUFFER_LEN;
  sys->send_buffer = (char *)malloc(SEND_BUFFER_LEN * sizeof(char));
  sys->send_buffer_loc = 0;
  sys->send_buffer_sent = 0;
  sys->line_loc = 0;

  SendBufferAppend(intf, "NICK ");
  SendBufferAppend(intf, sys->nick);
  SendBufferAppend(intf, "\r\n");
  
  SendBufferAppend(intf, "USER ");
  SendBufferAppend(intf, sys->nick);
  SendBufferAppend(intf, " 8 * vlc\r\n");

  sys->playlist = pl_Get(intf);

  EventLoop(fd, intf);

  free(sys->send_buffer);
  free(sys);
}

void EventLoop(int fd, void *handle)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;

  sys->line = (char *)malloc(MAX_LINE * sizeof(char));

  while(1) {
    struct pollfd ufd = { .fd = fd, .events = POLLIN | POLLOUT, };
    
    if(poll(&ufd, 1, 1000) <= 0) /* block for 1s so we don't spin */
      continue;
    
    if(ufd.revents & POLLIN) {
      int rv = HandleRead(handle);
      if(rv != 0) {
	msg_Err(intf, "Read error: %s", strerror(rv));
	break;
      }
    } else if(ufd.revents & POLLOUT) {
      int rv = HandleWrite(handle);
      if(rv != 0) {
	msg_Err(intf, "Write error: %s", strerror(rv));
	break;
      }
    }
  }
}

int HandleRead(void *handle) {
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;

  static char ch, pch;

  if(recv(sys->fd, &ch, 1, 0) == -1)
    return errno;
  
  if(pch == '\r' && ch == '\n') { /* were the last two characters \r\n? */
    sys->line[sys->line_loc-1] = '\0'; /* overwrite CR with a nullbyte */
    LineReceived(handle, sys->line);
    sys->line_loc = 0;
    sys->line = (char *)malloc(MAX_LINE * sizeof(char)); /* allocate a new line, lineReceived will free the old one */
  } else {
    sys->line[sys->line_loc] = ch;
    pch = ch;
    sys->line_loc++;
  }

  return 0;
}

int HandleWrite(void *handle)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;

  int send_len = sys->send_buffer_loc-sys->send_buffer_sent;

  if(send_len == 0)
    return 0;

  int sent = send(sys->fd, sys->send_buffer+sys->send_buffer_sent, send_len, 0);

  if(sent == -1)
    return errno;

  if(sys->send_buffer_sent+sent == sys->send_buffer_len) { /* full buffer is sent */
    sys->send_buffer_loc = 0;
    ResizeSendBuffer(handle, SEND_BUFFER_LEN);
  } else {
    sys->send_buffer_sent += sent;
  }

  return 0;
}

void LineReceived(void *handle, char *line)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;

  msg_Dbg(intf, "Line received: %s", line);

  struct irc_msg_t *irc_msg = ParseIRC(line);

  if(irc_msg == NULL) {
    msg_Dbg(intf, "Malformed IRC message: %s", line);
    goto line_error;
  }
    
  if(strcmp(irc_msg->command, "376") == 0) { /* End of MotD */
    SendBufferAppend(handle, "JOIN ");
    SendBufferAppend(handle, sys->channel);
    SendBufferAppend(handle, "\r\n");
  } else if(strcmp(irc_msg->command, "PING") == 0) {
    irc_PING(handle, irc_msg);
  } else if(strcmp(irc_msg->command, "PRIVMSG") == 0) {
    irc_PRIVMSG(handle, irc_msg);
  }

 line_error:
  free(line);
  return;
}

void irc_PING(void *handle, struct irc_msg_t *irc_msg)
{
  char pong[512];

  strncat(pong, "PONG ", 512 * sizeof(char));
  strncat(pong, irc_msg->trailing, 512 * sizeof(char));

  SendBufferAppend(handle, pong);
}

void irc_PRIVMSG(void *handle, struct irc_msg_t *irc_msg)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;

  char *msg = irc_msg->trailing;

  /* TODO: refactor with callbacks i.e. var_Set etc */
  input_thread_t *p_input = playlist_CurrentInput(sys->playlist);

  if(strcmp(msg, ">pause") == 0) {
    msg_Info(intf, "Pause");
    
    if(p_input && var_GetInteger(p_input, "state") == PLAYING_S)
      playlist_Pause(sys->playlist);

    if( p_input )
      vlc_object_release( p_input );
  } else if(strcmp(msg, ">play") == 0) {
    msg_Info(intf, "Play");
    input_thread_t *p_input =  playlist_CurrentInput(sys->playlist);

    if(!p_input || var_GetInteger( p_input, "state" ) != PLAYING_S)
      playlist_Play(sys->playlist);

    if(p_input)
      vlc_object_release( p_input );
  }
}

void SendBufferAppend(void *handle, char *string)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;

  msg_Dbg(intf, "Sending %s", string);

  int new_len = sys->send_buffer_loc + strlen(string);
  if(sys->send_buffer_len < new_len) {
    ResizeSendBuffer(handle, new_len);
  }

  memcpy(sys->send_buffer+sys->send_buffer_loc, string, strlen(string) * sizeof(char));
  sys->send_buffer_loc += strlen(string);
}

void ResizeSendBuffer(void *handle, int len)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;
  
  sys->send_buffer_len = len;
  sys->send_buffer = (char *)realloc(sys->send_buffer, len * sizeof(char));  
}

struct irc_msg_t *ParseIRC(char *line)
{
  struct irc_msg_t *irc_msg = (struct irc_msg_t *)malloc(sizeof(struct irc_msg_t));
  char *ch, *sv_ptr;
  int started = 0;

  irc_msg->prefix = irc_msg->command = irc_msg->params = irc_msg->trailing = NULL;

  if(line[0] == ':') { /* check for prefix */
    *line = line[1];
    ch = strtok_r(line, " ", &sv_ptr); /* grab prefix */
    started = 1;
    if(ch != NULL)
      irc_msg->prefix = strdup(ch); 
    else
      return NULL; /* malformed IRC message */
  }

  if(started)
    ch = strtok_r(NULL, " ", &sv_ptr); /* grab command */
  else
    ch = strtok_r(line, " ", &sv_ptr);

  irc_msg->command = strdup(ch);

  ch = strtok_r(NULL, ":", &sv_ptr); /* check for trailing */
  if(ch != NULL) {
    irc_msg->params = strdup(ch); /* grab params */
    ch = strtok_r(NULL, ":", &sv_ptr);
    if(ch != NULL)
      irc_msg->trailing = strdup(ch); /* gram trailing */
  } else {
    ch = strtok_r(NULL, "", &sv_ptr); 
    irc_msg->params = strdup(ch); /* grab params */
  }

  return irc_msg;
}
