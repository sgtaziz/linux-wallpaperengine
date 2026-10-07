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
    bool topLevel = false;

    StorageFixture (const std::filesystem::path& root, const std::string& identity,
                    const std::string& screen) {
        storage = std::make_unique<LocalStorageObject> (context, root, identity, screen,
                                                        [this] { return topLevel; });
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
    // The message of a SyntaxError thrown by code, or "" when nothing throws.
    std::string error (const char* code) {
        const std::string wrapped = std::string ("try { ") + code +
            "; '' } catch (e) { (e instanceof SyntaxError && e.name === 'SyntaxError') ? e.message : 'not a SyntaxError' }";
        return eval (wrapped.c_str ());
    }
};

std::filesystem::path freshRoot (const char* name) {
    const auto root = std::filesystem::temp_directory_path () /
        (std::string (name) + "-" + std::to_string (getpid ()));
    std::filesystem::remove_all (root);
    return root;
}
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
        // Native reads keys as C strings, so 'a\0b' names the same entry as 'a'.
        REQUIRE (first.eval (R"(localStorage.set('a' + String.fromCharCode(0) + 'b', 17);
            localStorage.get('a'))") == "17");
        REQUIRE (first.eval (R"(localStorage.set('a', 23); localStorage.get('a' + String.fromCharCode(0) + 'b'))") == "23");
    }
    {
        StorageFixture second (root, "wallpaper-a", "screen-two");
        REQUIRE (second.eval ("localStorage.get('state') === undefined") == "true");
        REQUIRE (second.eval ("typeof localStorage.get('state')") == "undefined");
        REQUIRE (second.eval ("localStorage.get('shared', localStorage.LOCATION_GLOBAL)") == "3.5");
        REQUIRE (second.eval ("localStorage.set('state', 'two'); localStorage.delete('missing')") == "false");
        REQUIRE (second.eval ("localStorage.delete('state')") == "true");
    }
    {
        StorageFixture firstAgain (root, "wallpaper-a", "screen-one");
        REQUIRE (firstAgain.eval ("localStorage.get('state').count") == "7");
        REQUIRE (firstAgain.eval ("localStorage.clear() === true && localStorage.get('state') === undefined") == "true");
        REQUIRE (firstAgain.eval ("localStorage.get('shared', localStorage.LOCATION_GLOBAL)") == "3.5");
    }
    {
        StorageFixture other (root, "wallpaper-b", "screen-one");
        REQUIRE (other.eval ("localStorage.get('shared', localStorage.LOCATION_GLOBAL) === undefined") == "true");
    }
    std::filesystem::remove_all (root);
}

