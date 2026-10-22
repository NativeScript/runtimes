#!/usr/bin/env python3
"""Add an internal xcframework to the app template's Embed Frameworks phase.

Usage: embed_engine_xcframework.py <project.pbxproj> <Name.xcframework>

The template project embeds NativeScript.xcframework and TKLiveSync.xcframework
by explicit entries. An engine that ships as its own dynamic framework (Hermes)
must be embedded the same way, or dyld cannot find it at app launch. The new
entries are cloned from the TKLiveSync ones so they land in the same group,
build phase and settings; ids are derived from the framework name so re-running
is idempotent.
"""

import hashlib
import re
import sys

TEMPLATE_NAME = "TKLiveSync.xcframework"


def pbx_id(seed):
    return hashlib.md5(seed.encode("utf-8")).hexdigest()[:24].upper()


def main(pbxproj_path, framework_name):
    with open(pbxproj_path) as f:
        lines = f.read().split("\n")

    if any(framework_name in line for line in lines):
        print("{} already referenced in {}".format(framework_name, pbxproj_path))
        return 0

    template_lines = [line for line in lines if TEMPLATE_NAME in line]
    template_ids = sorted(set(re.findall(r"\b([0-9A-F]{24})\b", "\n".join(template_lines))))
    if len(template_ids) != 2:
        print("expected the file reference and build file ids for {}, found {}".format(
            TEMPLATE_NAME, template_ids), file=sys.stderr)
        return 1

    # The build-file entry references the file reference (`fileRef = <id>`), so
    # the id that appears as a fileRef target is the file reference.
    joined = "\n".join(template_lines)
    file_ref_id = next(i for i in template_ids if re.search(r"fileRef = " + i, joined))
    build_file_id = next(i for i in template_ids if i != file_ref_id)
    replacements = {
        file_ref_id: pbx_id(framework_name + ":fileRef"),
        build_file_id: pbx_id(framework_name + ":buildFile"),
        TEMPLATE_NAME: framework_name,
    }

    output = []
    for line in lines:
        output.append(line)
        if TEMPLATE_NAME in line:
            clone = line
            for old, new in replacements.items():
                clone = clone.replace(old, new)
            output.append(clone)

    with open(pbxproj_path, "w") as f:
        f.write("\n".join(output))
    print("embedded {} in {}".format(framework_name, pbxproj_path))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
