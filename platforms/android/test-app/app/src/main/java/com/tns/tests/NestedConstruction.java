package com.tns.tests;

public class NestedConstruction {
    private final Object created;

    public NestedConstruction(Class<?> toCreate, Runnable during) throws Exception {
        created = toCreate == null ? null : toCreate.newInstance();
        if (during != null) {
            during.run();
        }
    }

    public Object getCreated() {
        return created;
    }
}
