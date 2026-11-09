#include "Performance.h"

#include <chrono>

#include "js_native_api.h"
#include "mach/mach_time.h"
#include "native_api_util.h"

namespace nativescript {

namespace {
double g_timeOrigin = 0;

double monotonicNowMs() {
  uint64_t time = mach_absolute_time();
  mach_timebase_info_data_t timebase;
  mach_timebase_info(&timebase);
  double nanoseconds =
      (double)time * (double)timebase.numer / (double)timebase.denom;
  return nanoseconds / 1000000.0;
}
}  // namespace

JS_CLASS_INIT(Performance::Init) {
  napi_value Performance, performance;
  const auto systemNow =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  g_timeOrigin = (double)systemNow - monotonicNowMs();

  const napi_property_descriptor properties[] = {
      {
          .utf8name = "now",
          .name = nullptr,
          .method = Now,
          .getter = nullptr,
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_default,
          .data = nullptr,
      },
      {
          .utf8name = "timeOrigin",
          .name = nullptr,
          .method = nullptr,
          .getter = [](napi_env env, napi_callback_info info) -> napi_value {
            napi_value result;
            napi_create_double(env, g_timeOrigin, &result);
            return result;
          },
          .setter = nullptr,
          .value = nullptr,
          .attributes = napi_default,
          .data = nullptr,
      },
  };

  napi_define_class(env, "Performance", NAPI_AUTO_LENGTH,
                    Performance::Constructor, nullptr, 2, properties,
                    &Performance);

  napi_new_instance(env, Performance, 0, nullptr, &performance);
  napi_set_named_property(env, global, "Performance", Performance);
  napi_set_named_property(env, global, "performance", performance);
}

JS_METHOD(Performance::Constructor) {
  napi_value thisArg;
  napi_get_cb_info(env, cbinfo, nullptr, nullptr, &thisArg, nullptr);

  return thisArg;
}

JS_METHOD(Performance::Now) {
  napi_value result;
  napi_create_double(env, monotonicNowMs(), &result);
  return result;
}

}  // namespace nativescript
