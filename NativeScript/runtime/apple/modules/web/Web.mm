#ifdef __APPLE__

#include "Web.h"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "js_native_api.h"
#include "jsr.h"
#include "native_api_util.h"
#include "runtime/apple/Runtime.h"
#include "runtime/apple/Util.h"

namespace nativescript {

namespace {

struct FetchCompletion {
  napi_env env = nullptr;
  napi_deferred deferred = nullptr;
  bool ok = false;
  bool redirected = false;
  long status = 0;
  std::string statusText;
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<uint8_t> body;
  std::string errorName;
  std::string errorMessage;
};

struct WebSocketContext {
  int64_t id = 0;
  napi_env env = nullptr;
  napi_ref callback = nullptr;
  NSURLSession* session = nil;
  NSURLSessionWebSocketTask* task = nil;
  std::atomic<bool> closing{false};
  std::atomic<bool> cleaned{false};
};

std::mutex g_fetchMutex;
std::unordered_map<int64_t, NSURLSessionDataTask*> g_fetchTasks;

std::mutex g_wsMutex;
std::unordered_map<int64_t, std::shared_ptr<WebSocketContext>> g_webSockets;
std::atomic<int64_t> g_nextWebSocketId{1};

std::string ToString(napi_env env, napi_value value) {
  napi_value coerced;
  if (napi_coerce_to_string(env, value, &coerced) != napi_ok) {
    return "";
  }

  size_t length = 0;
  if (napi_get_value_string_utf8(env, coerced, nullptr, 0, &length) != napi_ok) {
    return "";
  }

  std::vector<char> buffer(length + 1);
  if (napi_get_value_string_utf8(env, coerced, buffer.data(), buffer.size(), nullptr) != napi_ok) {
    return "";
  }

  return {buffer.data(), length};
}

bool IsNullOrUndefined(napi_env env, napi_value value) {
  if (value == nullptr) {
    return true;
  }

  napi_valuetype type;
  if (napi_typeof(env, value, &type) != napi_ok) {
    return false;
  }

  return type == napi_null || type == napi_undefined;
}

void SetNamedString(napi_env env, napi_value object, const char* name, const std::string& value) {
  napi_set_named_property(env, object, name, napi_util::to_js_string(env, value));
}

napi_value CreateError(napi_env env, const std::string& name, const std::string& message) {
  napi_value messageValue = napi_util::to_js_string(env, message);
  napi_value error = nullptr;

  if (name == "TypeError") {
    napi_create_type_error(env, nullptr, messageValue, &error);
  } else {
    napi_create_error(env, nullptr, messageValue, &error);
    SetNamedString(env, error, "name", name);
  }

  return error;
}

void ResolveFetch(std::shared_ptr<FetchCompletion> completion) {
  if (completion == nullptr || !Runtime::IsAlive(completion->env)) {
    return;
  }

  auto runtime = Runtime::GetRuntime(completion->env);
  if (runtime == nullptr) {
    return;
  }

  ExecuteOnRunLoop(
      runtime->RuntimeLoop(),
      [completion] {
        if (!Runtime::IsAlive(completion->env)) {
          return;
        }

        NapiScope scope(completion->env);

        if (!completion->errorName.empty()) {
          napi_value error =
              CreateError(completion->env, completion->errorName, completion->errorMessage);
          napi_reject_deferred(completion->env, completion->deferred, error);
          return;
        }

        napi_value result;
        napi_create_object(completion->env, &result);

        napi_set_named_property(completion->env, result, "ok",
                                completion->ok ? napi_util::get_true(completion->env)
                                               : napi_util::get_false(completion->env));

        napi_set_named_property(completion->env, result, "redirected",
                                completion->redirected ? napi_util::get_true(completion->env)
                                                       : napi_util::get_false(completion->env));

        napi_set_named_property(
            completion->env, result, "status",
            napi_util::to_js_number(completion->env, static_cast<int32_t>(completion->status)));

        SetNamedString(completion->env, result, "statusText", completion->statusText);
        SetNamedString(completion->env, result, "url", completion->url);

        napi_value headers;
        napi_create_array_with_length(completion->env, completion->headers.size(), &headers);

        for (size_t i = 0; i < completion->headers.size(); i++) {
          napi_value pair;
          napi_create_array_with_length(completion->env, 2, &pair);
          napi_set_element(completion->env, pair, 0,
                           napi_util::to_js_string(completion->env, completion->headers[i].first));
          napi_set_element(completion->env, pair, 1,
                           napi_util::to_js_string(completion->env, completion->headers[i].second));
          napi_set_element(completion->env, headers, i, pair);
        }

        napi_set_named_property(completion->env, result, "headers", headers);

        napi_value bodyValue;
        if (completion->body.empty()) {
          void* emptyBuffer = nullptr;
          napi_create_arraybuffer(completion->env, 0, &emptyBuffer, &bodyValue);
        } else {
          uint8_t* buffer = static_cast<uint8_t*>(malloc(completion->body.size()));
          memcpy(buffer, completion->body.data(), completion->body.size());
          napi_create_external_arraybuffer(
              completion->env, buffer, completion->body.size(),
              [](napi_env, void* data, void*) { free(data); }, nullptr, &bodyValue);
        }

        napi_set_named_property(completion->env, result, "body", bodyValue);

        napi_resolve_deferred(completion->env, completion->deferred, result);
      },
      true);
}

bool ToHeaderDictionary(napi_env env, napi_value value,
                        NSMutableDictionary<NSString*, NSString*>* dict) {
  if (IsNullOrUndefined(env, value)) {
    return true;
  }

  napi_valuetype type;
  napi_typeof(env, value, &type);
  if (type != napi_object) {
    return false;
  }

  napi_value names;
  if (napi_get_property_names(env, value, &names) != napi_ok) {
    return false;
  }

  uint32_t length = 0;
  napi_get_array_length(env, names, &length);

  for (uint32_t i = 0; i < length; i++) {
    napi_value key;
    napi_get_element(env, names, i, &key);

    napi_value jsValue;
    napi_get_property(env, value, key, &jsValue);

    std::string keyUtf8 = ToString(env, key);
    std::string valueUtf8 = ToString(env, jsValue);

    NSString* nsKey = [NSString stringWithUTF8String:keyUtf8.c_str()];
    NSString* nsValue = [NSString stringWithUTF8String:valueUtf8.c_str()];

    if (nsKey != nil && nsValue != nil) {
      [dict setObject:nsValue forKey:nsKey];
    }
  }

  return true;
}

bool ValueToBytes(napi_env env, napi_value value, std::vector<uint8_t>& out) {
  if (IsNullOrUndefined(env, value)) {
    out.clear();
    return true;
  }

  bool isArrayBuffer = false;
  napi_is_arraybuffer(env, value, &isArrayBuffer);
  if (isArrayBuffer) {
    void* data = nullptr;
    size_t length = 0;
    napi_get_arraybuffer_info(env, value, &data, &length);
    out.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + length);
    return true;
  }

