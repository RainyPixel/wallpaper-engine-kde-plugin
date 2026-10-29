import QtQuick 2.5
import QtWebEngine 1.10
import QtWebChannel 1.10
import ".."
import "../js/utils.mjs" as Utils

Item {
    id: webItem
    anchors.fill: parent
    property url source
    property bool scriptsReady: false
    property bool hasLib: background.hasLib
    property int fps: background.fps
    property var readfile

    property string userPropsJson: background.userPropsJson
    property var projectProperties: ({})

    function sendUserProperties() {
        if (!webobj.loaded) return;
        const saved = Utils.parseJson(userPropsJson) || {};
        const properties = {};
        for (const key of Object.keys(projectProperties)) {
            const property = projectProperties[key];
            if (property && typeof property === "object" && "value" in property)
                properties[key] = {value: Object.prototype.hasOwnProperty.call(saved, key)
                    ? saved[key] : property.value};
        }
        webobj.sigUserProperties(properties);
    }

    onUserPropsJsonChanged: sendUserProperties()
    onFpsChanged: {
        if (webobj.loaded) webobj.sigGeneralProperties({fps: webItem.fps});
    }
    onSourceChanged: {
        webobj.loaded = false;
        projectProperties = {};
    }

    Image {
        id: pauseImage
        anchors.fill: parent
        visible: true
        enabled: false
    }
    QtObject {
        id: webobj
        WebChannel.id: "wpeQml"
        signal sigGeneralProperties(var properties)
        signal sigUserProperties(var properties)
        signal sigAudio(var audioArray)
        property bool loaded: false
        onLoadedChanged: {
            if (!loaded) return;
            webobj.sigGeneralProperties({fps: webItem.fps});
            const currentSource = String(webItem.source);
            const projectUrl = background.getWorkshopIDPath() + "/project.json";
            readfile(Common.urlNative(projectUrl)).then(text => {
                if (!webobj.loaded || String(webItem.source) !== currentSource) return;
                const project = Utils.parseJson(text);
                const properties = project?.general?.properties;
                webItem.projectProperties = properties && typeof properties === "object"
                    && !Array.isArray(properties) ? properties : {};
                webItem.sendUserProperties();
            }).catch(error => console.error("Failed to load web properties: " + error));
        }
    }

    WebChannel {
        id: channel
        registeredObjects: [webobj]
    }

    WebEngineView {
    //WebView {
        id: web
        anchors.fill: parent
        url: webItem.scriptsReady ? webItem.source : ""
        enabled: true
        audioMuted: background.mute
        activeFocusOnPress: false
        webChannel: channel

        property bool paused: false
        property bool _init: {
            settings.fullscreenSupportEnabled = true;
            settings.autoLoadIconsForPage = false;
            settings.printElementBackgrounds = false;
            settings.playbackRequiresUserGesture = false;
            settings.pdfViewerEnabled = false;
            settings.showScrollBars = false;

            settings.localContentCanAccessRemoteUrls = true;
            settings.allowGeolocationOnInsecureOrigins = true;
            _init = true;
        }


        //onContextMenuRequested: function(request) {
        //    request.accepted = true;
        //}
        onLoadingChanged: (loadingInfo) => {
            if (loadingInfo.status === WebEngineView.LoadStartedStatus) {
                webobj.loaded = false;
                webItem.projectProperties = {};
            }
            if(loadingInfo.status == WebEngineView.LoadSucceededStatus) {
                // check pause after load
                if(paused) {
                    webItem.play();
                    webItem.pause();
                }
                background.sig_backendFirstFrame('QtWebEngine');
            }
        }

        onPausedChanged: {
            if(paused) {
                pauseTimer.start();
            }
            else {
                web.visible = true;
                web.lifecycleState = WebEngineView.LifecycleState.Active;
                pauseImage.visible = false;
            }
        }

        Component.onCompleted: {
            // Load the resource before insertion; sourceUrl can race the first navigation.
            readfile(":/qtwebchannel/qwebchannel.js").then(script => {
                if (!script) throw new Error("Empty Qt WebChannel script");
                installScripts(String(script));
            }).catch(error => console.error("Failed to load Qt WebChannel script: " + error));
        }
        function installScripts(channelScript) {
            userScripts.insert([
                {
                    injectionPoint: WebEngineScript.DocumentCreation,
                    worldId: WebEngineScript.MainWorld,
                    name: "Audio",
                    sourceCode: `
                        window.wallpaperRegisterAudioListener = function(listener) {
                            if(window.wpeQml)
                                window.wpeQml.sigAudio.connect(listener);
                            else
                                window.wallpaperRAed = listener;
                        }
                    `
                },
                {
                    worldId: WebEngineScript.MainWorld,
                    injectionPoint: WebEngineScript.Deferred,
                    name: "ObjectInjector",
                    sourceCode: channelScript + `
                        new QWebChannel(qt.webChannelTransport, function(channel) {
                            window.wpeQml = channel.objects.wpeQml;
                            const wpeQml = window.wpeQml;
                            const propertyListener = window.wallpaperPropertyListener;
                            if(window.wallpaperRAed)
                                wpeQml.sigAudio.connect(window.wallpaperRAed);
                            if(propertyListener) {
                                if(propertyListener.applyGeneralProperties)
                                    wpeQml.sigGeneralProperties.connect(propertyListener.applyGeneralProperties);
                                if(propertyListener.applyUserProperties)
                                    wpeQml.sigUserProperties.connect(propertyListener.applyUserProperties);
                            }
                            wpeQml.loaded = true;
                        });
                        document.getElementsByTagName('body')[0].ondragstart = function() { return false; }
                        `
                }
            ])
            webItem.scriptsReady = true;
            background.nowBackend = 'QtWebEngine';
        }

    }
    // There is no signal for frame complete, so use timer to make sure not black result
    Timer{
        id: pauseTimer
        running: false
        repeat: false
        interval: 300 
        onTriggered: {
            // only check paused status on timer, not set
            // this is async
            web.grabToImage(function(result) {
                // check for paused again, make sure web is visible
                if(web.paused == false || web.visible == false) return;
                pauseImage.source = result.url;
                pauseImage.visible = true;
                web.visible = false;
                web.lifecycleState = WebEngineView.LifecycleState.Frozen;
            });
        }   
    }
    Component.onCompleted: {
    //target: web.children[0] ? web.children[0] : null
    }

    function play(){
        web.paused = false;
    }
    function pause(){
        // Set status first
        web.paused = true;
    }
    function getMouseTarget() {
        web.activeFocusOnPress = true;
        return Qt.binding(function() { return web.children[0]; })
    }
}
