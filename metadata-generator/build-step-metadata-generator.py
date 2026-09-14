#!/usr/bin/env python3

import os
import shlex
import subprocess
import sys

print("Python version: " + sys.version)

def str2bool(v):
  return v.lower() in ("yes", "y", "true", "t", "1")

def env(env_name):
    return os.environ[env_name]


def env_or_none(env_name):
    return os.environ.get(env_name)


def env_or_empty(env_name):
    result = env_or_none(env_name)
    if result is None:
        return ""
    return result

def env_bool(env_name):
    return str2bool(env_or_empty(env_name))

def map_and_list(func, iterable):
    result = map(func, iterable)
    if sys.version_info[0] >= 3:
        return list(result)
    return result


# process environment variables
# Xcode may omit EFFECTIVE_PLATFORM_NAME for some macOS builds.
# Fall back to PLATFORM_NAME to preserve behavior across targets.
effective_platform_name = env_or_empty("EFFECTIVE_PLATFORM_NAME")
if not effective_platform_name:
    platform_name = env_or_empty("PLATFORM_NAME")
    if platform_name:
        effective_platform_name = "-{}".format(platform_name)
docset_platform = "iOS"
default_deployment_target_flag_name = "-mios-simulator-version-min"
default_deployment_target_clang_env_name = "IPHONEOS_DEPLOYMENT_TARGET"
if effective_platform_name == "-macosx":
    docset_platform = "OSX"
    default_deployment_target_flag_name = "-mmacosx-version-min"
    default_deployment_target_clang_env_name = "MACOSX_DEPLOYMENT_TARGET"
elif effective_platform_name == "-maccatalyst":
    docset_platform = "maccatalyst"
    default_deployment_target_flag_name = "-mmaccatalyst-version-min"
    default_deployment_target_clang_env_name = "MACCATALYST_DEPLOYMENT_TARGET"
elif effective_platform_name == "-xrsimulator":
    docset_platform = "xros"
    default_deployment_target_flag_name = "-mxrossimulator-version-min"
    default_deployment_target_clang_env_name = "XROS_DEPLOYMENT_TARGET"
elif effective_platform_name == "-xros":
    docset_platform = "xros"
    default_deployment_target_flag_name = "-mxros-version-min"
    default_deployment_target_clang_env_name = "XROS_DEPLOYMENT_TARGET"
elif effective_platform_name == "-watchos":
    docset_platform = "watchOS"
    default_deployment_target_flag_name = "-mwatchos-version-min"
    default_deployment_target_clang_env_name = "WATCHOS_DEPLOYMENT_TARGET"
elif effective_platform_name == "-watchsimulator":
    docset_platform = "watchOS"
    default_deployment_target_flag_name = "-mwatchos-simulator-version-min"
    default_deployment_target_clang_env_name = "WATCHOS_DEPLOYMENT_TARGET"
elif effective_platform_name == "-appletvos":
    docset_platform = "tvOS"
    default_deployment_target_flag_name = "-mappletvos-version-min"
    default_deployment_target_clang_env_name = "APPLETVOS_DEPLOYMENT_TARGET"
elif effective_platform_name == "-appletvsimulator":
    docset_platform = "tvOS"
    default_deployment_target_flag_name = "-mappletvsimulator-version-min"
    default_deployment_target_clang_env_name = "APPLETVOS_DEPLOYMENT_TARGET"
elif effective_platform_name == "-iphoneos":
    default_deployment_target_flag_name = "-miphoneos-version-min"

sdk_version = env("SDK_VERSION") or "13.0"
llvm_target_triple_suffix = env_or_empty("LLVM_TARGET_TRIPLE_SUFFIX")
llvm_target_triple_os_name = "ios"
if effective_platform_name == "-macosx":
    llvm_target_triple_os_name = "macosx"
elif effective_platform_name == "-xrsimulator" or effective_platform_name == "-xros":
    llvm_target_triple_os_name = "xros"
