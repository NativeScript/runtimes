package com.tns.tests;

import android.content.Context;

/**
 * Methods with exactly one overload per name/arity. These are "resolved" by the
 * metadata generator and must be invoked directly with their metadata signature,
 * whatever JS values are passed (null/undefined, JS arrays, JS interface
 * implementations, numbers for boxed/Object parameters).
 */
public class SingleOverloadResolutionFixture {

    // Mirrors com.nativescript.text.Font.stringBuilderFromHtmlString
    public static String htmlLike(Context context, String fontFolder, String parentFontFamily,
                                  String htmlString, final boolean disableLinkUnderline, final int linkColor) {
        return (context != null) + "|" + fontFolder + "|" + parentFontFamily + "|" + htmlString + "|"
                + disableLinkUnderline + "|" + linkColor;
    }

    // Mirrors Activity.requestPermissions(String[], int)
    public String requestLike(String[] permissions, int requestCode) {
        StringBuilder sb = new StringBuilder();
        if (permissions == null) {
            sb.append("null");
        } else {
            for (int i = 0; i < permissions.length; i++) {
                if (i > 0) sb.append(",");
                sb.append(permissions[i]);
            }
        }
        return sb.append("#").append(requestCode).toString();
    }

    public static String intArray(int[] values) {
        int sum = 0;
        for (int v : values) sum += v;
        return "sum=" + sum;
    }

    public static String objectParam(Object value) {
        return value == null ? "null" : value.getClass().getName();
    }

    public static String boxedInt(Integer value) {
        return "boxed=" + value;
    }

    public static String boxedBool(Boolean value) {
        return "bool=" + value;
    }

    public static String charSeq(CharSequence value) {
        return "cs=" + value;
    }

    public static String runIt(Runnable r) {
        r.run();
        return "ran";
    }

    public static String twoStrings(String a, String b) {
        return a + "+" + b;
    }

    // Multi-overload, same arity: null in the first position must not prevent
    // the second argument from discriminating.
    public static String mixed(String a, int b) {
        return "string-int";
    }

    public static String mixed(String a, String b) {
        return "string-string";
    }
}
