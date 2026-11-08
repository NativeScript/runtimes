package org.nativescript.staticbindinggenerator;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * The build-time half of content-keyed binding names.
 *
 * This must agree byte for byte with MetadataNode::CreateContentKey in the
 * runtime -- the whole scheme rests on both sides deriving the same name from
 * the same class, with no shared coordinate system to fall back on. A change
 * here without the matching change there does not fail the build; it fails on
 * device as LookedUpClassNotFound. ContentKeyTest pins the pairing.
 *
 * Sorted because the runtime reads JS own-property order and this side reads
 * source order, and the generated binding treats both as unordered sets.
 * FNV-1a because it is trivially reproducible in C++; String.hashCode would not
 * survive the trip and java.util.zip checksums are not defined over this input.
 */
public final class ContentKey {

    private static final long FNV_OFFSET_BASIS = 0xcbf29ce484222325L;
    private static final long FNV_PRIME = 0x100000001b3L;

    private ContentKey() {
    }

    public static String of(String baseClassName, String[] interfaceNames, String[] methodNames) {
        String payload = payload(baseClassName, interfaceNames, methodNames);

        long hash = FNV_OFFSET_BASIS;
        for (byte b : payload.getBytes(java.nio.charset.StandardCharsets.UTF_8)) {
            hash ^= (b & 0xffL);
            hash *= FNV_PRIME;
        }

        return String.format("h%016x", hash);
    }

    static String payload(String baseClassName, String[] interfaceNames, String[] methodNames) {
        StringBuilder builder = new StringBuilder();
        builder.append(canonicalize(baseClassName));
        builder.append('|');
        builder.append(String.join(",", sortedNonEmpty(interfaceNames)));
        builder.append('|');
        builder.append(String.join(",", sortedNonEmpty(methodNames)));
        return builder.toString();
    }

    /**
     * Package and nested-type separators both collapse to '.'.
     *
     * This side reads names out of JS source, where a nested type is written
     * with a dot and is indistinguishable from a package; the runtime holds them
     * in binary form, with '/' between packages and '$' before a nested type.
     * The dotted form is therefore the only one both sides can produce. Leaving
     * '$' alone cost every class implementing a nested interface a content key
     * that disagreed with the runtime's, so none of them resolved.
     */
    private static String canonicalize(String name) {
        return name == null ? "" : name.replace('/', '.').replace('$', '.');
    }

    /**
     * An absent field in sbg-bindings.txt splits to a single empty string rather
     * than to nothing, so the empties are dropped here; the runtime never has
     * them to begin with.
     */
    private static List<String> sortedNonEmpty(String[] values) {
        List<String> result = new ArrayList<String>();
        if (values != null) {
            for (String value : values) {
                if (value != null && !value.isEmpty()) {
                    // Canonicalized before sorting, not after: '$' sorts after
                    // '.', so the two sides would otherwise order a nested
                    // interface differently even once the names matched.
                    result.add(canonicalize(value));
                }
            }
        }
        // Ordered by UTF-8 bytes, not by String.compareTo: the runtime sorts
        // std::string, which compares bytes, and UTF-16 order diverges from
        // UTF-8 order above the BMP. Identical for ASCII identifiers, which is
        // every realistic name -- but the two sides have no way to discover a
        // disagreement short of a missing class at runtime.
        Collections.sort(result, new java.util.Comparator<String>() {
            @Override
            public int compare(String left, String right) {
                byte[] a = left.getBytes(java.nio.charset.StandardCharsets.UTF_8);
                byte[] b = right.getBytes(java.nio.charset.StandardCharsets.UTF_8);
                int shared = Math.min(a.length, b.length);
                for (int i = 0; i < shared; i++) {
                    int diff = (a[i] & 0xff) - (b[i] & 0xff);
                    if (diff != 0) {
                        return diff;
                    }
                }
                return a.length - b.length;
            }
        });
        return result;
    }
}