TEST_CASE ("SceneScript storage applies native per-namespace accounting", "[script][storage]") {
    const auto root = freshRoot ("wpe-storage-cap-unit");
    {
        StorageFixture storage (root, "wallpaper-cap", "screen-one");
        REQUIRE (storage.eval ("localStorage.set('ok', 42); localStorage.clear()") == "true");
        // 8 + 99992 bytes of '"x...x"' fills the namespace exactly.
        REQUIRE (storage.error ("localStorage.set('big', 'x'.repeat(99990))").empty ());
        REQUIRE (storage.error ("localStorage.set('y', 1)") == "LocalStorageSet failed, possibly out of memory.");
        REQUIRE (storage.eval ("localStorage.get('y') === undefined") == "true");
        // Replacing an entry does not count its previous text.
        REQUIRE (storage.error ("localStorage.set('big', 'y'.repeat(99990))").empty ());
        REQUIRE (storage.error ("localStorage.set('big', 'y'.repeat(99991))") ==
                 "LocalStorageSet failed, possibly out of memory.");
        REQUIRE (storage.eval ("localStorage.get('big')[0] + localStorage.get('big').length") == "y99990");
        // The global namespace and other screens hold their own ~99 KB.
        REQUIRE (storage.error ("localStorage.set('big', 'g'.repeat(99990), localStorage.LOCATION_GLOBAL)").empty ());
    }
    {
        StorageFixture other (root, "wallpaper-cap", "screen-two");
        REQUIRE (other.error ("localStorage.set('big', 'z'.repeat(99990))").empty ());
        REQUIRE (other.error ("localStorage.set('y', 1, localStorage.LOCATION_GLOBAL)") ==
                 "LocalStorageSet failed, possibly out of memory.");
        REQUIRE (other.eval ("localStorage.get('big', localStorage.LOCATION_GLOBAL).length") == "99990");
    }
    {
        StorageFixture again (root, "wallpaper-cap", "screen-one");
        REQUIRE (again.eval ("localStorage.get('big')[0] + localStorage.get('big').length") == "y99990");
        // Stored entries count by their JSON.stringify text: 1e20 is 21 bytes
        // ("100000000000000000000"), so 8 + 21 + 8 + (99961 + 2) = 100000.
        REQUIRE (again.eval ("localStorage.clear(); localStorage.set('n', 1e20); localStorage.get('n') === 1e20") == "true");
        REQUIRE (again.error ("localStorage.set('big', 'x'.repeat(99962))") ==
                 "LocalStorageSet failed, possibly out of memory.");
        REQUIRE (again.error ("localStorage.set('big', 'x'.repeat(99961))").empty ());
        // A value JSON.stringify cannot encode keeps native's 9-byte text "undefined".
        REQUIRE (again.eval ("localStorage.clear(); localStorage.set('f', () => 1); localStorage.get('f') === null") == "true");
        REQUIRE (again.eval ("localStorage.clear('global'); localStorage.set('g', () => 1, 'global'); localStorage.set('g', 1, 'global')") == "undefined");
    }
    {
        // Reopened: 8 + 9 + 8 + (99973 + 2) = 100000.
        StorageFixture reopened (root, "wallpaper-cap", "screen-one");
        REQUIRE (reopened.error ("localStorage.set('big', 'x'.repeat(99974))") ==
                 "LocalStorageSet failed, possibly out of memory.");
        REQUIRE (reopened.error ("localStorage.set('big', 'x'.repeat(99973))").empty ());
        // A real null is 4 bytes: 8 + 4 + 8 + (99978 + 2) = 100000.
        REQUIRE (reopened.eval ("localStorage.delete('big') && localStorage.set('f', null) === undefined") == "true");
        REQUIRE (reopened.error ("localStorage.set('big', 'x'.repeat(99979))") ==
                 "LocalStorageSet failed, possibly out of memory.");
        REQUIRE (reopened.error ("localStorage.set('big', 'x'.repeat(99978))").empty ());
        // A global value replacing undefined text counts as itself: 8 + 1 + 8 + (99981 + 2).
        REQUIRE (reopened.error ("localStorage.set('big', 'x'.repeat(99981), 'global')").empty ());
        // A failed JSON.stringify stores empty text.
        REQUIRE (reopened.eval ("localStorage.clear(); const loop = {}; loop.loop = loop; "
                                "localStorage.set('cyc', loop) === undefined") == "true");
    }
    {
        // Reopened: 8 + 0 + 8 + (99982 + 2) = 100000.
        StorageFixture cyclic (root, "wallpaper-cap", "screen-one");
        REQUIRE (cyclic.error ("localStorage.set('big', 'x'.repeat(99983))") ==
                 "LocalStorageSet failed, possibly out of memory.");
        REQUIRE (cyclic.error ("localStorage.set('big', 'x'.repeat(99982))").empty ());
        REQUIRE (cyclic.eval ("localStorage.get('cyc') === null") == "true");
    }
    std::filesystem::remove_all (root);
}

