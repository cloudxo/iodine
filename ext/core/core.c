#include "rb-registry.h"
#include "rb-call.h"
#include "lib-server.h"
#include <ruby.h>
#include <ruby/thread.h>
#include <ruby/encoding.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>

///////////////
// This is a bridge between lib-server and Ruby.
//
// This defines two classes/modules:
//
// * Protocol class/module (integrated into whatever we get):
//        - **Callbacks**
//        - :on_open
//        - :on_data ( + on_message)
//        - :on_shutdown
//        - :on_close
//        - :ping
//        - :on_finish (class function)
//        - **Helpers**
//        - :each - returns an array of the protocols... (remember `service`)
//        - :read - reads X bytes from the connection to a specific buffer(?).
//        - :write - writes to the buffer
//        - :close - writes a "close" frame to the buffer
//        - :force_close - closes the connection immediately.
//        - :upgrade - change protocols.
//        - :server - returns the server object (?).
//        - :set_timeout - set a connection's timeout
//        - :touch - resets timeout counter for longer tasks.
//        - :run - runs an async task
//        - :run_after - runs an async timer
//        - :run_every - runs an async timer (repeats)
//
// - Server Settings / server core that offers a simple settings frame:
//        - **Callbacks**
//        - :on_start - callback.
//        - :on_finish - callback.
//        - :on_idle - callback.
//        - :on_tick - callback.
//        - **Helpers**
//        - :run - runs an async task
//        - :run_after - runs an async timer
//        - :run_every - runs an async timer (repeats)
//        - :protocol= - sets/gets the protocol **module**.
//        - :threads= - sets/gets the amount of threads used.
//        - :processes= - sets/gets the amount of forks.
//        - :timeout= - sets the default timeout for new connections.
//        - :timeout= - sets the default timeout for new connections.
//        - :port= - sets the port for the connection. port should be a string.
//        - :start - starts the server

static char* VERSION;
static int BinaryEncodingIndex;  // encoding index
static VALUE rProtocol;          // protocol module
static VALUE rCore;              // core class
static VALUE rServer;            // server object to Ruby class
static ID server_var_id;         // id for the Server variable (pointer)
static ID fd_var_id;             // id for the file descriptor (Fixnum)
static ID buff_var_id;           // id for the file descriptor (Fixnum)
static ID call_proc_id;          // id for the Proc.call method
static ID new_func_id;           // id for the Class.new method
static ID on_open_func_id;       // the on_open callback's ID
static ID on_data_func_id;       // the on_data callback
static ID on_message_func_id;    // the on_message optional
static ID on_shutdown_func_id;   // the on_shutdown callback
static ID on_close_func_id;      // the on_close callback
static ID ping_func_id;          // the ping callback

// for debugging:
// static void print_func_name_from_id(ID func) {
//   ID __id_list__[9] = {call_proc_id,         // id for the Proc.call method
//                        new_func_id,          // id for the Class.new method
//                        on_open_func_id,      // the on_open callback's ID
//                        on_data_func_id,      // the on_data callback
//                        on_message_func_id,   // the on_message optional
//                        on_shutdown_func_id,  // the on_shutdown callback
//                        on_close_func_id,     // the on_close callback
//                        ping_func_id,         // the ping callback
//                        0};
//   char* __name_list__[] = {"call",                // id for the Proc.call
//   method
//                            "new",                 // id for the Class.new
//                            method
//                            "on_open",             // the on_open callback's
//                            ID
//                            "on_data",             // the on_data callback
//                            "on_message_func_id",  // the on_message optional
//                            "on_shutdown_func_id",  // the on_shutdown
//                            callback
//                            "on_close_func_id",     // the on_close callback
//                            "ping_func_id",         // the ping callback
//                            0};
//   int i = 0;
//   while (__id_list__[i]) {
//     if (__id_list__[i] == func) {
//       printf("calling %s\n", __name_list__[i]);
//       return;
//     }
//     i++;
//   }
//   printf("calling unknown Ruby method\n");
// }

////////////////////////////////////////////////////////////////////////
// Every object (the Iodine core, the protocols) will have a reference to their
// respective `struct Server` object, allowing them to invoke server methods
// (i.e. `write`, `close`, etc')...
//
// This data (a C pointer) needs to be attached to the Ruby objects via a T_DATA
// object variable. These T_DATA types define memory considerations for the GC.
//
// We need to make sure Ruby doesn't free our server along with our object...

