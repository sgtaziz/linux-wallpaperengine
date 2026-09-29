#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Scripting/LocalStorageObject.h"

#include <filesystem>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using WallpaperEngine::Scripting::LocalStorageObject;

namespace {
struct StorageFixture {
    JSRuntime* runtime = JS_NewRuntime ();
    JSContext* context = JS_NewContext (runtime);
    std::unique_ptr<LocalStorageObject> storage;

    StorageFixture (const std::filesystem::path& root, const std::string& identity,
                    const std::string& screen) {
        storage = std::make_unique<LocalStorageObject> (context, root, identity, screen);
        JSValue global = JS_GetGlobalObject (context);
        JS_SetPropertyStr (context, global, "localStorage", storage->instance ());
        JS_FreeValue (context, global);
    }
    ~StorageFixture () {
        storage.reset ();
        JS_FreeContext (context);
        JS_FreeRuntime (runtime);
    }
    std::string eval (const char* code) {
        JSValue result = JS_Eval (context, code, std::char_traits<char>::length (code),
                                  "<storage-test>", JS_EVAL_TYPE_GLOBAL);
        REQUIRE_FALSE (JS_IsException (result));
        const char* chars = JS_ToCString (context, result);
        std::string text = chars ? chars : "";
        if (chars) JS_FreeCString (context, chars);
        JS_FreeValue (context, result);
        return text;
    }
};
} // namespace

TEST_CASE ("SceneScript storage persists typed values and separates displays and wallpapers", "[script][storage]") {
    const auto root = std::filesystem::temp_directory_path () /
        ("wpe-storage-unit-" + std::to_string (getpid ())) ;
    std::filesystem::remove_all (root);
    {
        StorageFixture first (root, "wallpaper-a", "screen-one");
        REQUIRE (first.eval (R"(localStorage.set('state', {count: 7, flags: [true, false]});
            localStorage.set('shared', 3.5, localStorage.LOCATION_GLOBAL);
            JSON.stringify(localStorage.get('state')))") == R"({"count":7,"flags":[true,false]})");
        REQUIRE (first.eval (R"(localStorage.set('a' + String.fromCharCode(0) + 'b', 17);
            localStorage.set('a', 23); localStorage.get('a' + String.fromCharCode(0) + 'b'))") == "17");
        REQUIRE (first.eval ("localStorage.get('a')") == "23");
    }
    {
        StorageFixture second (root, "wallpaper-a", "screen-two");
        REQUIRE (second.eval ("localStorage.get('state') === null") == "true");
        REQUIRE (second.eval ("localStorage.get('shared', localStorage.LOCATION_GLOBAL)") == "3.5");
        REQUIRE (second.eval ("localStorage.set('state', 'two'); localStorage.delete('missing')") == "false");
        REQUIRE (second.eval ("localStorage.delete('state')") == "true");
    }
    {
        StorageFixture firstAgain (root, "wallpaper-a", "screen-one");
        REQUIRE (firstAgain.eval ("localStorage.get('state').count") == "7");
        REQUIRE (firstAgain.eval ("localStorage.clear(); localStorage.get('state') === null") == "true");
        REQUIRE (firstAgain.eval ("localStorage.get('shared', localStorage.LOCATION_GLOBAL)") == "3.5");
    }
    {
        StorageFixture other (root, "wallpaper-b", "screen-one");
        REQUIRE (other.eval ("localStorage.get('shared', localStorage.LOCATION_GLOBAL) === null") == "true");
    }
    std::filesystem::remove_all (root);
}

TEST_CASE ("SceneScript storage rejects an oversized update without losing the previous value", "[script][storage]") {
    const auto root = std::filesystem::temp_directory_path () /
        ("wpe-storage-cap-unit-" + std::to_string (getpid ())) ;
    std::filesystem::remove_all (root);
    {
        StorageFixture storage (root, "wallpaper-cap", "screen");
        REQUIRE (storage.eval ("localStorage.set('ok', 42); 1") == "1");
        REQUIRE (storage.eval ("try { localStorage.set('huge', 'x'.repeat(103000)); false } catch(e) { true }") == "true");
        REQUIRE (storage.eval ("localStorage.get('ok')") == "42");
        REQUIRE (storage.eval ("localStorage.get('huge') === null") == "true");
    }
    std::filesystem::remove_all (root);
}

TEST_CASE ("Separate SceneScript processes retain concurrent global keys", "[script][storage]") {
    const auto root = std::filesystem::temp_directory_path () /
        ("wpe-storage-process-unit-" + std::to_string (getpid ())) ;
    std::filesystem::remove_all (root);
    int beginPipe[2];
    REQUIRE (pipe (beginPipe) == 0);
    pid_t children[2] {};
    for (int child = 0; child < 2; ++child) {
        children[child] = fork ();
        REQUIRE (children[child] >= 0);
        if (children[child] == 0) {
            close (beginPipe[1]);
            char start = 0;
            if (read (beginPipe[0], &start, 1) != 1) _exit (2);
            close (beginPipe[0]);
            try {
                StorageFixture js (root, "wallpaper-process", child ? "right" : "left");
                const char* source = child
                    ? "for(let i=0;i<12;i++) localStorage.set('right',i,localStorage.LOCATION_GLOBAL);"
                    : "for(let i=0;i<12;i++) localStorage.set('left',i,localStorage.LOCATION_GLOBAL);";
                JSValue result = JS_Eval (js.context, source, std::char_traits<char>::length (source),
                                          "<storage-process>", JS_EVAL_TYPE_GLOBAL);
                const bool failed = JS_IsException (result);
                JS_FreeValue (js.context, result);
                _exit (failed ? 3 : 0);
            } catch (...) { _exit (4); }
        }
    }
    close (beginPipe[0]);
    REQUIRE (write (beginPipe[1], "XX", 2) == 2);
    close (beginPipe[1]);
    for (pid_t child : children) {
        int status = 0;
        REQUIRE (waitpid (child, &status, 0) == child);
        REQUIRE (WIFEXITED (status));
        REQUIRE (WEXITSTATUS (status) == 0);
    }
    StorageFixture reader (root, "wallpaper-process", "left");
    REQUIRE (reader.eval ("localStorage.get('left',localStorage.LOCATION_GLOBAL)") == "11");
    REQUIRE (reader.eval ("localStorage.get('right',localStorage.LOCATION_GLOBAL)") == "11");
    std::filesystem::remove_all (root);
}
