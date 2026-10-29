import QtQuick
import QtTest
import "../../plugin/contents/ui" as Wallpaper

TestCase {
    name: "PyextPromises"
    Wallpaper.Pyext { id: pyext }

    function test_callbackErrorsReject() {
        let caught = false;
        pyext.readfile("fixture").then(() => { throw new Error("callback failed"); })
            .catch(error => { caught = error.message === "callback failed"; });
        tryVerify(() => caught);
    }

    function test_assimilationAndOrdering() {
        let result = "pending";
        pyext.readfile("fixture").then(() => Promise.resolve("nested"))
            .then(value => { result = value; });
        compare(result, "pending");
        tryCompare({ get result() { return result; } }, "result", "nested");
    }

    function test_nativeErrorsReject() {
        let caught = false;
        pyext.readfile("error").catch(error => { caught = error.message === "read failed"; });
        tryVerify(() => caught);
    }

    function test_atomicReplacementFlag() {
        let done = false;
        pyext.write_wallpaper_config("replace", {volume: 42}, true).then(() => { done = true; });
        tryVerify(() => done);
    }

    function test_failureNotifiesBeforeRejection_data() {
        return [{tag: "write", reset: false}, {tag: "reset", reset: true}];
    }

    function test_failureNotifiesBeforeRejection(data) {
        const helper = findChild(pyext, "fixtureFileHelper");
        verify(helper);
        helper.notifications = [];
        let rejected = false;
        const promise = data.reset ? pyext.reset_wallpaper_config("error")
                                   : pyext.write_wallpaper_config("error", {privateValue: "hidden"});
        promise.catch(() => { rejected = true; });
        compare(helper.notifications.length, 1);
        verify(!helper.notifications[0].includes("hidden"));
        verify(!rejected);
        tryVerify(() => rejected);
    }

    function test_writeFailure_data() {
        return [{tag: "write", reset: false}, {tag: "reset", reset: true}];
    }

    function test_writeFailure(data) {
        let caught = false;
        const promise = data.reset ? pyext.reset_wallpaper_config("error")
                                   : pyext.write_wallpaper_config("error", {});
        promise.catch(error => { caught = error instanceof Error; });
        tryVerify(() => caught);
    }
}