// we need this so GC doesn't free the server object... (a false free method)
// maybe there's another way to do this?
void dont_free(void* obj) {}
// define the server data type
static struct rb_data_type_struct my_server_type = {
    .wrap_struct_name = "IodineServer",
    .function.dfree = (void (*)(void*))dont_free,
};

////////////////////////////////////////////////////////////////////////
// The Ruby framework manages it's own contextc switching and memory...
// this means that when we make calls to Ruby (i.e. creating Ruby objects),
// we rick currupting the Ruby framework. Also, Ruby uses all these signals (for
// it's context switchings) and long-jumps (i.e. Ruby exceptions) that can drive
// the C code a little nuts...
//
// seperation is reqiured :-)

////////////////////////////////////////////////////////////////////////
// Lib-Server provides helper methods that are very benificial.
//
// The functions manage access to these C methods from the Ruby objects.

// performs pending async tasks while managing their Ruby registry.
static void perform_async(void* task) {
  RubyCaller.call((VALUE)task, call_proc_id);
  Registry.remove((VALUE)task);
  // // DON'T do this... async tasks might be persistent methods...
  // rb_gc_force_recycle(task);
}

// performs pending protocol task while managing it's Ruby registry.
static void perform_protocol_async(struct Server* srv, int fd, void* task) {
  RubyCaller.call((VALUE)task, call_proc_id);
  Registry.remove((VALUE)task);
  // // DON'T do this... async tasks might be persistent methods...
  // rb_gc_force_recycle(task);
}

// run a protocol task (preventing concurrency)
static VALUE run_protocol_task(VALUE self) {
  // requires a block to be passed
  rb_need_block();
  // get the server object
  struct Server* srv =
      (struct Server*)DATA_PTR(rb_ivar_get(self, server_var_id));
  // requires multi-threading
  if (Server.settings(srv)->threads < 0) {
    rb_warn(
        "called an async method in a non-async mode - the task will "
        "be performed immediately.");
    rb_yield(Qnil);
  }
  VALUE block = rb_block_proc();
  if (block == Qnil)
    return Qnil;
  Registry.add(block);
  Server.fd_task(srv, FIX2INT(rb_ivar_get(self, fd_var_id)),
                 perform_protocol_async, (void*)block);
  return block;
}

// schedules async tasks while managing their Ruby registry.
static VALUE run_async(VALUE self) {
  // requires a block to be passed
  rb_need_block();
  // get the server object
  struct Server* srv =
      (struct Server*)DATA_PTR(rb_ivar_get(self, server_var_id));
  // requires multi-threading
  if (Server.settings(srv)->threads < 0) {
    rb_warn(
        "called an async method in a non-async mode - the task will "
        "be performed immediately.");
    rb_yield(Qnil);
  }
  VALUE block = rb_block_proc();
  if (block == Qnil)
    return Qnil;
  Registry.add(block);
  Server.run_async(srv, perform_async, (void*)block);
  return block;
}

// writes data to the connection
static VALUE srv_write(VALUE self, VALUE data) {
  struct Server* srv = DATA_PTR(rb_ivar_get(self, server_var_id));
  int fd = FIX2INT(rb_ivar_get(self, fd_var_id));
  return LONG2FIX(Server.write(srv, fd, RSTRING_PTR(data), RSTRING_LEN(data)));
}
// writes data to the connection
static VALUE srv_write_urgent(VALUE self, VALUE data) {
  struct Server* srv = DATA_PTR(rb_ivar_get(self, server_var_id));
  int fd = FIX2INT(rb_ivar_get(self, fd_var_id));
  return LONG2FIX(
      Server.write_urgent(srv, fd, RSTRING_PTR(data), RSTRING_LEN(data)));
}