TEST_CASE ("SceneScript storage follows native argument edges", "[script][storage]") {
    const auto root = freshRoot ("wpe-storage-args-unit");
    {
        StorageFixture js (root, "wallpaper-args", "screen");
        const char* keyError = "LocalStorageSet key not a string.";
        REQUIRE (js.error ("localStorage.set(5, 1)") == keyError);
        REQUIRE (js.error ("localStorage.get(5)") == keyError);
        REQUIRE (js.error ("localStorage.delete({})") == keyError);
        REQUIRE (js.error ("localStorage.get()") == keyError);
        REQUIRE (js.error ("localStorage.set(new String('k'), 1)") == keyError);

        // Only the string "global" selects the global namespace.
        REQUIRE (js.eval ("localStorage.set('loc', 1, 'bogus'); localStorage.set('loc', 2, 5); localStorage.get('loc')") == "2");
        REQUIRE (js.eval ("localStorage.get('loc', localStorage.LOCATION_GLOBAL) === undefined") == "true");
        REQUIRE (js.eval ("localStorage.get('loc', new String('global'))") == "2");
        REQUIRE (js.eval ("localStorage.set('loc', 3, 'global' + String.fromCharCode(0) + 'x'); localStorage.get('loc', 'global')") == "3");
        REQUIRE (js.eval ("localStorage.get('loc', 'screen')") == "2");

        // An undefined value deletes the screen key whatever the location.
        REQUIRE (js.eval ("localStorage.set('k', 1); localStorage.set('k', 2, 'global'); "
                          "localStorage.set('k', undefined, localStorage.LOCATION_GLOBAL)") == "true");
        REQUIRE (js.eval ("localStorage.get('k') === undefined && localStorage.get('k', 'global') === 2") == "true");
        REQUIRE (js.eval ("localStorage.set('k')") == "false");
        // A value JSON.stringify cannot encode is stored and read back as null.
        REQUIRE (js.error ("localStorage.set('f', () => 1)").empty ());
        REQUIRE (js.eval ("localStorage.set('f', () => 1) === undefined && localStorage.get('f') === null") == "true");
        // A stringify exception is swallowed; native 2.8.42 stores empty text
        // in the requested namespace, which reads back as null.
        REQUIRE (js.eval ("localStorage.set('c', 1); const loop = {}; loop.loop = loop; "
                          "localStorage.set('c', loop) === undefined && localStorage.get('c') === null") == "true");
        REQUIRE (js.eval ("localStorage.set('c', loop, 'global') === undefined && "
                          "localStorage.get('c', 'global') === null") == "true");

        // Stored text keeps object key order and drops class identity.
        REQUIRE (js.eval ("localStorage.set('o', {b: 1, a: 2})") == "undefined");
        REQUIRE (js.eval (R"(class Vec3 { constructor (x, y, z) { this.x = x; this.y = y; this.z = z; } }
            localStorage.set('v', new Vec3 (1.5, -2.25, 3)))") == "undefined");
    }
    {
        StorageFixture reopened (root, "wallpaper-args", "screen");
        REQUIRE (reopened.eval ("Object.keys(localStorage.get('o')).join()") == "b,a");
        REQUIRE (reopened.eval (R"(const v = localStorage.get('v');
            Object.getPrototypeOf(v) === Object.prototype && v.x === 1.5 && v.y === -2.25 && v.z === 3)") == "true");
    }
    std::filesystem::remove_all (root);
}

TEST_CASE ("SceneScript storage shares one namespace in window mode", "[script][storage]") {
    const auto root = freshRoot ("wpe-storage-window-unit");
    {
        // Window modes use an empty screen key: native names no screen instance.
        StorageFixture window (root, "wallpaper-window", "");
        REQUIRE (window.eval ("localStorage.set('g', 7, localStorage.LOCATION_GLOBAL); localStorage.get('g')") == "7");
        REQUIRE (window.eval ("localStorage.set('s', 1); localStorage.get('s', localStorage.LOCATION_GLOBAL)") == "1");
        REQUIRE (window.eval ("localStorage.clear(localStorage.LOCATION_GLOBAL) && localStorage.get('g') === undefined "
                              "&& localStorage.get('s') === undefined") == "true");
        // One capacity: 8 + 99992 fills it for both locations.
        REQUIRE (window.error ("localStorage.set('big', 'x'.repeat(99990), localStorage.LOCATION_GLOBAL)").empty ());
        REQUIRE (window.error ("localStorage.set('y', 1)") == "LocalStorageSet failed, possibly out of memory.");
        REQUIRE (window.eval ("localStorage.clear() && localStorage.set('w', 3) === undefined") == "true");
    }
    {
        // A desktop screen keeps its own namespace beside the shared global one.
        StorageFixture desktop (root, "wallpaper-window", "screen-a");
        REQUIRE (desktop.eval ("localStorage.get('w') === undefined && localStorage.get('w', 'global') === 3") == "true");
        REQUIRE (desktop.eval ("localStorage.set('g', 7, 'global'); localStorage.get('g') === undefined") == "true");
        REQUIRE (desktop.eval ("localStorage.set('s', 1); localStorage.get('s', 'global') === undefined") == "true");
        // Separate capacities: global holds w and g (2 x 9), the screen holds s (9).
        REQUIRE (desktop.error ("localStorage.set('big', 'x'.repeat(99972), 'global')").empty ());
        REQUIRE (desktop.error ("localStorage.set('big', 'x'.repeat(99981))").empty ());
    }
    std::filesystem::remove_all (root);
}

TEST_CASE ("SceneScript storage rejects module top-level calls", "[script][storage]") {
    const auto root = freshRoot ("wpe-storage-top-unit");
    {
        StorageFixture js (root, "wallpaper-top", "screen");
        REQUIRE (js.eval ("localStorage.set('kept', 1); localStorage.get('kept')") == "1");
        js.topLevel = true;
        REQUIRE (js.error ("localStorage.get('kept')") == "LocalStorageGet cannot be cleared from global scope.");
        REQUIRE (js.error ("localStorage.set('kept', 2)") == "LocalStorageSet cannot be cleared from global scope.");
        REQUIRE (js.error ("localStorage.delete('kept')") == "LocalStorageDelete cannot be cleared from global scope.");
        REQUIRE (js.error ("localStorage.clear()") == "LocalStorageClear cannot be cleared from global scope.");
        REQUIRE (js.error ("localStorage.set(5)") == "LocalStorageSet cannot be cleared from global scope.");
        js.topLevel = false;
        REQUIRE (js.eval ("localStorage.get('kept')") == "1");
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
