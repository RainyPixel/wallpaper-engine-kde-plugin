import QtQuick
import QtTest
import "../../plugin/contents/ui/backend" as Backend
import "../../plugin/contents/ui" as Wallpaper

TestCase {
    id: testCase
    name: "WebProperties"
    when: windowShown
    width: 320
    height: 240
    Item {
        id: background
        property bool hasLib: false
        property int fps: 57
        property bool mute: true
        property string userPropsJson: '{"tint":"0.2 0.4 0.6"}'
        property string nowBackend
        function getWorkshopIDPath() {
            return Wallpaper.Common.getProjectDirectory(activeItem.source, testCase.projectId,
                [String(Qt.resolvedUrl(".")), String(Qt.resolvedUrl("fixtures"))]);
        }
        signal sig_backendFirstFrame(string backend)
    }
    property string projectId: "fixtures"
    property var activeItem
    property string readPath
    property var projectReader

    function init() {
        projectId = "fixtures";
        activeItem = null;
        readPath = "";
        background.fps = 57;
        background.userPropsJson = '{"tint":"0.2 0.4 0.6"}';
        projectReader = path => Promise.resolve('{"general":{"properties":{"tint":{"value":"1 1 1"},"enabled":{"value":true}}}}');
    }

    Component {
        id: webComponent
        Backend.QtWebView {
            source: Qt.resolvedUrl("fixtures/web-properties.html")
            readfile: path => {
                if (path === ":/qtwebchannel/qwebchannel.js") {
                    return new Promise(resolve => {
                        const request = new XMLHttpRequest();
                        request.open("GET", "qrc:///qtwebchannel/qwebchannel.js");
                        request.onreadystatechange = () => {
                            if (request.readyState === XMLHttpRequest.DONE) resolve(request.responseText);
                        };
                        request.send();
                    });
                }
                testCase.readPath = path;
                return testCase.projectReader(path);
            }
        }
    }

    function canonical(value) {
        if (Array.isArray(value)) return value.map(canonical);
        if (!value || typeof value !== "object") return value;
        const result = {};
        Object.keys(value).sort().forEach(key => { result[key] = canonical(value[key]); });
        return result;
    }

    function evaluate(webView, script, expected) {
        let actual;
        tryVerify(() => {
            webView.runJavaScript("JSON.stringify(" + script + ")", result => {
                actual = result ? JSON.parse(result) : undefined;
            });
            return JSON.stringify(canonical(actual)) === JSON.stringify(canonical(expected));
        }, 10000, script + " got " + JSON.stringify(actual));
    }

    function test_propertiesAndReload() {
        const item = createTemporaryObject(webComponent, this);
        verify(item);
        activeItem = item;
        // WebEngineView is the only child exposing runJavaScript.
        const view = item.children.find(child => typeof child.runJavaScript === "function");
        verify(view);
        evaluate(view, "window.receivedGeneral", [{fps: 57}]);
        evaluate(view, "window.receivedUser", [{tint: {value: "0.2 0.4 0.6"}, enabled: {value: true}}]);
        verify(readPath.endsWith("/fixtures/project.json"));
        background.fps = 43;
        evaluate(view, "window.receivedGeneral", [{fps: 57}, {fps: 43}]);
        background.userPropsJson = '{"enabled":false}';
        evaluate(view, "window.receivedUser.slice(-1)", [{tint: {value: "1 1 1"}, enabled: {value: false}}]);
        projectId = "second-library";
        item.source = Qt.resolvedUrl("fixtures/second-library/web/index.html");
        evaluate(view, "window.receivedGeneral", [{fps: 43}]);
        evaluate(view, "window.receivedUser", [{tint: {value: "1 1 1"}, enabled: {value: false}}]);
        verify(readPath.endsWith("/fixtures/second-library/project.json"));
    }
    function test_staleProjectReadIgnored() {
        let resolveFirst;
        background.userPropsJson = "{}";
        projectReader = path => {
            if (path.endsWith("/fixtures/project.json"))
                return new Promise(resolve => { resolveFirst = resolve; });
            return Promise.resolve('{"general":{"properties":{"marker":{"value":"B"}}}}');
        };
        const item = createTemporaryObject(webComponent, this);
        verify(item);
        activeItem = item;
        const view = item.children.find(child => typeof child.runJavaScript === "function");
        verify(view);
        tryVerify(() => typeof resolveFirst === "function");
        projectId = "second-library";
        item.source = Qt.resolvedUrl("fixtures/second-library/web/index.html");
        evaluate(view, "window.receivedUser", [{marker: {value: "B"}}]);
        resolveFirst('{"general":{"properties":{"marker":{"value":"A"}}}}');
        let drained = false;
        Promise.resolve().then(() => { drained = true; });
        tryVerify(() => drained);
        compare(item.projectProperties.marker.value, "B");
        evaluate(view, "window.receivedUser", [{marker: {value: "B"}}]);
    }

}