// reads from a connection, up to x size bytes
// DEFAULTS TO 10Kb
static VALUE srv_read(int argc, VALUE* argv, VALUE self) {
  if (argc > 1) {
    rb_raise(
        rb_eArgError,
        "read accepts only one argument - a Fixnum (buffer length) or a String "
        "(it's capacity - or 1Kb, whichever's the higher - will be used as "
        "buffer's length).");
    return Qnil;
  }
  VALUE buffer = (argc == 1 ? argv[0] : Qnil);
  if (buffer != Qnil && TYPE(buffer) != T_FIXNUM && TYPE(buffer) != T_STRING) {
    rb_raise(rb_eTypeError,
             "buffer should either be a length (a new string will be created) "
             "or a string (reading will be limited to the original string's "
             "capacity or 1Kb - whichever the larger).");
    return Qnil;
  }
  VALUE str;
  long len;
  int fd = FIX2INT(rb_ivar_get(self, fd_var_id));
  if (buffer == Qnil) {
    buffer = LONG2FIX(1024);
  }
  if (TYPE(buffer) == T_FIXNUM) {
    len = FIX2LONG(buffer);
    if (len <= 0)
      len = 1024;
    str = rb_str_buf_new(len);
    // create a rb_String with X length and take it's pointer
    // rb_str_resize(VALUE str, long len)
    // RSTRING_PTR(str)
  } else {
    // take the string's pointer and length
    len = rb_str_capacity(buffer);
    if (len < 1024)
      rb_str_resize(buffer, (len = 1024));
    str = buffer;
  }
  // struct Server* srv = DATA_PTR(rb_ivar_get(self, server_var_id));
  ssize_t in = Server.read(fd, RSTRING_PTR(str), len);
  // make sure it's binary encoded
  rb_enc_associate_index(str, BinaryEncodingIndex);
  // set actual size....
  if (in > 0)
    rb_str_set_len(str, (long)in);
  else
    rb_str_set_len(str, 0);
  // return empty string? or fix above if to return Qnil?
  return str;
}

// closes a connection, gracefully
static VALUE srv_close(VALUE self, VALUE data) {
  struct Server* srv = DATA_PTR(rb_ivar_get(self, server_var_id));
  int fd = FIX2INT(rb_ivar_get(self, fd_var_id));
  Server.close(srv, fd);
  return Qnil;
}

// closes a connection, without waiting for writing to finish
static VALUE srv_force_close(VALUE self, VALUE data) {
  int fd = FIX2INT(rb_ivar_get(self, fd_var_id));
  close(fd);
  return Qnil;
}

// replaces a connection's existing protocol with another
static VALUE srv_upgrade(VALUE self, VALUE protocol) {
  if (protocol == Qnil)
    return Qnil;
  struct Server* srv = DATA_PTR(rb_ivar_get(self, server_var_id));
  int fd = FIX2INT(rb_ivar_get(self, fd_var_id));
  // include the rProtocol within the object.
  if (TYPE(protocol) == T_CLASS) {
    // include the Protocol module
    // // do we neet to check?
    // if (rb_mod_include_p(protocol, rProtocol) == Qfalse)
    rb_include_module(protocol, rProtocol);
    protocol = RubyCaller.call_unsafe(protocol, new_func_id);
  } else {
    // include the Protocol module in the object's class
    VALUE p_class = rb_obj_class(protocol);
    // // do we neet to check?
    // if (rb_mod_include_p(p_class, rProtocol) == Qfalse)
    rb_include_module(p_class, rProtocol);
  }
  // make sure everything whent as it should
  if (protocol == Qnil)
    return Qnil;
  // set the new protocol at the server's udata
  Server.set_udata(srv, fd, (void*)protocol);
  // remove old protocol from the Registry.
  Registry.remove(self);
  // add new protocol to the Registry
  Registry.add(protocol);
  // initialize pre-required variables
  rb_ivar_set(protocol, fd_var_id, INT2FIX(fd));
  rb_ivar_set(protocol, server_var_id,
              TypedData_Wrap_Struct(rServer, &my_server_type, srv));
  // initialize the new protocol
  RubyCaller.call_unsafe(protocol, on_open_func_id);
  return protocol;
}

////////////////////////////////////////////////////////////////////////
// Lib-Server uses callbacks for the protocol.
//
// The following callbacks bridge the Ruby and C layers togethr.

