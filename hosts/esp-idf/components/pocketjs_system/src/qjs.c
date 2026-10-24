/*
 * The guest namespaces of the system services, the same shapes the 3DS
 * publishes (hosts/3ds/src/qjs.c):
 *
 *   ui.appTable/appLaunch/appShot   spec ops 39..41 (launcher.c)
 *   globalThis.link                 contracts/spec/link.ts (link.c)
 *   globalThis.net                  contracts/spec/net.ts over the link
 *   globalThis.wifi                 contracts/spec/wifi.ts (wifi.c)
 *   globalThis.fs                   contracts/spec/fs.ts, when the guest's
 *                                   plan declares data.fs (fs.c)
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "fs.h"
#include "launcher.h"
#include "link.h"
#include "pocketjs/guest_quickjs.h"
#include "system_internal.h"

enum {
  OP_APP_TABLE,
  OP_APP_LAUNCH,
  OP_APP_SHOT,
  OP_LINK_STATUS,
  OP_LINK_PAIR,
  OP_LINK_FORGET,
  OP_LINK_CATALOG,
  OP_LINK_INSTALL,
  OP_LINK_REMOVE,
  OP_LINK_POLL,
  OP_NET_START,
  OP_NET_TAKE,
  OP_NET_CANCEL,
  OP_NET_POLL,
  OP_NET_LAST_ERROR,
  OP_WIFI_STATUS,
  OP_WIFI_SET_ENABLED,
  OP_WIFI_CONNECT,
  OP_WIFI_SCAN,
  OP_WIFI_JOIN,
  OP_WIFI_FORGET,
  OP_FS_READ,
  OP_FS_WRITE,
  OP_FS_REMOVE,
  OP_FS_LIST,
  OP_FS_STAT,
  OP_FS_MKDIR,
  OP_FS_RENAME,
  OP_FS_USAGE,
  OP_FS_LAST_ERROR,
};

static int32_t argument_int(JSContext *ctx, int argc, JSValueConst *argv, int index) {
  int32_t value = 0;
  if (index < argc) JS_ToInt32(ctx, &value, argv[index]);
  return value;
}

static double argument_float(JSContext *ctx, int argc, JSValueConst *argv, int index) {
  double value = 0;
  if (index < argc) JS_ToFloat64(ctx, &value, argv[index]);
  return value;
}

/* A string argument, NULL with length 0 when absent; free with free_text. */
static const char *argument_text(JSContext *ctx, int argc, JSValueConst *argv, int index, size_t *length) {
  *length = 0;
  if (index >= argc || !JS_IsString(argv[index])) return NULL;
  return JS_ToCStringLen(ctx, length, argv[index]);
}

static void free_text(JSContext *ctx, const char *text) {
  if (text != NULL) JS_FreeCString(ctx, text);
}

/* An ArrayBuffer or a view of one, borrowed until JS runs again. */
static bool argument_bytes(JSContext *ctx, int argc, JSValueConst *argv, int index, uint8_t **bytes,
                           size_t *length) {
  if (index >= argc) return false;
  size_t direct_length = 0;
  uint8_t *direct = JS_GetArrayBuffer(ctx, &direct_length, argv[index]);
  if (direct != NULL) {
    *bytes = direct;
    *length = direct_length;
    return true;
  }
  JS_FreeValue(ctx, JS_GetException(ctx));
  size_t offset = 0;
  size_t view_length = 0;
  size_t element = 0;
  JSValue buffer = JS_GetTypedArrayBuffer(ctx, argv[index], &offset, &view_length, &element);
  if (JS_IsException(buffer)) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
  }
  size_t base_length = 0;
  uint8_t *base = JS_GetArrayBuffer(ctx, &base_length, buffer);
  JS_FreeValue(ctx, buffer);
  if (base == NULL || offset + view_length > base_length) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
  }
  *bytes = base + offset;
  *length = view_length;
  return true;
}

static JSValue text_or_undefined(JSContext *ctx, const char *text) {
  return text == NULL ? JS_UNDEFINED : JS_NewString(ctx, text);
}

static JSValue fs_line(JSContext *ctx, const char *line) {
  return JS_NewString(ctx, line != NULL ? line : "{\"error\":\"out of memory\"}");
}