  bool isTypedArray = false;
  napi_is_typedarray(env, value, &isTypedArray);
  if (isTypedArray) {
    napi_typedarray_type typedArrayType;
    size_t length = 0;
    void* data = nullptr;
    napi_value buffer;
    size_t byteOffset = 0;
    napi_get_typedarray_info(env, value, &typedArrayType, &length, &data, &buffer, &byteOffset);

    size_t elementSize = 1;
    switch (typedArrayType) {
      case napi_int16_array:
      case napi_uint16_array:
        elementSize = 2;
        break;
      case napi_int32_array:
      case napi_uint32_array:
      case napi_float32_array:
        elementSize = 4;
        break;
      case napi_float64_array:
      case napi_bigint64_array:
      case napi_biguint64_array:
        elementSize = 8;
        break;
      default:
        elementSize = 1;
        break;
    }

    const uint8_t* ptr = static_cast<uint8_t*>(data);
    out.assign(ptr, ptr + (length * elementSize));
    return true;
  }

  std::string text = ToString(env, value);
  out.assign(text.begin(), text.end());
  return true;
}

void RemoveFetchTask(int64_t requestId) {
  if (requestId <= 0) {
    return;
  }

  NSURLSessionDataTask* taskToRelease = nil;
  {
    std::lock_guard<std::mutex> lock(g_fetchMutex);
    auto it = g_fetchTasks.find(requestId);
    if (it != g_fetchTasks.end()) {
      taskToRelease = it->second;
      g_fetchTasks.erase(it);
    }
  }

  if (taskToRelease != nil) {
    [taskToRelease release];
  }
}

std::shared_ptr<WebSocketContext> GetWebSocket(int64_t id) {
  std::lock_guard<std::mutex> lock(g_wsMutex);
  auto it = g_webSockets.find(id);
  if (it == g_webSockets.end()) {
    return nullptr;
  }

  return it->second;
}

void EmitWebSocketEvent(std::shared_ptr<WebSocketContext> socket, const std::string& type,
                        const std::string& textData, const std::vector<uint8_t>& binaryData,
                        bool isBinary, int code, const std::string& reason,
                        const std::string& protocol, const std::string& errorMessage) {
  if (socket == nullptr || socket->callback == nullptr || !Runtime::IsAlive(socket->env)) {
    return;
  }

  auto runtime = Runtime::GetRuntime(socket->env);
  if (runtime == nullptr) {
    return;
  }

  ExecuteOnRunLoop(
      runtime->RuntimeLoop(),
      [socket, type, textData, binaryData, isBinary, code, reason, protocol, errorMessage] {
        if (socket->callback == nullptr || !Runtime::IsAlive(socket->env)) {
          return;
        }

        NapiScope scope(socket->env);
        napi_value callback;
        napi_get_reference_value(socket->env, socket->callback, &callback);
        if (!napi_util::is_of_type(socket->env, callback, napi_function)) {
          return;
        }

        napi_value eventObj;
        napi_create_object(socket->env, &eventObj);
        SetNamedString(socket->env, eventObj, "type", type);

        if (type == "message") {
          if (isBinary) {
            napi_value dataValue;
            if (binaryData.empty()) {
              void* emptyBuffer = nullptr;
              napi_create_arraybuffer(socket->env, 0, &emptyBuffer, &dataValue);
            } else {
              uint8_t* buffer = static_cast<uint8_t*>(malloc(binaryData.size()));
              memcpy(buffer, binaryData.data(), binaryData.size());
              napi_create_external_arraybuffer(
                  socket->env, buffer, binaryData.size(),
                  [](napi_env, void* ptr, void*) { free(ptr); }, nullptr, &dataValue);
            }
            napi_set_named_property(socket->env, eventObj, "data", dataValue);
            napi_set_named_property(socket->env, eventObj, "binary",
                                    napi_util::get_true(socket->env));
          } else {
            SetNamedString(socket->env, eventObj, "data", textData);
            napi_set_named_property(socket->env, eventObj, "binary",
                                    napi_util::get_false(socket->env));
          }
        }

        if (type == "open") {
          SetNamedString(socket->env, eventObj, "protocol", protocol);
        }

        if (type == "close") {
          napi_set_named_property(socket->env, eventObj, "code",
                                  napi_util::to_js_number(socket->env, code));
          SetNamedString(socket->env, eventObj, "reason", reason);
          napi_set_named_property(socket->env, eventObj, "wasClean",
                                  napi_util::get_true(socket->env));
        }

        if (type == "error") {
          SetNamedString(socket->env, eventObj, "message", errorMessage);
        }

        napi_value global;
        napi_get_global(socket->env, &global);
        napi_value args[1] = {eventObj};
        napi_call_function(socket->env, global, callback, 1, args, nullptr);
      },
      true);
}

void CleanupWebSocket(std::shared_ptr<WebSocketContext> socket) {
  if (socket == nullptr) {
    return;
  }

  bool expected = false;
  if (!socket->cleaned.compare_exchange_strong(expected, true)) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_wsMutex);
    g_webSockets.erase(socket->id);
  }

  if (socket->task != nil) {
    [socket->task cancel];
    [socket->task release];
    socket->task = nil;
  }

  if (socket->session != nil) {
    [socket->session invalidateAndCancel];
    [socket->session release];
    socket->session = nil;
  }

  if (socket->callback != nullptr && Runtime::IsAlive(socket->env)) {
    auto runtime = Runtime::GetRuntime(socket->env);
    if (runtime != nullptr) {
      napi_ref callback = socket->callback;
      socket->callback = nullptr;

      ExecuteOnRunLoop(
          runtime->RuntimeLoop(),
          [env = socket->env, callback] {
            if (Runtime::IsAlive(env)) {
              NapiScope scope(env);
              napi_delete_reference(env, callback);
            }
          },
          true);
    } else {
      socket->callback = nullptr;
    }
  }
}