//  default callback - do nothing
static VALUE empty_func(VALUE self) {
  return Qnil;
}
//  default callback - do nothing
static VALUE def_on_message(VALUE self, VALUE data) {
  return Qnil;
}
// defaule ping method
static VALUE no_ping_func(VALUE self) {
  struct Server* srv = DATA_PTR(rb_ivar_get(self, server_var_id));
  int fd = FIX2INT(rb_ivar_get(self, fd_var_id));
  // only close if the main code (on_data / task) isn't running.
  if (!Server.is_busy(srv, fd))
    Server.close(srv, fd);
  else  // prevent future timeouts until they occure again.
    Server.touch(srv, fd);
  return Qnil;
}
// defaule on_data method
static VALUE def_on_data(VALUE self) {
  VALUE buff = rb_ivar_get(self, buff_var_id);
  if (buff == Qnil) {
    rb_ivar_set(self, buff_var_id, (buff = rb_str_buf_new(1024)));
  }
  do {
    srv_read(1, &buff, self);
    if (!RSTRING_LEN(buff))
      return Qnil;
    rb_funcall(self, on_message_func_id, 1, buff);
  } while (RSTRING_LEN(buff) == rb_str_capacity(buff));
  return Qnil;
}

// a new connection - registers a new protocol object and forwards the event.
static void on_open(struct Server* server, int fd) {
  VALUE protocol = (VALUE)Server.get_udata(server, 0);
  protocol = RubyCaller.call(protocol, new_func_id);
  if (protocol == Qnil) {
    Server.close(server, fd);
    return;
  }
  Registry.add(protocol);
  rb_ivar_set(protocol, fd_var_id, INT2FIX(fd));
  rb_ivar_set(protocol, server_var_id,
              TypedData_Wrap_Struct(rServer, &my_server_type, server));
  Server.set_udata(server, fd, (void*)protocol);
  RubyCaller.call(protocol, on_open_func_id);
}

// // called when data is pending on the connection.
// // (this is a testing implementation)
// static void on_data(struct Server* server, int fd) {
//   char buff[128];
//   int in = 0;
//   if ((in = Server.read(fd, buff, 128)) <= 0)
//     return;
//   Server.write(server, fd, buff, in);
//   if (!memcmp(buff, "bye", 3)) {
//     Server.close(server, fd);
//   }
// }

// the on_data implementation
static void on_data(struct Server* server, int fd) {
  VALUE protocol = (VALUE)Server.get_udata(server, fd);
  if (!protocol)
    return;
  RubyCaller.call(protocol, on_data_func_id);
}

// calls the ping callback
static void ping(struct Server* server, int fd) {
  VALUE protocol = (VALUE)Server.get_udata(server, fd);
  if (!protocol)
    return;
  RubyCaller.call(protocol, ping_func_id);
}

// calls the on_shutdown callback
static void on_shutdown(struct Server* server, int fd) {
  VALUE protocol = (VALUE)Server.get_udata(server, fd);
  if (!protocol)
    return;
  RubyCaller.call(protocol, on_shutdown_func_id);
}

// calls the on_close callback and de-registers the connection
static void on_close(struct Server* server, int fd) {
  VALUE protocol = (VALUE)Server.get_udata(server, fd);
  if (!protocol)
    return;
  RubyCaller.call(protocol, on_close_func_id);
  Registry.remove(protocol);
  Server.set_udata(server, fd, 0);
  // // DON'T do this... async tasks might have bindings to this one...
  // rb_gc_force_recycle(protocol);
}

// called when the server starts up. Saves the server object to the instance.
static void on_init(struct Server* server) {
  VALUE core_instance = *((VALUE*)Server.settings(server)->udata);
  // save the updated protocol class as a global value on the server, using fd=0
  Server.set_udata(server, 0,
                   (void*)rb_ivar_get(core_instance, rb_intern("@protocol")));
  // message
  fprintf(stderr, "Starting up Iodine V. %s using %d thread%s X %d processes\n",
          VERSION, Server.settings(server)->threads,
          (Server.settings(server)->threads > 1 ? "s" : ""),
          Server.settings(server)->processes);
}

// called when server is idling
void on_idle(struct Server* srv) {
  // call(reg->obj, on_data_func_id);
  // rb_gc_start();
}

// called for each new thread
void on_new_thread(struct Server* srv) {
  // todo: register threads with Ruby....
  // fprintf(stderr, "A new worker thread is listening to tasks\n");
}

