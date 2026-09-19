package com.tns.tests;

public class ReturnTypeResolutionTest {
    public String stringValue() {
        return "string-result";
    }

    public Object objectValue() {
        return new String("object-result");
    }

    public int intValue() {
        return 42;
    }

    public long longValue() {
        return 42000000000L;
    }

    public double doubleValue() {
        return 4.25d;
    }

    public boolean booleanValue() {
        return true;
    }

    public byte byteValue() {
        return 7;
    }

    public short shortValue() {
        return 8;
    }

    public char charValue() {
        return 'A';
    }

    public float floatValue() {
        return 2.5f;
    }

    public int[] intArrayValue() {
        return new int[] { 1, 2, 3 };
    }

    public void voidValue() {
    }

    public String overloaded(int value) {
        return "int-overload";
    }

    public int overloaded(String value) {
        return 7;
    }
}