void ReceiveNextWebSocketMessage(std::shared_ptr<WebSocketContext> socket) {
  if (socket == nullptr || socket->task == nil || socket->closing.load() ||
      socket->cleaned.load()) {
    return;
  }

  [socket->task
      receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* message, NSError* error) {
        if (socket->cleaned.load()) {
          return;
        }

        if (error != nil) {
          NSString* desc = [error localizedDescription];
          std::string err = desc == nil ? "WebSocket error" : std::string([desc UTF8String]);
          EmitWebSocketEvent(socket, "error", "", {}, false, 0, "", "", err);
          if (!socket->closing.load()) {
            EmitWebSocketEvent(socket, "close", "", {}, false, 1006, "", "", "");
          }
          CleanupWebSocket(socket);
          return;
        }

        if (message == nil) {
          return;
        }

        if (message.type == NSURLSessionWebSocketMessageTypeString) {
          NSString* value = message.string;
          std::string str = value == nil ? "" : std::string([value UTF8String]);
          EmitWebSocketEvent(socket, "message", str, {}, false, 0, "", "", "");
        } else if (message.type == NSURLSessionWebSocketMessageTypeData) {
          NSData* data = message.data;
          std::vector<uint8_t> bytes;
          if (data != nil && [data length] > 0) {
            bytes.resize([data length]);
            memcpy(bytes.data(), [data bytes], [data length]);
          }
          EmitWebSocketEvent(socket, "message", "", bytes, true, 0, "", "", "");
        }

        if (!socket->closing.load() && !socket->cleaned.load()) {
          ReceiveNextWebSocketMessage(socket);
        }
      }];
}

napi_value FetchNative(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  if (argc < 5) {
    napi_throw_type_error(
        env, nullptr,
        "__ns__fetchNative(url, method, headers, body, requestId) expects 5 arguments");
    return nullptr;
  }

  std::string url = ToString(env, argv[0]);
  std::string method = ToString(env, argv[1]);

  int64_t requestId = 0;
  if (napi_util::is_of_type(env, argv[4], napi_number)) {
    napi_get_value_int64(env, argv[4], &requestId);
  }

  napi_deferred deferred;
  napi_value promise;
  napi_create_promise(env, &deferred, &promise);

  NSURL* nsUrl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
  NSString* scheme = [nsUrl scheme];
  if (nsUrl == nil || scheme == nil || [scheme length] == 0) {
    napi_reject_deferred(env, deferred,
                         CreateError(env, "TypeError", "Invalid URL passed to fetch"));
    return promise;
  }

  NSMutableURLRequest* request =
      [NSMutableURLRequest requestWithURL:nsUrl
                              cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                          timeoutInterval:60.0];

  NSString* nsMethod = [NSString stringWithUTF8String:(method.empty() ? "GET" : method.c_str())];
  [request setHTTPMethod:nsMethod];

  NSMutableDictionary<NSString*, NSString*>* headers = [NSMutableDictionary dictionary];
  if (!ToHeaderDictionary(env, argv[2], headers)) {
    napi_reject_deferred(env, deferred,
                         CreateError(env, "TypeError", "Invalid headers object"));
    return promise;
  }
  [request setAllHTTPHeaderFields:headers];

  std::vector<uint8_t> bodyBytes;
  if (!ValueToBytes(env, argv[3], bodyBytes)) {
    napi_reject_deferred(env, deferred,
                         CreateError(env, "TypeError", "Invalid body value"));
    return promise;
  }

  std::string methodUpper = method;
  std::transform(methodUpper.begin(), methodUpper.end(), methodUpper.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

  if (!bodyBytes.empty() && methodUpper != "GET" && methodUpper != "HEAD") {
    NSData* body = [NSData dataWithBytes:bodyBytes.data() length:bodyBytes.size()];
    [request setHTTPBody:body];
  }

  auto completion = std::make_shared<FetchCompletion>();
  completion->env = env;
  completion->deferred = deferred;

  std::string originalUrl = url;

  NSURLSessionDataTask* task = [[NSURLSession sharedSession]
      dataTaskWithRequest:request
        completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
          RemoveFetchTask(requestId);

          if (error != nil) {
            completion->errorName =
                [error code] == NSURLErrorCancelled ? "AbortError" : "TypeError";
            NSString* message = [error localizedDescription];
            completion->errorMessage =
                message == nil ? "Network request failed" : std::string([message UTF8String]);
            ResolveFetch(completion);
            return;
          }

          NSHTTPURLResponse* httpResponse = [response isKindOfClass:[NSHTTPURLResponse class]]
                                                ? (NSHTTPURLResponse*)response
                                                : nil;

          if (httpResponse != nil) {
            completion->status = [httpResponse statusCode];
            completion->ok = completion->status >= 200 && completion->status < 300;

            NSString* statusText =
                [NSHTTPURLResponse localizedStringForStatusCode:completion->status];
            if (statusText != nil) {
              completion->statusText = [statusText UTF8String];
            }

            NSDictionary* responseHeaders = [httpResponse allHeaderFields];
            for (id key in responseHeaders) {
              NSString* name = [key description];
              NSString* value = [[responseHeaders objectForKey:key] description];
              completion->headers.emplace_back(name == nil ? "" : std::string([name UTF8String]),
                                               value == nil ? "" : std::string([value UTF8String]));
            }
          }

          NSString* responseUrl = [[response URL] absoluteString];
          completion->url =
              responseUrl == nil ? originalUrl : std::string([responseUrl UTF8String]);
          completion->redirected = completion->url != originalUrl;

          if (data != nil && [data length] > 0) {
            completion->body.resize([data length]);
            memcpy(completion->body.data(), [data bytes], [data length]);
          }

          ResolveFetch(completion);
        }];

  if (requestId > 0) {
    std::lock_guard<std::mutex> lock(g_fetchMutex);
    g_fetchTasks[requestId] = [task retain];
  }

  [task resume];

  return promise;
}

