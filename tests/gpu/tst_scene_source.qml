import QtQuick
import QtTest
import com.github.catsout.wallpaperEngineKde 1.2

Item {
    id: root
    width: 640
    height: 240

    Component {
        id: sceneComponent
        SceneViewer {
            width: 320
            height: 240
            assets: Qt.resolvedUrl(".")
            source: Qt.resolvedUrl("scenes/red/scene.json")
            muted: true
            mirrorScene: true
            fps: 30
        }
    }

    TestCase {
        name: "SceneFrameSource"

        function initTestCase() {
            tryVerify(() => windowShown, 5000, "the GPU test window must be exposed");
        }

        function test_mirror_replacement_data() {
            return [{tag: "shared", shared: true}, {tag: "private", shared: false}];
        }

        function expectColor(viewer, red) {
            tryVerify(() => {
                const image = grabImage(viewer);
                return Math.abs(image.red(160, 120) - red) < 5
                    && image.green(160, 120) < 5 && image.blue(160, 120) < 5;
            }, 5000, "the imported frame must belong to the current scene source");
        }

        function test_mirror_replacement(data) {
            const primary = createTemporaryObject(sceneComponent, root, {shareGpu: data.shared});
            verify(primary);
            primary.play();
            expectColor(primary, 255);
            const mirror = createTemporaryObject(sceneComponent, root, {x: 320, shareGpu: data.shared});
            verify(mirror);
            mirror.play();
            expectColor(mirror, 255);

            // The old secondary becomes a new producer, then the old producer joins it.
            mirror.source = Qt.resolvedUrl("scenes/black/scene.json");
            expectColor(mirror, 0);
            primary.source = mirror.source;
            expectColor(primary, 0);

            // A private producer's device must survive while its mirror imports survive.
            mirror.destroy();
            wait(100);
            primary.source = Qt.resolvedUrl("scenes/red/scene.json");
            expectColor(primary, 255);
        }
    }
}
