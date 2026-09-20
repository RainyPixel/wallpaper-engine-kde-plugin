import QtQuick
import QtWebEngine

Item {
    id: root

    required property var host
    required property var surface

    readonly property int errorAborted: -3

    property bool everLoaded: false
    property bool pageReady: false
    property bool propertiesPending: false
    // Incremented by every pause, resume and load change; pending freeze work checks it.
    property int transition: 0
    property int loadSerial: 0
    property var grabResult: null

    // Calls window.wallpaperPropertyListener[name](argument) if the page defines it.
    function callListener(name, argument) {
        web.runJavaScript("(function(n,a){var l=window.wallpaperPropertyListener;"
                          + "if(l&&typeof l[n]==='function')l[n](a);})("
                          + JSON.stringify(name) + "," + JSON.stringify(argument) + ")");
    }

    function reportLifecycle() {
        root.surface.reportFrozen(web.lifecycleState === WebEngineView.LifecycleState.Frozen);
    }

    function showPage() {
        web.lifecycleState = WebEngineView.LifecycleState.Active;
        web.visible = true;
        still.visible = false;
        still.source = "";
        root.grabResult = null;
        root.reportLifecycle();
    }

    function applyPaused() {
        const serial = ++root.transition;
        freezeTimer.stop();
        if (root.host.paused) {
            // Applied by pageAvailable() once a page is ready.
            if (!root.pageReady)
                return;
            root.callListener("setPaused", true);
            freezeTimer.serial = serial;
            freezeTimer.restart();
            return;
        }
        root.showPage();
        if (!root.pageReady)
            return;
        root.callListener("setPaused", false);
        if (root.propertiesPending) {
            root.propertiesPending = false;
            root.callListener("applyUserProperties", root.host.currentProperties());
        }
    }

    // A page can only be frozen while it is hidden, so a still image covers the output.
    function freeze(serial) {
        if (serial !== root.transition || !root.host.paused || !root.pageReady)
            return;
        web.visible = false;
        web.lifecycleState = WebEngineView.LifecycleState.Frozen;
        root.reportLifecycle();
    }

    // Drops pending freeze work. A still image from an earlier pause stays on top while the
    // new page loads.
    function beginLoad() {
        ++root.loadSerial;
        ++root.transition;
        freezeTimer.stop();
        root.pageReady = false;
        web.lifecycleState = WebEngineView.LifecycleState.Active;
        web.visible = true;
        root.reportLifecycle();
        root.surface.reportLoading();
    }

    function pageAvailable() {
        root.everLoaded = true;
        root.pageReady = true;
        root.propertiesPending = false;
        root.callListener("applyGeneralProperties", { fps: root.host.fps });
        root.callListener("applyUserProperties", root.host.currentProperties());
        root.surface.reportLoaded();
        if (root.host.paused)
            root.applyPaused();
        else
            root.showPage();
    }

    // A cancelled navigation leaves the previous document, or what was loaded of the new
    // one, in place.
    function loadCancelled() {
        const serial = root.loadSerial;
        if (!root.host.navigationAllowed(web.url)) {
            console.warn("load of " + web.url + " was cancelled");
            return;
        }
        web.runJavaScript("document.readyState", function(state) {
            if (serial === root.loadSerial && !root.pageReady
                    && (state === "interactive" || state === "complete"))
                root.pageAvailable();
        });
    }

    function loadFailed(message) {
        ++root.transition;
        freezeTimer.stop();
        root.pageReady = false;
        if (!root.everLoaded)
            root.surface.reportLoadFailed(message);
        else if (root.surface.recover("cannot load " + message))
            reloadTimer.restart();
    }

    WebEngineView {
        id: web

        anchors.fill: parent
        url: root.host.url
        audioMuted: !root.host.audio
        activeFocusOnPress: false
        backgroundColor: "black"

        settings.showScrollBars: false
        settings.autoLoadIconsForPage: false
        settings.playbackRequiresUserGesture: false
        settings.localContentCanAccessFileUrls: true
        settings.localContentCanAccessRemoteUrls: root.host.allowRemote
        settings.focusOnNavigationEnabled: false
        settings.javascriptCanOpenWindows: false
        settings.javascriptCanAccessClipboard: false
        settings.pdfViewerEnabled: false
        settings.pluginsEnabled: false

        onLifecycleStateChanged: root.reportLifecycle()

        onLoadingChanged: function(info) {
            switch (info.status) {
            case WebEngineView.LoadStartedStatus:
                root.beginLoad();
                break;
            case WebEngineView.LoadSucceededStatus:
                // Failed loads are handled by their LoadFailedStatus.
                if (!info.isErrorPage)
                    root.pageAvailable();
                break;
            case WebEngineView.LoadStoppedStatus:
                root.loadCancelled();
                break;
            case WebEngineView.LoadFailedStatus:
                if (info.errorCode === root.errorAborted)
                    root.loadCancelled();
                else
                    root.loadFailed(info.url + ": " + info.errorString);
                break;
            }
        }

        onNavigationRequested: function(request) {
            if (!request.isMainFrame || root.host.navigationAllowed(request.url))
                return;
            root.surface.reportBlockedNavigation(request.url);
            if (typeof request.reject === "function")
                request.reject();
            else
                request.action = WebEngineNavigationRequest.IgnoreRequest;
        }

        onRenderProcessTerminated: function(terminationStatus, exitCode) {
            if (terminationStatus === WebEngineView.NormalTerminationStatus)
                return;
            ++root.transition;
            freezeTimer.stop();
            root.pageReady = false;
            if (root.surface.recover("web renderer terminated with exit code " + exitCode))
                reloadTimer.restart();
        }

        onJavaScriptConsoleMessage: function(level, message, lineNumber, sourceId) {
            if (root.host.diagnostics)
                root.surface.reportConsoleMessage(message, lineNumber, sourceId);
        }

        onJavaScriptDialogRequested: function(request) {
            request.accepted = true;
            request.dialogReject();
        }

        onFileDialogRequested: function(request) {
            request.accepted = true;
            request.dialogReject();
        }

        onAuthenticationDialogRequested: function(request) {
            request.accepted = true;
            request.dialogReject();
        }

        onContextMenuRequested: function(request) {
            request.accepted = true;
        }
    }

    Image {
        id: still

        anchors.fill: parent
        visible: false
        cache: false
    }

    Connections {
        target: root.host

        function onPausedChanged() {
            root.applyPaused();
        }

        function onPropertyValuesChanged(changed) {
            // Scripts do not run in a frozen page; the full set is sent again on resume
            // or with the next load.
            if (!root.pageReady || root.host.paused) {
                root.propertiesPending = true;
                return;
            }
            root.callListener("applyUserProperties", changed);
        }
    }

    Timer {
        id: freezeTimer

        property int serial: 0

        // Gives the page a frame to react to setPaused before the still image is taken.
        interval: 150
        onTriggered: {
            const serial = freezeTimer.serial;
            // After a reload during a pause the earlier still image is kept.
            if (still.visible) {
                root.freeze(serial);
                return;
            }
            const grabbing = web.grabToImage(function(result) {
                if (serial !== root.transition || !root.host.paused || !root.pageReady)
                    return;
                root.grabResult = result;
                still.source = result.url;
                still.visible = true;
                root.freeze(serial);
            });
            if (!grabbing)
                root.freeze(serial);
        }
    }

    // Loads the project entry again after a failed load or a renderer exit.
    Timer {
        id: reloadTimer

        interval: 1000
        onTriggered: {
            web.lifecycleState = WebEngineView.LifecycleState.Active;
            if (String(web.url) === String(root.host.url))
                web.reload();
            else
                web.url = root.host.url;
        }
    }

    Timer {
        interval: 500
        repeat: true
        running: root.host.diagnostics && root.pageReady && !root.host.paused
        onTriggered: {
            const serial = root.loadSerial;
            web.runJavaScript("JSON.stringify({readyState:document.readyState,"
                              + "visibilityState:document.visibilityState,"
                              + "page:typeof window.wallpaperDiagnostics==='function'"
                              + "?window.wallpaperDiagnostics():null})",
                              function(result) {
                                  if (serial === root.loadSerial && root.pageReady)
                                      root.surface.reportDiagnostics(String(result));
                              });
        }
    }
}