napi_value FetchAbort(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN(1)

  int64_t requestId = 0;
  if (napi_get_value_int64(env, argv[0], &requestId) != napi_ok || requestId <= 0) {
    return nullptr;
  }

  NSURLSessionDataTask* task = nil;
  {
    std::lock_guard<std::mutex> lock(g_fetchMutex);
    auto it = g_fetchTasks.find(requestId);
    if (it != g_fetchTasks.end()) {
      task = it->second;
      g_fetchTasks.erase(it);
    }
  }

  if (task != nil) {
    [task cancel];
    [task release];
  }

  return nullptr;
}

napi_value WebSocketCreate(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  if (argc < 3) {
    napi_throw_type_error(env, nullptr,
                          "__ns__wsCreate(url, protocols, callback) expects 3 arguments");
    return nullptr;
  }

  if (!napi_util::is_of_type(env, argv[2], napi_function)) {
    napi_throw_type_error(env, nullptr, "WebSocket callback must be a function");
    return nullptr;
  }

  std::string url = ToString(env, argv[0]);
  NSURL* nsUrl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
  if (nsUrl == nil) {
    napi_throw_type_error(env, nullptr, "Invalid WebSocket URL");
    return nullptr;
  }

  NSMutableArray<NSString*>* protocols = [NSMutableArray array];
  if (!IsNullOrUndefined(env, argv[1])) {
    bool isArray = false;
    napi_is_array(env, argv[1], &isArray);
    if (isArray) {
      uint32_t length = 0;
      napi_get_array_length(env, argv[1], &length);
      for (uint32_t i = 0; i < length; i++) {
        napi_value item;
        napi_get_element(env, argv[1], i, &item);
        std::string protocol = ToString(env, item);
        NSString* nsProtocol = [NSString stringWithUTF8String:protocol.c_str()];
        if (nsProtocol != nil) {
          [protocols addObject:nsProtocol];
        }
      }
    } else {
      std::string protocol = ToString(env, argv[1]);
      NSString* nsProtocol = [NSString stringWithUTF8String:protocol.c_str()];
      if (nsProtocol != nil) {
        [protocols addObject:nsProtocol];
      }
    }
  }

  int64_t id = g_nextWebSocketId.fetch_add(1);

  auto context = std::make_shared<WebSocketContext>();
  context->id = id;
  context->env = env;
  napi_create_reference(env, argv[2], 1, &context->callback);

  NSURLSessionConfiguration* config = [NSURLSessionConfiguration defaultSessionConfiguration];
  NSURLSession* session = [NSURLSession sessionWithConfiguration:config
                                                        delegate:nil
                                                   delegateQueue:nil];

  NSURLSessionWebSocketTask* task = [session webSocketTaskWithURL:nsUrl protocols:protocols];

  context->session = [session retain];
  context->task = [task retain];

  {
    std::lock_guard<std::mutex> lock(g_wsMutex);
    g_webSockets[id] = context;
  }

  [task resume];
  EmitWebSocketEvent(context, "open", "", {}, false, 0, "", "", "");
  ReceiveNextWebSocketMessage(context);

  return napi_util::to_js_number(env, static_cast<double>(id));
}

napi_value WebSocketSend(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  if (argc < 2) {
    napi_throw_type_error(env, nullptr, "__ns__wsSend(id, data) expects 2 args");
    return nullptr;
  }

  int64_t socketId = 0;
  if (napi_get_value_int64(env, argv[0], &socketId) != napi_ok || socketId <= 0) {
    napi_throw_type_error(env, nullptr, "Invalid WebSocket id");
    return nullptr;
  }

  auto socket = GetWebSocket(socketId);
  if (socket == nullptr || socket->task == nil || socket->cleaned.load()) {
    napi_throw_type_error(env, nullptr, "WebSocket is not open");
    return nullptr;
  }

  bool isArrayBuffer = false;
  napi_is_arraybuffer(env, argv[1], &isArrayBuffer);

  bool isTypedArray = false;
  napi_is_typedarray(env, argv[1], &isTypedArray);

  if (isArrayBuffer || isTypedArray) {
    std::vector<uint8_t> data;
    if (!ValueToBytes(env, argv[1], data)) {
      napi_throw_type_error(env, nullptr, "Invalid WebSocket binary payload");
      return nullptr;
    }

    NSData* nsData =
        data.empty() ? [NSData data] : [NSData dataWithBytes:data.data() length:data.size()];
    NSURLSessionWebSocketMessage* message =
        [[[NSURLSessionWebSocketMessage alloc] initWithData:nsData] autorelease];

    [socket->task sendMessage:message
            completionHandler:^(NSError* error) {
              if (error != nil) {
                NSString* desc = [error localizedDescription];
                std::string err =
                    desc == nil ? "WebSocket send error" : std::string([desc UTF8String]);
                EmitWebSocketEvent(socket, "error", "", {}, false, 0, "", "", err);
              }
            }];

    return nullptr;
  }

  std::string text = ToString(env, argv[1]);
  NSString* nsText = [NSString stringWithUTF8String:text.c_str()];
  NSURLSessionWebSocketMessage* message =
      [[[NSURLSessionWebSocketMessage alloc] initWithString:nsText] autorelease];

  [socket->task sendMessage:message
          completionHandler:^(NSError* error) {
            if (error != nil) {
              NSString* desc = [error localizedDescription];
              std::string err =
                  desc == nil ? "WebSocket send error" : std::string([desc UTF8String]);
              EmitWebSocketEvent(socket, "error", "", {}, false, 0, "", "", err);
            }
          }];

  return nullptr;
}

