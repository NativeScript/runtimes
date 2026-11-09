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
    }

    public static String pick(Object value) {
        return "object";
    }

    public static String pick(HttpUrl value) {
        return "http-url";
    }

    public static String text(Object value) {
        return "object";
    }

    public static String marker(Object value) {
        return "object";
    }

    public static String numeric(double value) {
        return "double";
    }

    public static String array(Object[] value) {
        return "object-array";
    }
}