elif effective_platform_name == "-watchos" or effective_platform_name == "-watchsimulator":
    llvm_target_triple_os_name = "watchos"
elif effective_platform_name == "-appletvos" or effective_platform_name == "-appletvsimulator":
    llvm_target_triple_os_name = "tvos"
llvm_target_triple_os_version = "{}{}".format(llvm_target_triple_os_name, sdk_version)
# env("LLVM_TARGET_TRIPLE_OS_VERSION") is the deployment target, so doesn't have all APIs
# usually it's ios9.0 for NativeScript projects
llvm_target_triple_vendor = env("LLVM_TARGET_TRIPLE_VENDOR") or "apple"

conf_build_dir = env("CONFIGURATION_BUILD_DIR")
sdk_root = env("SDKROOT")
src_root = env("SRCROOT")
deployment_target_flag_name = env_or_empty("DEPLOYMENT_TARGET_CLANG_FLAG_NAME") or default_deployment_target_flag_name
deployment_target = env_or_empty(env_or_empty("DEPLOYMENT_TARGET_CLANG_ENV_NAME") or default_deployment_target_clang_env_name) or sdk_version
std = env("GCC_C_LANGUAGE_STANDARD")
header_search_paths = env_or_empty("HEADER_SEARCH_PATHS")
framework_search_paths = env_or_empty("FRAMEWORK_SEARCH_PATHS")
header_search_paths_list = shlex.split(header_search_paths)
framework_search_paths_list = shlex.split(framework_search_paths)

if effective_platform_name == "-macosx":
    sdk_usr_include = os.path.normpath(os.path.join(sdk_root, "usr/include"))
    filtered_header_search_paths = []
    for search_path in header_search_paths_list:
        normalized = os.path.normpath(search_path)
        if normalized == sdk_usr_include:
            print(
                "Skipping SDK usr/include header search path on macOS: {}".format(
                    search_path
                )
            )
            continue
        filtered_header_search_paths.append(search_path)
    header_search_paths_list = filtered_header_search_paths


def has_nativescript_framework(search_paths):
    for search_path in search_paths:
        if os.path.isdir(os.path.join(search_path, "NativeScript.framework")):
            return True
    return False


def is_nativescript_source_root(search_path):
    normalized = os.path.normpath(search_path)
    srcroot_nativescript = os.path.normpath(os.path.join(src_root, "NativeScript"))

    if normalized == srcroot_nativescript:
        return True

    return (
        os.path.isfile(os.path.join(normalized, "NativeScript.h"))
        and os.path.isdir(os.path.join(normalized, "runtime"))
    )


if has_nativescript_framework(framework_search_paths_list):
    filtered_header_search_paths = []
    for search_path in header_search_paths_list:
        if is_nativescript_source_root(search_path):
            print("Skipping NativeScript source header search path: {}".format(search_path))
            continue
        filtered_header_search_paths.append(search_path)
    header_search_paths_list = filtered_header_search_paths

header_search_paths_parsed = map_and_list((lambda s: "-I" + s), header_search_paths_list)
framework_search_paths_parsed = map_and_list((lambda s: "-F" + s), framework_search_paths_list)
other_cflags = env_or_empty("OTHER_CFLAGS")
other_cflags_parsed = shlex.split(other_cflags)
enable_modules = env_bool("CLANG_ENABLE_MODULES")
if effective_platform_name == "-macosx" and enable_modules:
    # macOS SDK module maps pull in DriverKit C++ headers during metadata
    # umbrella discovery, which breaks Objective-C parsing.
    enable_modules = False
    print("Disabling clang modules for macOS metadata generation.")
preprocessor_defs = env_or_empty("GCC_PREPROCESSOR_DEFINITIONS")
preprocessor_defs_parsed = map_and_list((lambda s: "-D" + s), shlex.split(preprocessor_defs, '\''))
typescript_output_folder = env_or_none("NS_TYPESCRIPT_DECLARATIONS_PATH") or env_or_none("TNS_TYPESCRIPT_DECLARATIONS_PATH")
docset_path = os.path.join(os.path.expanduser("~"),
                           "Library/Developer/Shared/Documentation/DocSets/com.apple.adc.documentation.{}.docset"
                           .format(docset_platform))