////////////////////////////////////////////////////////////////////////
// This function consolidates the server settings and starts the server.
//////////
// the no-GVL state
static void* srv_start_no_gvl(void* _settings) {
  struct ServerSettings* settings = _settings;
  long ret = Server.listen(*settings);
  if (ret < 0)
    perror("Couldn't start server");
  return 0;
}
/////////
// The stop unblock fun
static void unblck(void* _) {
  Server.stop_all();
}
/////////
// the actual method
static VALUE srv_start(VALUE self) {
  // load the settings from the Ruby layer to the C layer
  VALUE rb_protocol = rb_ivar_get(self, rb_intern("@protocol"));
  VALUE rb_port = rb_ivar_get(self, rb_intern("@port"));
  VALUE rb_bind = rb_ivar_get(self, rb_intern("@address"));
  VALUE rb_timeout = rb_ivar_get(self, rb_intern("@timeout"));
  VALUE rb_threads = rb_ivar_get(self, rb_intern("@threads"));
  VALUE rb_processes = rb_ivar_get(self, rb_intern("@processes"));
  VALUE rb_busymsg = rb_ivar_get(self, rb_intern("@busy_msg"));
  // validate protocol - this is the only required setting
  if (TYPE(rb_protocol) != T_CLASS) {
    rb_raise(rb_eTypeError,
             "protocol isn't a valid object (should be a class).");
    return Qnil;
  }
  // validate port
  if (rb_port != Qnil && TYPE(rb_port) != T_FIXNUM) {
    rb_raise(rb_eTypeError, "port isn't a valid number.");
    return Qnil;
  }
  int iport = rb_port == Qnil ? 3000 : FIX2INT(rb_port);
  if (iport > 65535 || iport < 0) {
    rb_raise(rb_eTypeError, "port out of range.");
    return Qnil;
  }
  // validate bind
  if (rb_bind != Qnil && TYPE(rb_bind) != T_STRING) {
    rb_raise(rb_eTypeError, "bind should be either a String or nil.");
    return Qnil;
  }
  if (rb_bind != Qnil)
    rb_warn("the `bind` property is ignored, unimplemented yet");
  // validate timeout
  if (rb_timeout != Qnil &&
      (TYPE(rb_timeout) != T_FIXNUM || (FIX2INT(rb_timeout) > 255) ||
       (FIX2INT(rb_timeout) < 0))) {
    rb_raise(rb_eTypeError,
             "timeout isn't a valid number (any number from 0 to 255).");
    return Qnil;
  }
  // validate process count and set limits
  if (rb_processes != Qnil && TYPE(rb_processes) != T_FIXNUM &&
      FIX2INT(rb_processes) > 32) {
    rb_raise(rb_eTypeError, "processes isn't a valid number (1-32).");
    return Qnil;
  }
  // validate thread count and set limits
  if (rb_threads != Qnil && TYPE(rb_threads) != T_FIXNUM &&
      FIX2INT(rb_threads) > 128) {
    rb_raise(rb_eTypeError, "threads isn't a valid number (-1 to 128).");
    return Qnil;
  }
  // validate busy message
  if (rb_busymsg != Qnil && TYPE(rb_busymsg) != T_STRING) {
    rb_raise(rb_eTypeError, "busy_msg should be either a String or nil.");
    return Qnil;
  }
  // make port into a CString (for Lib-Server)
  char port[7];
  char* bind = rb_bind == Qnil ? NULL : StringValueCStr(rb_bind);
  unsigned char timeout = rb_timeout == Qnil ? 10 : FIX2INT(rb_timeout);
  snprintf(port, 6, "%d", iport);
  if (TYPE(rb_protocol) == T_CLASS) {  // a class
    // include the protocol module
    rb_include_module(rb_protocol, rProtocol);
  } else {  // it's a module
    // extend the protocol module
  }
  struct Protocol protocol = {.on_open = on_open,
                              .on_data = on_data,
                              .ping = ping,
                              .on_shutdown = on_shutdown,
                              .on_close = on_close};
  struct ServerSettings settings = {
      .protocol = &protocol,
      .timeout = timeout,
      .threads = rb_threads == Qnil ? 0 : (FIX2INT(rb_threads)),
      .processes = rb_processes == Qnil ? 0 : (FIX2INT(rb_processes)),
      .on_init = on_init,
      .on_idle = on_idle,
      .on_init_thread = on_new_thread,
      .port = (iport > 0 ? port : NULL),
      .address = bind,
      .udata = &self,
      .busy_msg = (rb_busymsg == Qnil ? NULL : StringValueCStr(rb_busymsg)),
  };
  // rb_thread_call_without_gvl(slow_func, slow_arg, unblck_func, unblck_arg);
  rb_thread_call_without_gvl(srv_start_no_gvl, &settings, unblck, NULL);
  return Qnil;
}

