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
void ResizeSendBuffer(void *);
struct irc_msg_t *ParseIRC(char *);
static void Run(intf_thread_t *intf);
static int Playlist(vlc_object_t *, char const *, vlc_value_t, vlc_value_t, void *);
static void RegisterCallbacks(intf_thread_t *);
void FreeIRCMsg(struct irc_msg_t *irc_msg);

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

  int canc = vlc_savecancel();

  RegisterCallbacks(intf);

  while(1) {
    msg_Info(intf, "Creating IRC connection...");

    fd = net_ConnectTCP(VLC_OBJECT(intf), sys->server, 6667);

    if(fd == -1) {
      msg_Err(intf, "Error connecting to server");
      return;
    }

    msg_Info(intf, "Connected to server");

    /* initialize context */
    sys->fd = fd;
    sys->send_buffer_len = 0;
    sys->send_buffer = (char *)malloc(SEND_BUFFER_LEN * sizeof(char));
    sys->send_buffer_loc = 0;
    sys->line_loc = 0;

    SendBufferAppend(intf, "NICK ");
    SendBufferAppend(intf, sys->nick);
    SendBufferAppend(intf, "\r\n");
  
    SendBufferAppend(intf, "USER ");
    SendBufferAppend(intf, sys->nick);
    SendBufferAppend(intf, " 8 * vlc\r\n");

    sys->playlist = pl_Get(intf);

    #ifdef STOP_HACK
    playlist_Pause(sys->playlist);
    input_thread_t * input = playlist_CurrentInput(sys->playlist);
    var_SetFloat(input, "position", 0.0);
    #endif

    EventLoop(fd, intf);

    free(sys->send_buffer);

    sleep(30);
  }

  free(sys);

  vlc_restorecancel(canc);
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
  
  int sent = send(sys->fd, sys->send_buffer, sys->send_buffer_len, 0);

  if(sent == -1)
    return errno;

  if(sent == 0)
    return 0;

  sys->send_buffer_len -= sent;
  sys->send_buffer_loc -= sent;

  memcpy(sys->send_buffer, sys->send_buffer+sent, sys->send_buffer_len);
  ResizeSendBuffer(handle);

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
  FreeIRCMsg(irc_msg);
  return;
}

void FreeIRCMsg(struct irc_msg_t *irc_msg)
{
  if(irc_msg) {
    if(irc_msg->prefix)
      free(irc_msg->prefix);
    if(irc_msg->command)
      free(irc_msg->command);
    if(irc_msg->params)
      free(irc_msg->params);
    if(irc_msg->trailing)
      free(irc_msg->trailing);
    free(irc_msg);
  }
}

void irc_PING(void *handle, struct irc_msg_t *irc_msg)
{
  SendBufferAppend(handle, "PONG :");
  SendBufferAppend(handle, irc_msg->trailing);
  SendBufferAppend(handle, "\r\n");
}

void irc_PRIVMSG(void *handle, struct irc_msg_t *irc_msg)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;

  char *msg = irc_msg->trailing;

  if(msg[0] == '>') {
    char *cmd = msg+1;
    if(var_Type(intf, cmd) & VLC_VAR_ISCOMMAND) {
      vlc_value_t val;
      var_Set(intf, cmd, val);
    }
  }
}

void SendBufferAppend(void *handle, char *string)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;

  msg_Dbg(intf, "Sending %s", string);

  int new_len = sys->send_buffer_len + strlen(string);
  if(sys->send_buffer_len < SEND_BUFFER_LEN) {
    sys->send_buffer_len = new_len;
    ResizeSendBuffer(handle);
  }

  memcpy(sys->send_buffer+sys->send_buffer_loc, string, strlen(string) * sizeof(char));
  sys->send_buffer_loc += strlen(string);
}

void ResizeSendBuffer(void *handle)
{
  intf_thread_t *intf = (intf_thread_t*)handle;
  intf_sys_t *sys = intf->p_sys;
  
  sys->send_buffer = (char *)realloc(sys->send_buffer, sys->send_buffer_len * sizeof(char));  
}

/* im so sorry */
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
    else { /* theres only one token, must be trailing */
      irc_msg->trailing = irc_msg->params;
      irc_msg->params = NULL;
    }
  } else {
    ch = strtok_r(NULL, "", &sv_ptr);
    irc_msg->params = strdup(ch); /* grab params */
  }

  return irc_msg;
}

static void RegisterCallbacks(intf_thread_t *intf)
{
  /* Register commands that will be cleaned up upon object destruction */
#define ADD( name, type, target )                                   \
  var_Create(intf, name, VLC_VAR_ ## type | VLC_VAR_ISCOMMAND ); \
  var_AddCallback(intf, name, target, NULL );
    ADD("play", VOID, Playlist)
    ADD("pause", VOID, Playlist)
#undef ADD
}

static int Playlist(vlc_object_t *obj, char const *cmd,
                    vlc_value_t oldval, vlc_value_t newval, void *p_data)
{
  intf_thread_t *intf = (intf_thread_t*)obj;
  intf_sys_t *sys = intf->p_sys;

  playlist_t *playlist = sys->playlist;
  input_thread_t * input = playlist_CurrentInput(playlist);
  int state;

  if(input) {
    state = var_GetInteger(input, "state");
    vlc_object_release(input);
  } else {
    return VLC_EGENERIC;
  }

  if(strcmp(cmd, "pause") == 0) {
    msg_Info(intf, "Pause");    
    if(state == PLAYING_S)
      playlist_Pause(sys->playlist);
  } else if(strcmp(cmd, "play") == 0) {
    msg_Info(intf, "Play");
    if(state != PLAYING_S)
      playlist_Play(sys->playlist);
  }
}