static JSValue operation(JSContext *ctx, JSValueConst this_value, int argc, JSValueConst *argv, int magic) {
  (void)this_value;
  size_t length = 0;
  size_t second_length = 0;
  const char *text = NULL;
  const char *second = NULL;
  uint8_t *bytes = NULL;
  size_t byte_length = 0;
  JSValue result = JS_UNDEFINED;
  switch (magic) {
    case OP_APP_TABLE:
      return JS_NewString(ctx, launcher_table_json());
    case OP_APP_LAUNCH:
      text = argument_text(ctx, argc, argv, 0, &length);
      result = JS_NewInt32(ctx, text != NULL && launcher_launch(text) ? 1 : 0);
      break;
    case OP_APP_SHOT:
      return JS_NewInt32(ctx, -1);
    case OP_LINK_STATUS:
      return JS_NewString(ctx, link_status_json());
    case OP_LINK_PAIR:
    case OP_LINK_FORGET:
    case OP_LINK_REMOVE:
      text = argument_text(ctx, argc, argv, 0, &length);
      if (text == NULL) return JS_FALSE;
      result = JS_NewBool(ctx, magic == OP_LINK_PAIR     ? link_pair(text, length)
                               : magic == OP_LINK_FORGET ? link_forget(text)
                                                         : link_remove(text, length));
      break;
    case OP_LINK_INSTALL:
      text = argument_text(ctx, argc, argv, 0, &length);
      result = JS_NewInt32(ctx, text == NULL ? -1 : link_install(text, length));
      break;
    case OP_LINK_CATALOG:
      return JS_NewInt32(ctx, link_catalog());
    case OP_LINK_POLL:
      return text_or_undefined(ctx, link_poll());
    case OP_NET_START:
      text = argument_text(ctx, argc, argv, 0, &length);
      if (text == NULL) return JS_NewInt32(ctx, -1);
      /* Borrowed after the string conversion and used before JS runs:
       * start() copies the body. */
      if (!argument_bytes(ctx, argc, argv, 1, &bytes, &byte_length)) {
        bytes = NULL;
        byte_length = 0;
      }
      result = JS_NewInt32(ctx, link_net_start(text, length, bytes, byte_length));
      break;
    case OP_NET_TAKE:
      if (!argument_bytes(ctx, argc, argv, 1, &bytes, &byte_length)) return JS_NewInt32(ctx, -1);
      return JS_NewInt32(ctx, link_net_take(argument_int(ctx, argc, argv, 0), bytes, byte_length));
    case OP_NET_CANCEL:
      link_net_cancel(argument_int(ctx, argc, argv, 0));
      return JS_UNDEFINED;
    case OP_NET_POLL:
      return text_or_undefined(ctx, link_net_poll());
    case OP_NET_LAST_ERROR:
      return JS_NewString(ctx, link_net_last_error());
    case OP_WIFI_STATUS:
      return JS_NewString(ctx, wifi_status_json());
    case OP_WIFI_SET_ENABLED:
      return JS_NewBool(ctx, wifi_set_enabled(argc > 0 && JS_ToBool(ctx, argv[0])));
    case OP_WIFI_CONNECT:
      return JS_NewBool(ctx, wifi_connect(argument_int(ctx, argc, argv, 0)));
    case OP_WIFI_SCAN:
      return JS_NewBool(ctx, wifi_scan());
    case OP_WIFI_JOIN:
      text = argument_text(ctx, argc, argv, 0, &length);
      second = argument_text(ctx, argc, argv, 1, &second_length);
      result = JS_NewBool(ctx, text != NULL && wifi_join(text, length, second != NULL ? second : "", second_length));
      break;
    case OP_WIFI_FORGET:
      return JS_NewBool(ctx, wifi_forget(argument_int(ctx, argc, argv, 0)));
    case OP_FS_READ:
    case OP_FS_LIST:
    case OP_FS_STAT:
      text = argument_text(ctx, argc, argv, 0, &length);
      result = fs_line(ctx, magic == OP_FS_READ
                                ? fs_read(text, length, argument_float(ctx, argc, argv, 1),
                                          argument_float(ctx, argc, argv, 2))
                            : magic == OP_FS_LIST ? fs_list(text, length, argument_float(ctx, argc, argv, 1))
                                                  : fs_stat(text, length));
      break;
    case OP_FS_WRITE:
      text = argument_text(ctx, argc, argv, 0, &length);
      second = argument_text(ctx, argc, argv, 1, &second_length);
      result = JS_NewInt32(ctx, fs_write(text, length, second, second_length, argument_int(ctx, argc, argv, 2)));
      break;
    case OP_FS_REMOVE:
    case OP_FS_MKDIR:
      text = argument_text(ctx, argc, argv, 0, &length);
      result = JS_NewInt32(ctx, magic == OP_FS_REMOVE ? fs_remove(text, length, argument_int(ctx, argc, argv, 1))
                                                      : fs_mkdir(text, length));
      break;
    case OP_FS_RENAME:
      text = argument_text(ctx, argc, argv, 0, &length);
      second = argument_text(ctx, argc, argv, 1, &second_length);
      result = JS_NewInt32(ctx, fs_rename(text, length, second, second_length));
      break;
    case OP_FS_USAGE:
      return fs_line(ctx, fs_usage());
    case OP_FS_LAST_ERROR:
      return JS_NewString(ctx, fs_last_error());
    default:
      break;
  }
  free_text(ctx, text);
  free_text(ctx, second);
  return result;
}

