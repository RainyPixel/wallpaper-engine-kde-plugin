import QtQuick 2.5
import com.github.catsout.wallpaperEngineKde 1.2
import ".."

Item{
    id: sceneItem
    anchors.fill: parent
    property alias source: player.source
    property string assets: "assets"
    property int displayMode: background.displayMode
    property string userPropsJson: background.userPropsJson
    // Non-empty when the scene's bottom layer is an embedded MP4: mpv plays it
    // below, and the renderer draws the remaining layers over it transparently.
    readonly property string underlayUrl: player.videoUnderlayUrl(player.source)
    property var volumeFade: Common.createVolumeFade(
        sceneItem, 
        Qt.binding(function() { return background.mute ? 0 : background.volume; }),
        (volume) => { player.volume = volume / 100.0; }
    )

    onDisplayModeChanged: {
        if(displayMode == Common.DisplayMode.Scale)
            player.fillMode = SceneViewer.STRETCH;
        else if(displayMode == Common.DisplayMode.Aspect)
            player.fillMode = SceneViewer.ASPECTFIT;
        else if(displayMode == Common.DisplayMode.Crop)
            player.fillMode = SceneViewer.ASPECTCROP;
        if(underlay.item)
            underlay.applyDisplayMode();
    }

    // Force fillMode update on background.displayMode change
    Timer {
        id: displayModeFixTimer
        interval: 50
        repeat: false
        onTriggered: sceneItem.displayModeChanged()
    }
    Connections {
        target: background
        function onDisplayModeChanged() {
            displayModeFixTimer.restart();
        }
    }

    Loader {
        id: underlay
        anchors.fill: parent
        active: sceneItem.underlayUrl !== ""
        sourceComponent: Mpv {
            mute: true
            volume: 0
            hwdec: background.mpvHwdec
            maxFps: background.fps
            // mpv only accepts files once its render context exists.
            // loadfile directly: a QUrl source would re-encode the slice:// path.
            onInitFinished: command(["loadfile", sceneItem.underlayUrl])
        }
        function applyDisplayMode() {
            const crop = sceneItem.displayMode == Common.DisplayMode.Crop;
            const scale = sceneItem.displayMode == Common.DisplayMode.Scale;
            item.setProperty("keepaspect", !scale);
            item.setProperty("panscan", crop ? 1.0 : 0.0);
        }
        onLoaded: {
            applyDisplayMode();
            item.setProperty("speed", background.speed);
        }
    }
    onUnderlayUrlChanged: {
        if(underlay.item && underlayUrl !== "")
            underlay.item.command(["loadfile", underlayUrl]);
    }

    SceneViewer {
        id: player
        anchors.fill: parent
        fps: background.fps
        muted: background.mute
        cachePasses: background.cacheScenePasses
        shareGpu: background.shareGpuContext
        mirrorScene: background.mirrorScene
        speed: background.speed
        assets: sceneItem.assets
        userProperties: sceneItem.userPropsJson
        Component.onCompleted: {
            player.setAcceptMouse(true);
            player.setAcceptHover(true);
        }

        Connections {
            target: player
            function onFirstFrame() {
                background.sig_backendFirstFrame('scene');
            }
        }
    }

    Component.onCompleted: {
        background.nowBackend = 'scene';
        sceneItem.displayModeChanged();
    }
    function play() {
        volumeFade.start();
        player.play();
        if(underlay.item) underlay.item.play();
    }
    function pause() {
        volumeFade.stop();
        player.pause();
        if(underlay.item) underlay.item.pause();
    }
    
    function getMouseTarget() {
        return Qt.binding(function() { return player; })
    }

    function resetPointer() {
        player.centerPointer()
    }
}
