package com.tns.tests;

public final class OverloadResolutionFixture {
    private OverloadResolutionFixture() {
    }

    public static final class HttpUrl {
    }

    public interface Marker {
    }

    public static final class MarkerImpl implements Marker {
    }

    public static final class Builder {
        public String url(HttpUrl value) {
            return "http-url";
        }

        public String url(java.net.URL value) {
            return "java-url";
        }

        public String url(String value) {
            return "string";
        }
    }

    public static String pick(Object value) {
        return "object";
    }

    public static String pick(HttpUrl value) {
        return "http-url";
    }

    public static String pick(String value) {
        return "string";
    }

    public static String text(Object value) {
        return "object";
    }

    public static String text(CharSequence value) {
        return "char-sequence";
    }

    public static String text(String value) {
        return "string";
    }

    public static String marker(Object value) {
        return "object";
    }

    public static String marker(Marker value) {
        return "marker";
    }

    public static String numeric(int value) {
        return "int";
    }

    public static String numeric(long value) {
        return "long";
    }

    public static String numeric(double value) {
        return "double";
    }

    public static String array(Object[] value) {
        return "object-array";
    }

    public static String array(String[] value) {
        return "string-array";
    }
}