static void add(JSContext *ctx, JSValue object, const char *name, int arity, int magic) {
  JS_SetPropertyStr(ctx, object, name,
                    JS_NewCFunctionMagic(ctx, operation, name, arity, JS_CFUNC_generic_magic, magic));
}

static esp_err_t install(JSContext *ctx, void *unused) {
  (void)unused;
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ui = JS_GetPropertyStr(ctx, global, "ui");
  if (!JS_IsObject(ui)) {
    JS_FreeValue(ctx, ui);
    JS_FreeValue(ctx, global);
    return ESP_ERR_INVALID_STATE;
  }
  add(ctx, ui, "appTable", 0, OP_APP_TABLE);
  add(ctx, ui, "appLaunch", 1, OP_APP_LAUNCH);
  add(ctx, ui, "appShot", 0, OP_APP_SHOT);
  JS_FreeValue(ctx, ui);

  JSValue link = JS_NewObject(ctx);
  add(ctx, link, "status", 0, OP_LINK_STATUS);
  add(ctx, link, "pair", 1, OP_LINK_PAIR);
  add(ctx, link, "forget", 1, OP_LINK_FORGET);
  add(ctx, link, "catalog", 0, OP_LINK_CATALOG);
  add(ctx, link, "install", 1, OP_LINK_INSTALL);
  add(ctx, link, "remove", 1, OP_LINK_REMOVE);
  add(ctx, link, "poll", 0, OP_LINK_POLL);
  JS_SetPropertyStr(ctx, global, "link", link);

  JSValue net = JS_NewObject(ctx);
  add(ctx, net, "start", 2, OP_NET_START);
  add(ctx, net, "take", 2, OP_NET_TAKE);
  add(ctx, net, "cancel", 1, OP_NET_CANCEL);
  add(ctx, net, "poll", 0, OP_NET_POLL);
  add(ctx, net, "lastError", 0, OP_NET_LAST_ERROR);
  JS_SetPropertyStr(ctx, global, "net", net);

  JSValue wifi = JS_NewObject(ctx);
  add(ctx, wifi, "status", 0, OP_WIFI_STATUS);
  add(ctx, wifi, "setEnabled", 1, OP_WIFI_SET_ENABLED);
  add(ctx, wifi, "connect", 1, OP_WIFI_CONNECT);
  add(ctx, wifi, "scan", 0, OP_WIFI_SCAN);
  add(ctx, wifi, "join", 2, OP_WIFI_JOIN);
  add(ctx, wifi, "forget", 1, OP_WIFI_FORGET);
  JS_SetPropertyStr(ctx, global, "wifi", wifi);

  /* Only a guest whose plan declares data.fs has a bound tree. */
  if (fs_bound()) {
    JSValue fs = JS_NewObject(ctx);
    add(ctx, fs, "read", 3, OP_FS_READ);
    add(ctx, fs, "write", 3, OP_FS_WRITE);
    add(ctx, fs, "remove", 2, OP_FS_REMOVE);
    add(ctx, fs, "list", 2, OP_FS_LIST);
    add(ctx, fs, "stat", 1, OP_FS_STAT);
    add(ctx, fs, "mkdir", 1, OP_FS_MKDIR);
    add(ctx, fs, "rename", 2, OP_FS_RENAME);
    add(ctx, fs, "usage", 0, OP_FS_USAGE);
    add(ctx, fs, "lastError", 0, OP_FS_LAST_ERROR);
    JS_SetPropertyStr(ctx, global, "fs", fs);
  }
  bool failed = JS_HasException(ctx);
  JS_FreeValue(ctx, global);
  return failed ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t system_qjs_install(pocketjs_guest_t *guest) {
  return pocketjs_guest_quickjs_install_once(guest, "pocketjs.system", install, NULL);
}