yaml_output_folder = env_or_none("NS_DEBUG_METADATA_PATH") or env_or_none("TNS_DEBUG_METADATA_PATH")
strict_includes = env_or_none("NS_DEBUG_METADATA_STRICT_INCLUDES") or env_or_none("TNS_DEBUG_METADATA_STRICT_INCLUDES")
signature_bindings_cpp_path = env_or_none("NS_SIGNATURE_BINDINGS_CPP_PATH") or env_or_none("TNS_SIGNATURE_BINDINGS_CPP_PATH")
if signature_bindings_cpp_path is None:
    default_signature_bindings_path = os.path.join(src_root, "NativeScript", "ffi", "objc", "shared", "GeneratedSignatureDispatch.inc")
    if os.path.isdir(os.path.dirname(default_signature_bindings_path)):
        signature_bindings_cpp_path = default_signature_bindings_path
auto_filter_enabled = env_bool("NS_METADATA_AUTO_FILTER") or env_bool("TNS_METADATA_AUTO_FILTER")
auto_filter_bundle_paths = env_or_none("NS_METADATA_BUNDLE_PATHS") or env_or_none("TNS_METADATA_BUNDLE_PATHS")
symbol_analyzer_path = env_or_none("NS_METADATA_SYMBOL_ANALYZER") or env_or_none("TNS_METADATA_SYMBOL_ANALYZER")
if symbol_analyzer_path is None:
    symbol_analyzer_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ns-metadata-symbols")


def save_stream_to_file(filename, stream):
    f = open(filename, "w")
    f.write(stream)
    f.close()


