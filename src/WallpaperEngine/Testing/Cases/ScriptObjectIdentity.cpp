#include <catch2/catch_test_macros.hpp>
#include "WallpaperEngine/Scripting/Adapters/ObjectInstanceCache.h"

using WallpaperEngine::Scripting::Adapters::ObjectInstanceCache;

TEST_CASE ("Layer identity retains wrappers without reassigning a removed handle", "[script][lifetime]") {
    JSRuntime* runtime = JS_NewRuntime ();
    JSContext* context = JS_NewContext (runtime);
    {
        ObjectInstanceCache cache (context);
        int address = 0;
        JSValue old = JS_NewObject (context);
        JS_SetPropertyStr (context, old, "generation", JS_NewInt32 (context, 1));
        cache.retain (&address, old);
        JSValue lookup = cache.find (&address);
        REQUIRE (JS_VALUE_GET_PTR (lookup) == JS_VALUE_GET_PTR (old));
        JS_FreeValue (context, lookup);
        cache.forget (&address);
        REQUIRE (JS_IsUndefined (cache.find (&address)));
        JSValue replacement = JS_NewObject (context);
        JS_SetPropertyStr (context, replacement, "generation", JS_NewInt32 (context, 2));
        cache.retain (&address, replacement);
        lookup = cache.find (&address);
        REQUIRE (JS_VALUE_GET_PTR (lookup) == JS_VALUE_GET_PTR (replacement));
        REQUIRE (JS_VALUE_GET_PTR (lookup) != JS_VALUE_GET_PTR (old));
        JS_FreeValue (context, lookup);
        JSValue generation = JS_GetPropertyStr (context, old, "generation");
        int value = 0;
        REQUIRE (JS_ToInt32 (context, &value, generation) == 0);
        REQUIRE (value == 1);
        JS_FreeValue (context, generation);
        JS_FreeValue (context, old);
        JS_FreeValue (context, replacement);
    }
    JS_FreeContext (context);
    JS_FreeRuntime (runtime);
}

namespace {
void countFinalization (JSRuntime*, JSValueConst object) {
    auto* count = static_cast<int*> (JS_GetOpaque (object, JS_GetClassID (object)));
    if (count) ++*count;
}
}

TEST_CASE ("Layer cache releases handles before context teardown including retained JS cycles", "[script][lifetime]") {
    int finalized = 0;
    JSRuntime* runtime = JS_NewRuntime ();
    JSContext* context = JS_NewContext (runtime);
    JSClassID classId = 0;
    JS_NewClassID (runtime, &classId);
    JSClassDef definition {.class_name = "IdentityLifecycle", .finalizer = countFinalization};
    REQUIRE (JS_NewClass (runtime, classId, &definition) == 0);
    {
        ObjectInstanceCache cache (context);
        int first = 0, second = 0;
        JSValue object = JS_NewObjectClass (context, classId);
        JS_SetOpaque (object, &finalized);
        cache.retain (&first, object);
        JS_FreeValue (context, object);
        JS_RunGC (runtime);
        REQUIRE (finalized == 0);
        cache.forget (&first);
        REQUIRE (finalized == 1);
        object = JS_NewObjectClass (context, classId);
        JS_SetOpaque (object, &finalized);
        JS_SetPropertyStr (context, object, "self", JS_DupValue (context, object));
        cache.retain (&second, object);
        JS_FreeValue (context, object);
        cache.clear ();
        JS_RunGC (runtime);
        REQUIRE (finalized == 2);
    }
    JS_FreeContext (context);
    JS_FreeRuntime (runtime);
    REQUIRE (finalized == 2);
}
