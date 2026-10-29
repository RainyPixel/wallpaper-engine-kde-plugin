import QtQuick
import QtTest
import "../../plugin/contents/ui/js/wallpaper-config.mjs" as Config

TestCase {
    name: "PendingConfiguration"

    function test_failedReplacementRetainsBothAndAllowsRetry() {
        const changes = {one: {volume: 42}};
        const resets = new Set(["one"]);
        const calls = [];
        let persisted = {oldKey: true, volume: 5};
        let failWrite = true;
        const helper = {
            reset_wallpaper_config(id) {
                calls.push("reset:" + id);
                return Promise.resolve();
            },
            write_wallpaper_config(id, value, replaceExisting) {
                calls.push("write:" + id + ":" + replaceExisting);
                if (failWrite) return Promise.reject(new Error("write failed"));
                persisted = replaceExisting ? value : Object.assign({}, persisted, value);
                return Promise.resolve();
            }
        };
        let rejected = false;
        Config.savePending(helper, changes, resets).catch(() => { rejected = true; });
        tryVerify(() => rejected);
        compare(calls, ["write:one:true"]);
        verify(resets.has("one"));
        compare(changes.one.volume, 42);
        compare(persisted, {oldKey: true, volume: 5});
        failWrite = false;
        let done = false;
        Config.savePending(helper, changes, resets).then(() => { done = true; });
        tryVerify(() => done);
        compare(calls, ["write:one:true", "write:one:true"]);
        compare(persisted, {volume: 42});
        compare(resets.size, 0);
        compare(Object.keys(changes).length, 0);
    }

    function test_resetOnlyFailureRetainsReset() {
        const changes = {};
        const resets = new Set(["one"]);
        let failReset = true;
        const helper = {
            reset_wallpaper_config(id) {
                return failReset ? Promise.reject(new Error("reset failed")) : Promise.resolve();
            }
        };
        let rejected = false;
        Config.savePending(helper, changes, resets).catch(() => { rejected = true; });
        tryVerify(() => rejected);
        verify(resets.has("one"));
        failReset = false;
        let done = false;
        Config.savePending(helper, changes, resets).then(() => { done = true; });
        tryVerify(() => done);
        compare(resets.size, 0);
    }

    function test_newerChangesRetained() {
        const changes = {one: {volume: 42}};
        const resets = new Set();
        let written;
        const helper = {
            write_wallpaper_config(id, value, replaceExisting) {
                verify(!replaceExisting);
                written = value.volume;
                changes.one.volume = 99;
                return Promise.resolve();
            }
        };
        let done = false;
        Config.savePending(helper, changes, resets).then(() => { done = true; });
        compare(written, 42);
        tryVerify(() => done);
        compare(changes.one.volume, 99);
    }
}