# noinspection PyShadowingNames
def generate_metadata(arch):
    # metadata generator arguments
    generator_call = ["./objc-metadata-generator", "-verbose",
                      "-output-bin", "{}/metadata-{}.bin".format(conf_build_dir, arch),
                      "-output-umbrella", "{}/umbrella-{}.h".format(conf_build_dir, arch),
                      "-docset-path", docset_path]

    if strict_includes is not None:
        generator_call.extend(["-strict-includes={}".format(strict_includes)])

    # optionally add typescript output folder
    if typescript_output_folder is not None:
        current_typescript_output_folder = os.path.join(typescript_output_folder, arch)
        os.makedirs(current_typescript_output_folder, exist_ok=True)
        generator_call.extend(["-output-typescript", current_typescript_output_folder])
        print("Generating TypeScript declarations in: \"{}\"".format(current_typescript_output_folder))

    # optionally add yaml output folder
    current_yaml_output_folder = None
    if yaml_output_folder is not None:
        current_yaml_output_folder = yaml_output_folder + "-" + arch
        generator_call.extend(["-output-yaml", current_yaml_output_folder])
        print("Generating debug metadata in: \"{}\"".format(current_yaml_output_folder))

    if signature_bindings_cpp_path is not None:
        generator_call.extend(["-output-signature-bindings-cpp", signature_bindings_cpp_path])
        print("Generating signature dispatch bindings in: \"{}\"".format(signature_bindings_cpp_path))

    whitelist_file_name = os.path.join(src_root, "whitelist.mdg")
    effective_whitelist_file_name = whitelist_file_name if os.path.exists(whitelist_file_name) else None
    if auto_filter_enabled:
      if not auto_filter_bundle_paths:
        raise RuntimeError("NS_METADATA_AUTO_FILTER requires NS_METADATA_BUNDLE_PATHS")
      if not os.path.isfile(symbol_analyzer_path) or not os.access(symbol_analyzer_path, os.X_OK):
        raise RuntimeError("Metadata symbol analyzer is not executable: {}".format(symbol_analyzer_path))

      effective_whitelist_file_name = os.path.join(
          conf_build_dir, "metadata-auto-whitelist-{}.mdg".format(arch))
      analyzer_call = [symbol_analyzer_path, "--output", effective_whitelist_file_name]
      if os.path.exists(whitelist_file_name):
        analyzer_call.extend(["--include-whitelist", whitelist_file_name])
      analyzer_call.extend(shlex.split(auto_filter_bundle_paths))
      print("Generating metadata whitelist from app bundle(s):")
      print(" ".join(analyzer_call))
      subprocess.check_call(analyzer_call)

    if effective_whitelist_file_name is not None:
      generator_call.extend(["--whitelist-modules-file", effective_whitelist_file_name])

    blacklist_file_name = os.path.join(src_root, "blacklist.mdg")
    if os.path.exists(blacklist_file_name):
      generator_call.extend(["--blacklist-modules-file", blacklist_file_name])

    # clang arguments
    generator_call.extend(["Xclang",
                           "-isysroot", sdk_root,
                           "-std=" + std])

    if env_or_empty("IS_UIKITFORMAC").capitalize() == "YES":
      generator_call.extend(["-arch", arch,
                             deployment_target_flag_name + "=" + deployment_target])
    else:
      generator_call.extend(["-target", "{}-{}-{}{}".format(arch, llvm_target_triple_vendor, llvm_target_triple_os_version, llvm_target_triple_suffix)])

    generator_call.extend(header_search_paths_parsed)  # HEADER_SEARCH_PATHS
    generator_call.extend(framework_search_paths_parsed)  # FRAMEWORK_SEARCH_PATHS
    generator_call.extend(other_cflags_parsed)  # OTHER_CFLAGS
    generator_call.extend(preprocessor_defs_parsed)  # GCC_PREPROCESSOR_DEFINITIONS

    if enable_modules:
        # -I. is needed for includes coming from clang's lib/clang/<version>/include/ directory when parsing modules
        generator_call.extend(["-I.", "-fmodules"])

    print("Calling metadata generator with: ")
    print(" ".join(generator_call))

    child_process = subprocess.Popen(
        generator_call,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True
    )
    sys.stdout.flush()
    output_stream_content, error_stream_content = child_process.communicate()

    # save stdout stream content to file
    output_log_file = "{}/metadata-generation-stdout-{}.txt".format(conf_build_dir, arch)
    print("Saving metadata generation's stdout stream to: {}".format(output_log_file))
    save_stream_to_file(output_log_file, output_stream_content)

    # save error stream content to file
    error_log_file = "{}/metadata-generation-stderr-{}.txt".format(conf_build_dir, arch)
    print("Saving metadata generation's stderr stream to: {}".format(error_log_file))
    save_stream_to_file(error_log_file, error_stream_content)
    if current_yaml_output_folder is not None:
      yaml_dir_error_log_file = "{}/metadata-generation-stderr.txt".format(current_yaml_output_folder)
      print("Copying metadata stderr stream to: {}".format(yaml_dir_error_log_file))
      save_stream_to_file(yaml_dir_error_log_file, error_stream_content)

    if child_process.returncode != 0:
        # macOS TestRunner currently relies on checked-in metadata fallback.
        # Keep generator failures non-fatal on macOS while preserving strict
        # behavior on the other platforms.
        if effective_platform_name == "-macosx":
            print(
                "Warning: metadata generation failed for {} (macOS fallback metadata will be used).".format(
                    arch
                )
            )
            return

        print("Error: Unable to generate metadata for {}.".format(arch))
        print(output_stream_content)
        print(error_stream_content)
        sys.exit(1)


for arch in env("ARCHS").split():
    # skip metadata generation for architectures different than the one specified in the command line
    # in case the command line argument is not specified, generate metadata for all architectures
    if len(sys.argv) >= 2 and sys.argv[1].lower() != arch.lower():
        print("Skipping metadata generation for " + arch)
        continue

    print("Generating metadata for " + arch)
    generate_metadata(arch)
