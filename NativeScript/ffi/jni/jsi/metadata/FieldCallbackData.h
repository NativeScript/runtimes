#ifndef FIELDCALLBACKDATA_H_
#define FIELDCALLBACKDATA_H_

#include "jni.h"
#include "Engine.h"
#include "MetadataEntry.h"

namespace tns {
    class ObjectManager;

    struct FieldCallbackData {
        FieldCallbackData(MetadataEntry metadata)
                :
                metadata(metadata), fid(nullptr), clazz(nullptr) {

        }

        MetadataEntry metadata;
        jfieldID fid;
        jclass clazz;
        // Cached per-runtime ObjectManager (this data is created per runtime, so
        // the pointer's lifetime matches it) — avoids a locked runtime lookup.
        tns::ObjectManager *objectManager = nullptr;
    };

}

#endif /* FIELDCALLBACKDATA_H_ */
