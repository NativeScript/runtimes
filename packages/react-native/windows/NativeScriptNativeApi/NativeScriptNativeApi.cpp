#include "pch.h"

#include "NativeScriptNativeApi.h"

#include "UIRuntime.h"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace winrt::NativeScriptNativeApi {

namespace {

std::mutex g_lock;
std::shared_ptr<NativeScriptRN::UIRuntime> g_runtime;
bool g_installed = false;

// This module: it embeds the windows-napi addon its UI-thread runtime loads.
HMODULE ThisModule() {
  HMODULE module{};
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&ThisModule),
      &module);
  return module;
}

} // namespace

void NativeScriptNativeApi::Initialize(React::ReactContext const &reactContext) noexcept {
  m_context = reactContext;
  std::lock_guard lock(g_lock);
  // One runtime per app: a reload gets a new React instance and module, not a new UI thread.
  if (!g_runtime) {
    auto dispatcher = reactContext.UIDispatcher();
    g_runtime = NativeScriptRN::UIRuntime::Create(
        [dispatcher](std::function<void()> work) { dispatcher.Post([work = std::move(work)]() { work(); }); },
        ThisModule());
  }
}

bool NativeScriptNativeApi::install(std::string /*metadataPath*/) noexcept {
  std::lock_guard lock(g_lock);
  if (!g_runtime) return false;
  // Created on the UI thread, before any worklet: the dispatcher runs work in order.
  auto runtime = g_runtime;
  m_context.UIDispatcher().Post([runtime]() { runtime->Start(); });
  g_installed = true;
  return true;
}

bool NativeScriptNativeApi::installWorkletRuntime(React::JSValue /*runtimeHolder*/, std::string /*metadataPath*/) noexcept {
  return false;
}

bool NativeScriptNativeApi::isInstalled() noexcept {
  std::lock_guard lock(g_lock);
  return g_installed;
}

std::string NativeScriptNativeApi::defaultMetadataPath() noexcept {
  return "";
}

std::string NativeScriptNativeApi::getRuntimeBackend() noexcept {
  return "hermes-node-api-ui";
}

bool NativeScriptNativeApi::__writeTestMarker(std::string content) noexcept {
  wchar_t temp[MAX_PATH + 1];
  if (!GetTempPathW(MAX_PATH + 1, temp)) return false;
  std::ofstream file(std::filesystem::path(temp) / L"nativescript-react-native-marker.txt", std::ios::binary);
  file << content;
  return static_cast<bool>(file);
}

void NativeScriptNativeApi::runWorklet(
    std::string code,
    std::string payload,
    React::ReactPromise<std::string> &&result) noexcept {
  std::shared_ptr<NativeScriptRN::UIRuntime> runtime;
  {
    std::lock_guard lock(g_lock);
    runtime = g_runtime;
  }
  if (!runtime) {
    result.Reject("NativeScriptNativeApi isn't initialized");
    return;
  }
  m_context.UIDispatcher().Post([runtime, code = std::move(code), payload = std::move(payload), result]() {
    // Failures inside the worklet resolve with { ok: false }; the JS side throws them.
    runtime->Run(code, payload, [result](std::string json) { result.Resolve(json); });
  });
}

} // namespace winrt::NativeScriptNativeApi