////////////////////////////////////////////////////////////////////////
// Ruby loads the library and invokes the Init_<lib_name> function...
//
// Here we connect all the C code to the Ruby interface, completing the bridge
// between Lib-Server and Ruby.
void Init_core(void) {
  // initialize globally used IDs, for faster access to the Ruby layer.
  call_proc_id = rb_intern("call");
  server_var_id = rb_intern("server");
  new_func_id = rb_intern("new");
  fd_var_id = rb_intern("sockfd");
  on_open_func_id = rb_intern("on_open");
  on_data_func_id = rb_intern("on_data");
  on_shutdown_func_id = rb_intern("on_shutdown");
  on_close_func_id = rb_intern("on_close");
  ping_func_id = rb_intern("ping");
  on_message_func_id = rb_intern("on_message");
  buff_var_id = rb_intern("scrtbuffer");

  BinaryEncodingIndex = rb_enc_find_index("binary");

  // The core Iodine class wraps the ServerSettings and little more.
  rCore = rb_define_class("Iodine", rb_cObject);
  rb_define_method(rCore, "start", srv_start, 0);
  rb_define_method(rCore, "run", run_async, 0);
  rb_define_attr(rCore, "protocol", 1, 1);
  rb_define_attr(rCore, "port", 1, 1);
  rb_define_attr(rCore, "address", 1, 1);
  rb_define_attr(rCore, "threads", 1, 1);
  rb_define_attr(rCore, "processes", 1, 1);
  rb_define_attr(rCore, "timeout", 1, 1);
  rb_define_attr(rCore, "busy_msg", 1, 1);

  // get-set version
  {
    VALUE version = rb_const_get(rCore, rb_intern("VERSION"));
    if (version == Qnil)
      VERSION = "0.2.0";
    else
      VERSION = StringValueCStr(version);
  }
  // The Protocol module will inject helper methods and core functionality into
  // the Ruby protocol class provided by the user.
  rProtocol = rb_define_module_under(rCore, "Protocol");
  rb_define_method(rProtocol, "on_open", empty_func, 0);
  rb_define_method(rProtocol, "on_data", def_on_data, 0);
  rb_define_method(rProtocol, "on_message", def_on_message, 1);
  rb_define_method(rProtocol, "ping", no_ping_func, 0);
  rb_define_method(rProtocol, "on_shutdown", empty_func, 0);
  rb_define_method(rProtocol, "on_close", empty_func, 0);
  // helper method
  rb_define_method(rProtocol, "run", run_async, 0);
  rb_define_method(rProtocol, "defer", run_protocol_task, 0);
  rb_define_method(rProtocol, "read", srv_read, -1);
  rb_define_method(rProtocol, "write", srv_write, 1);
  rb_define_method(rProtocol, "write_urgent", srv_write_urgent, 1);
  rb_define_method(rProtocol, "close", srv_close, 0);
  rb_define_method(rProtocol, "force_close", srv_force_close, 0);
  rb_define_method(rProtocol, "upgrade", srv_upgrade, 1);

  // Every Protocol (and Server?) instance will hold a reference to the server
  // define the Server Ruby class.
  rServer = rb_define_class_under(rCore, "ServerObject", rb_cData);

  // Initialize the registry under the Iodine core
  Registry.init(rCore);
}
//
// require "/Users/2Be/rC/iodine/ext/ccore/core"
// i = IodineCore.new
// class Prtc
// def on_message data
//   write data
//   close if data =~ /^bye/
// end
// end
// i.protocol = Prtc
// i.threads = 2
// i.processes = 1
// i.start