napi_value WebSocketClose(napi_env env, napi_callback_info info) {
  NAPI_CALLBACK_BEGIN_VARGS()

  if (argc < 1) {
    return nullptr;
  }

  int64_t socketId = 0;
  if (napi_get_value_int64(env, argv[0], &socketId) != napi_ok || socketId <= 0) {
    return nullptr;
  }

  auto socket = GetWebSocket(socketId);
  if (socket == nullptr || socket->task == nil || socket->cleaned.load()) {
    return nullptr;
  }

  int32_t code = 1000;
  if (argc > 1 && !IsNullOrUndefined(env, argv[1])) {
    napi_get_value_int32(env, argv[1], &code);
  }

  std::string reason;
  if (argc > 2 && !IsNullOrUndefined(env, argv[2])) {
    reason = ToString(env, argv[2]);
  }

  socket->closing.store(true);

  NSData* reasonData =
      reason.empty() ? [NSData data] : [NSData dataWithBytes:reason.data() length:reason.length()];

  [socket->task cancelWithCloseCode:(NSURLSessionWebSocketCloseCode)code reason:reasonData];

  EmitWebSocketEvent(socket, "close", "", {}, false, code, reason, "", "");
  CleanupWebSocket(socket);

  return nullptr;
}

void InstallWebRuntimeScript(napi_env env) {
  const char* script = R"WEB(
    (function (global) {
      'use strict';

      const nativeFetch = global.__ns__fetchNative;
      const nativeFetchAbort = global.__ns__fetchAbort;
      const nativeWsCreate = global.__ns__wsCreate;
      const nativeWsSend = global.__ns__wsSend;
      const nativeWsClose = global.__ns__wsClose;

      if (typeof nativeFetch !== 'function' || typeof nativeWsCreate !== 'function') {
        return;
      }

      const hasOwn = Object.prototype.hasOwnProperty;

      function normalizeHeaderName(name) {
        const normalized = String(name).toLowerCase();
        if (!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(normalized)) {
          throw new TypeError(`Invalid HTTP header name: ${name}`);
        }
        return normalized;
      }

      function normalizeHeaderValue(value) {
        return String(value).trim();
      }

      class Headers {
        constructor(init = undefined) {
          this._map = new Map();

          if (init instanceof Headers) {
            init.forEach((value, key) => this.append(key, value));
            return;
          }

          if (Array.isArray(init)) {
            for (const pair of init) {
              if (!Array.isArray(pair) || pair.length !== 2) {
                throw new TypeError('Each header pair must be a [name, value] tuple');
              }
              this.append(pair[0], pair[1]);
            }
            return;
          }

          if (init && typeof init === 'object') {
            for (const key of Object.keys(init)) {
              this.append(key, init[key]);
            }
          }
        }

        append(name, value) {
          const key = normalizeHeaderName(name);
          const normalized = normalizeHeaderValue(value);
          const existing = this._map.get(key);
          if (existing === undefined) {
            this._map.set(key, [normalized]);
          } else {
            existing.push(normalized);
          }
        }

        delete(name) {
          this._map.delete(normalizeHeaderName(name));
        }

        get(name) {
          const value = this._map.get(normalizeHeaderName(name));
          return value ? value.join(', ') : null;
        }

        has(name) {
          return this._map.has(normalizeHeaderName(name));
        }

        set(name, value) {
          this._map.set(normalizeHeaderName(name), [normalizeHeaderValue(value)]);
        }

        forEach(callback, thisArg = undefined) {
          for (const [key, values] of this._map.entries()) {
            callback.call(thisArg, values.join(', '), key, this);
          }
        }

        *keys() {
          for (const key of this._map.keys()) {
            yield key;
          }
        }

        *values() {
          for (const values of this._map.values()) {
            yield values.join(', ');
          }
        }

        *entries() {
          for (const [key, values] of this._map.entries()) {
            yield [key, values.join(', ')];
          }
        }

        [Symbol.iterator]() {
          return this.entries();
        }
      }

      function normalizeMethod(method = 'GET') {
        const normalized = String(method).toUpperCase();
        return normalized;
      }

      function cloneArrayBuffer(buffer) {
        if (!(buffer instanceof ArrayBuffer)) {
          return new ArrayBuffer(0);
        }

        const copy = new ArrayBuffer(buffer.byteLength);
        new Uint8Array(copy).set(new Uint8Array(buffer));
        return copy;
      }

      function stringToArrayBuffer(text) {
        const value = String(text);
        if (typeof TextEncoder === 'function') {
          return new TextEncoder().encode(value).buffer;
        }

        const bytes = new Uint8Array(value.length);
        for (let i = 0; i < value.length; i++) {
          bytes[i] = value.charCodeAt(i) & 0xff;
        }
        return bytes.buffer;
      }

      function arrayBufferToText(buffer) {
        if (typeof TextDecoder === 'function') {
          return new TextDecoder().decode(new Uint8Array(buffer));
        }

        let result = '';
        const bytes = new Uint8Array(buffer);
        for (let i = 0; i < bytes.length; i++) {
          result += String.fromCharCode(bytes[i]);
        }
        return result;
      }

      function bodyToArrayBuffer(body) {
        if (body === undefined || body === null) {
          return new ArrayBuffer(0);
        }

        if (body instanceof ArrayBuffer) {
          return cloneArrayBuffer(body);
        }

        if (ArrayBuffer.isView(body)) {
          const view = body;
          const slice = view.buffer.slice(view.byteOffset, view.byteOffset + view.byteLength);
          return cloneArrayBuffer(slice);
        }

        return stringToArrayBuffer(body);
      }

      class ReadableStreamDefaultReader {
        constructor(stream) {
          if (!(stream instanceof ReadableStream)) {
            throw new TypeError('ReadableStreamDefaultReader requires a ReadableStream');
          }
          if (stream.locked) {
            throw new TypeError('ReadableStream is already locked');
          }
          this._stream = stream;
          stream._reader = this;
        }

        read() {
          if (!this._stream) {
            return Promise.reject(new TypeError('Reader has been released'));
          }
          return this._stream._read();
        }

        releaseLock() {
          if (this._stream) {
            this._stream._reader = null;
            this._stream = null;
          }
        }

        cancel(reason) {
          if (!this._stream) {
            return Promise.resolve();
          }
          return this._stream.cancel(reason);
        }

        [Symbol.asyncIterator]() {
          return {
            next: () => this.read(),
          };
        }
      }

      class ReadableStream {
        constructor(underlyingSource = {}) {
          this._queue = [];
          this._pendingReads = [];
          this._reader = null;
          this._closed = false;
          this._errored = null;

          this._pull = typeof underlyingSource.pull === 'function' ? underlyingSource.pull : null;
          this._cancel = typeof underlyingSource.cancel === 'function' ? underlyingSource.cancel : null;

          const controller = {
            enqueue: (chunk) => {
              if (this._closed || this._errored) {
                return;
              }
              if (this._pendingReads.length > 0) {
                const read = this._pendingReads.shift();
                read.resolve({ value: chunk, done: false });
              } else {
                this._queue.push(chunk);
              }
            },
            close: () => {
              if (this._closed) {
                return;
              }
              this._closed = true;
              while (this._pendingReads.length > 0) {
                const read = this._pendingReads.shift();
                read.resolve({ value: undefined, done: true });
              }
            },
            error: (error) => {
              if (this._errored) {
                return;
              }
              this._errored = error;
              while (this._pendingReads.length > 0) {
                const read = this._pendingReads.shift();
                read.reject(error);
              }
            },
          };

          if (typeof underlyingSource.start === 'function') {
            Promise.resolve()
              .then(() => underlyingSource.start(controller))
              .catch((error) => controller.error(error));
          }

          this._controller = controller;
        }

        get locked() {
          return this._reader !== null;
        }

        getReader() {
          return new ReadableStreamDefaultReader(this);
        }

        _read() {
          if (this._queue.length > 0) {
            const value = this._queue.shift();
            return Promise.resolve({ value, done: false });
          }

          if (this._errored) {
            return Promise.reject(this._errored);
          }

          if (this._closed) {
            return Promise.resolve({ value: undefined, done: true });
          }

          if (this._pull) {
            Promise.resolve().then(() => this._pull(this._controller)).catch((error) => {
              this._controller.error(error);
            });
          }

          return new Promise((resolve, reject) => {
            this._pendingReads.push({ resolve, reject });
          });
        }

        cancel(reason) {
          this._closed = true;
          this._queue.length = 0;

          while (this._pendingReads.length > 0) {
            const read = this._pendingReads.shift();
            read.resolve({ value: undefined, done: true });
          }

          if (this._cancel) {
            return Promise.resolve(this._cancel(reason));
          }

          return Promise.resolve();
        }
      }

      class WritableStream {
        constructor(sink = {}) {
          this._sink = sink;
          this._closed = false;
        }

        getWriter() {
          return {
            write: (chunk) => {
              if (this._closed) {
                return Promise.reject(new TypeError('WritableStream is closed'));
              }
              if (typeof this._sink.write === 'function') {
                return Promise.resolve(this._sink.write(chunk));
              }
              return Promise.resolve();
            },
            close: () => {
              this._closed = true;
              if (typeof this._sink.close === 'function') {
                return Promise.resolve(this._sink.close());
              }
              return Promise.resolve();
            },
            abort: (reason) => {
              this._closed = true;
              if (typeof this._sink.abort === 'function') {
                return Promise.resolve(this._sink.abort(reason));
              }
              return Promise.resolve();
            },
          };
        }
      }

      class TransformStream {
        constructor(transformer = {}) {
          let controller = null;
          this.readable = new ReadableStream({
            start(ctrl) {
              controller = ctrl;
            },
          });

          this.writable = new WritableStream({
            write(chunk) {
              if (typeof transformer.transform === 'function') {
                return Promise.resolve(transformer.transform(chunk, controller));
              }
              controller.enqueue(chunk);
              return Promise.resolve();
            },
            close() {
              if (typeof transformer.flush === 'function') {
                return Promise.resolve(transformer.flush(controller)).then(() => {
                  controller.close();
                });
              }
              controller.close();
              return Promise.resolve();
            },
            abort(reason) {
              controller.error(reason || new Error('TransformStream aborted'));
              return Promise.resolve();
            },
          });
        }
      }

      class ByteLengthQueuingStrategy {
        constructor({ highWaterMark } = { highWaterMark: 1 }) {
          this.highWaterMark = Number(highWaterMark);
          this.size = (chunk) => {
            if (chunk == null) {
              return 0;
            }
            if (typeof chunk.byteLength === 'number') {
              return chunk.byteLength;
            }
            return 1;
          };
        }
      }

      class CountQueuingStrategy {
        constructor({ highWaterMark } = { highWaterMark: 1 }) {
          this.highWaterMark = Number(highWaterMark);
          this.size = () => 1;
        }
      }

      function createReadableStreamFromBuffer(buffer) {
        let sent = false;
        return new ReadableStream({
          pull(controller) {
            if (!sent) {
              sent = true;
              controller.enqueue(new Uint8Array(buffer));
            }
            controller.close();
          },
        });
      }

      const kBodyBuffer = Symbol('bodyBuffer');
      const kBodyUsed = Symbol('bodyUsed');
      const kBodyStream = Symbol('bodyStream');

      class Body {
        constructor(body = null) {
          this[kBodyBuffer] = bodyToArrayBuffer(body);
          this[kBodyUsed] = false;
          this[kBodyStream] = null;
        }

        get body() {
          if (this[kBodyStream] === null) {
            this[kBodyStream] = createReadableStreamFromBuffer(this[kBodyBuffer]);
          }
          return this[kBodyStream];
        }

        get bodyUsed() {
          return this[kBodyUsed];
        }

        _consumeBody() {
          if (this[kBodyUsed]) {
            return Promise.reject(new TypeError('Body has already been consumed'));
          }
          this[kBodyUsed] = true;
          return Promise.resolve(cloneArrayBuffer(this[kBodyBuffer]));
        }

        arrayBuffer() {
          return this._consumeBody();
        }

        text() {
          return this._consumeBody().then((buffer) => arrayBufferToText(buffer));
        }

        json() {
          return this.text().then((text) => JSON.parse(text));
        }
      }

      class Request extends Body {
        constructor(input, init = {}) {
          if (input instanceof Request) {
            const body = hasOwn.call(init, 'body') ? init.body : cloneArrayBuffer(input[kBodyBuffer]);
            super(body);

            this.url = input.url;
            this.method = normalizeMethod(init.method || input.method || 'GET');
            this.headers = new Headers(init.headers || input.headers);
            this.signal = init.signal || input.signal || null;
          } else {
            super(init.body);

            this.url = String(input);
            this.method = normalizeMethod(init.method || 'GET');
            this.headers = new Headers(init.headers);
            this.signal = init.signal || null;
          }

          if ((this.method === 'GET' || this.method === 'HEAD') && this[kBodyBuffer].byteLength > 0) {
            throw new TypeError('Request with GET/HEAD method cannot have body');
          }
        }

        clone() {
          return new Request(this);
        }
      }

      class Response extends Body {
        constructor(body = null, init = {}) {
          super(body);

          const status = init.status == null ? 200 : Number(init.status);
          if (status < 200 || status > 599) {
            throw new RangeError('Invalid status code');
          }

          this.status = status;
          this.statusText = init.statusText == null ? '' : String(init.statusText);
          this.headers = new Headers(init.headers);
          this.url = init.url == null ? '' : String(init.url);
          this.redirected = Boolean(init.redirected);
          this.type = 'default';
        }

        get ok() {
          return this.status >= 200 && this.status < 300;
        }

        clone() {
          return new Response(cloneArrayBuffer(this[kBodyBuffer]), {
            status: this.status,
            statusText: this.statusText,
            headers: this.headers,
            url: this.url,
            redirected: this.redirected,
          });
        }

        static json(data, init = {}) {
          const headers = new Headers(init.headers);
          if (!headers.has('content-type')) {
            headers.set('content-type', 'application/json');
          }

          return new Response(JSON.stringify(data), {
            ...init,
            headers,
          });
        }
      }

      function createAbortError() {
        if (typeof DOMException === 'function') {
          return new DOMException('The operation was aborted.', 'AbortError');
        }
        const error = new Error('The operation was aborted.');
        error.name = 'AbortError';
        return error;
      }

      function createInvalidStateError(message) {
        if (typeof DOMException === 'function') {
          return new DOMException(message, 'InvalidStateError');
        }
        const error = new Error(message);
        error.name = 'InvalidStateError';
        return error;
      }

      let nextFetchRequestId = 1;

      function fetch(input, init = undefined) {
        const request = input instanceof Request && init === undefined ? input : new Request(input, init || {});

        if (request.signal && request.signal.aborted) {
          return Promise.reject(createAbortError());
        }

        const requestId = nextFetchRequestId++;

        const headerObject = {};
        request.headers.forEach((value, key) => {
          headerObject[key] = value;
        });

        let abortListener = null;
        if (request.signal && typeof request.signal.addEventListener === 'function') {
          abortListener = () => {
            nativeFetchAbort(requestId);
          };
          request.signal.addEventListener('abort', abortListener, { once: true });
        }

        const cleanupAbortListener = () => {
            if (abortListener && request.signal && typeof request.signal.removeEventListener === 'function') {
              request.signal.removeEventListener('abort', abortListener);
            }
        };

        return nativeFetch(
          request.url,
          request.method,
          headerObject,
          request[kBodyBuffer],
          requestId,
        ).then(
          (nativeResponse) => {
            cleanupAbortListener();
            return new Response(nativeResponse.body, {
              status: nativeResponse.status,
              statusText: nativeResponse.statusText,
              headers: nativeResponse.headers,
              url: nativeResponse.url,
              redirected: nativeResponse.redirected,
            });
          },
          (error) => {
            cleanupAbortListener();
            if (error && error.name === 'AbortError') {
              throw createAbortError();
            }
            if (error instanceof TypeError) {
              throw error;
            }
            throw new TypeError(error && error.message ? error.message : 'Network request failed');
          },
        );
      }

      class Event {
        constructor(type, init = {}) {
          this.type = String(type);
          this.bubbles = Boolean(init.bubbles);
          this.cancelable = Boolean(init.cancelable);
          this.defaultPrevented = false;
          this.target = null;
          this.currentTarget = null;
        }

        preventDefault() {
          if (this.cancelable) {
            this.defaultPrevented = true;
          }
        }
      }

      class MessageEvent extends Event {
        constructor(type, init = {}) {
          super(type, init);
          this.data = init.data;
        }
      }

      class CloseEvent extends Event {
        constructor(type, init = {}) {
          super(type, init);
          this.code = init.code == null ? 0 : Number(init.code);
          this.reason = init.reason == null ? '' : String(init.reason);
          this.wasClean = Boolean(init.wasClean);
        }
      }

      class EventTarget {
        constructor() {
          this._listeners = new Map();
        }

        addEventListener(type, listener) {
          if (typeof listener !== 'function') {
            return;
          }
          const key = String(type);
          const existing = this._listeners.get(key);
          if (existing) {
            existing.add(listener);
          } else {
            this._listeners.set(key, new Set([listener]));
          }
        }

        removeEventListener(type, listener) {
          const key = String(type);
          const existing = this._listeners.get(key);
          if (!existing) {
            return;
          }
          existing.delete(listener);
          if (existing.size === 0) {
            this._listeners.delete(key);
          }
        }

        dispatchEvent(event) {
          if (!(event instanceof Event)) {
            throw new TypeError('dispatchEvent expects an Event instance');
          }

          event.target = this;
          event.currentTarget = this;

          const listeners = this._listeners.get(event.type);
          if (listeners) {
            for (const listener of Array.from(listeners)) {
              listener.call(this, event);
            }
          }

          const handler = this[`on${event.type}`];
          if (typeof handler === 'function') {
            handler.call(this, event);
          }

          return !event.defaultPrevented;
        }
      }

      class WebSocket extends EventTarget {
        constructor(url, protocols = []) {
          super();

          const parsed = new URL(String(url));
          if (parsed.protocol !== 'ws:' && parsed.protocol !== 'wss:') {
            throw new SyntaxError('WebSocket URL protocol must be ws or wss');
          }

          if (typeof protocols === 'string') {
            protocols = [protocols];
          } else if (protocols == null) {
            protocols = [];
          } else if (!Array.isArray(protocols)) {
            throw new TypeError('protocols must be a string or array');
          }

          this.url = parsed.href;
          this.readyState = WebSocket.CONNECTING;
          this.bufferedAmount = 0;
          this.extensions = '';
          this.protocol = '';
          this.binaryType = 'arraybuffer';

          this.onopen = null;
          this.onmessage = null;
          this.onerror = null;
          this.onclose = null;

          this._socketId = nativeWsCreate(this.url, protocols, (event) => {
            this._handleNativeEvent(event);
          });
        }

        _handleNativeEvent(event) {
          if (!event || typeof event.type !== 'string') {
            return;
          }

          if (event.type === 'open') {
            this.readyState = WebSocket.OPEN;
            this.protocol = event.protocol || '';
            this.dispatchEvent(new Event('open'));
            return;
          }

          if (event.type === 'message') {
            const payload = event.binary ? event.data : String(event.data);
            this.dispatchEvent(new MessageEvent('message', { data: payload }));
            return;
          }

          if (event.type === 'error') {
            this.dispatchEvent(new Event('error'));
            return;
          }

          if (event.type === 'close') {
            this.readyState = WebSocket.CLOSED;
            this.dispatchEvent(new CloseEvent('close', {
              code: event.code,
              reason: event.reason,
              wasClean: event.wasClean,
            }));
          }
        }

        send(data) {
          if (this.readyState !== WebSocket.OPEN) {
            throw createInvalidStateError('WebSocket is not open');
          }

          nativeWsSend(this._socketId, data);
        }

        close(code = 1000, reason = '') {
          if (this.readyState === WebSocket.CLOSING || this.readyState === WebSocket.CLOSED) {
            return;
          }

          this.readyState = WebSocket.CLOSING;
          nativeWsClose(this._socketId, code, reason);
        }
      }

      Object.defineProperties(WebSocket, {
        CONNECTING: { value: 0 },
        OPEN: { value: 1 },
        CLOSING: { value: 2 },
        CLOSED: { value: 3 },
      });

      Object.defineProperties(WebSocket.prototype, {
        CONNECTING: { value: 0 },
        OPEN: { value: 1 },
        CLOSING: { value: 2 },
        CLOSED: { value: 3 },
      });

      if (typeof global.Event !== 'function') {
        global.Event = Event;
      }
      if (typeof global.EventTarget !== 'function') {
        global.EventTarget = EventTarget;
      }
      if (typeof global.MessageEvent !== 'function') {
        global.MessageEvent = MessageEvent;
      }
      if (typeof global.CloseEvent !== 'function') {
        global.CloseEvent = CloseEvent;
      }

      global.Headers = Headers;
      global.Request = Request;
      global.Response = Response;
      global.fetch = fetch;
      global.WebSocket = WebSocket;

      if (typeof global.ReadableStream !== 'function') {
        global.ReadableStream = ReadableStream;
      }
      if (typeof global.ReadableStreamDefaultReader !== 'function') {
        global.ReadableStreamDefaultReader = ReadableStreamDefaultReader;
      }
      if (typeof global.WritableStream !== 'function') {
        global.WritableStream = WritableStream;
      }
      if (typeof global.TransformStream !== 'function') {
        global.TransformStream = TransformStream;
      }
      if (typeof global.ByteLengthQueuingStrategy !== 'function') {
        global.ByteLengthQueuingStrategy = ByteLengthQueuingStrategy;
      }
      if (typeof global.CountQueuingStrategy !== 'function') {
        global.CountQueuingStrategy = CountQueuingStrategy;
      }
    })(globalThis);
  )WEB";

  napi_value source;
  napi_create_string_utf8(env, script, NAPI_AUTO_LENGTH, &source);
  napi_value unused;
  napi_run_script(env, source, &unused);
}

}  // namespace

void Web::Init(napi_env env, napi_value global) {
  napi_util::napi_set_function(env, global, "__ns__fetchNative", FetchNative);
  napi_util::napi_set_function(env, global, "__ns__fetchAbort", FetchAbort);
  napi_util::napi_set_function(env, global, "__ns__wsCreate", WebSocketCreate);
  napi_util::napi_set_function(env, global, "__ns__wsSend", WebSocketSend);
  napi_util::napi_set_function(env, global, "__ns__wsClose", WebSocketClose);

  InstallWebRuntimeScript(env);
}

napi_value Web::LoadInternalModule(napi_env env, const std::string& moduleName) {
  if (moduleName != "web" && moduleName != "stream/web" && moduleName != "node:web" &&
      moduleName != "node:stream/web") {
    return nullptr;
  }

  napi_value moduleObj;
  napi_value exports;
  napi_create_object(env, &moduleObj);
  napi_create_object(env, &exports);

  napi_value global;
  napi_get_global(env, &global);

  if (moduleName == "web" || moduleName == "node:web") {
    const char* names[] = {"fetch",    "WebSocket",      "Headers",        "Request",
                           "Response", "ReadableStream", "WritableStream", "TransformStream"};

    for (auto name : names) {
      napi_value value;
      if (napi_get_named_property(env, global, name, &value) == napi_ok) {
        napi_set_named_property(env, exports, name, value);
      }
    }
  } else {
    const char* names[] = {
        "ReadableStream",  "ReadableStreamDefaultReader", "WritableStream",
        "TransformStream", "ByteLengthQueuingStrategy",   "CountQueuingStrategy"};

    for (auto name : names) {
      napi_value value;
      if (napi_get_named_property(env, global, name, &value) == napi_ok) {
        napi_set_named_property(env, exports, name, value);
      }
    }
  }

  napi_set_named_property(env, moduleObj, "exports", exports);
  return moduleObj;
}

}  // namespace nativescript

#endif  // __APPLE__
