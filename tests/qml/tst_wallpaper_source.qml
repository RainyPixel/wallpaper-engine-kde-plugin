import QtQuick
import QtTest
import "../../plugin/contents/ui" as Wallpaper

TestCase {
    id: testCase
    name: "WallpaperSource"
    property url storedSource
    property string projectReadPath

    Component {
        id: listComponent
        Wallpaper.WallpaperListModel {
            enabled: false
            readfile: (path) => {
                testCase.projectReadPath = path;
                return Promise.resolve('{"file":"clip#?%23.mp4","type":"video"}');
            }
        }
    }

    function test_roundTrip_data() {
        return [
            { tag: "plain", file: "movie.mp4" },
            { tag: "fragment", file: "movie#1.mp4" },
            { tag: "query", file: "movie?1.mp4" },
            { tag: "percent", file: "100%.mp4" },
            { tag: "encoded-literals", file: "literal%23%3F%25%20.mp4" },
            { tag: "unicode", file: "Обои 桜 🌸.mp4" },
            { tag: "subdirectory", file: "folder #?/clip %23.mp4" },
            { tag: "plus", file: "clip+scene.mp4" }
        ];
    }

    function test_roundTrip(data) {
        const directory = Wallpaper.Common.localFileUrl("/tmp/Обои #%23?100%/wallpapers");
        const packed = Wallpaper.Common.packWallpaperSource({
            path: directory, file: data.file, type: "video"
        });
        // Exercise the same QUrl conversion used by Plasma and main.qml.
        storedSource = packed;
        const source = String(Qt.resolvedUrl(storedSource));
        const unpacked = Wallpaper.Common.unpackWallpaperSource(source);
        compare(unpacked.type, "video");
        verify(!unpacked.path.includes("#"));
        verify(!unpacked.path.includes("?"));
        compare(Wallpaper.Common.urlNative(unpacked.path), "/tmp/Обои #%23?100%/wallpapers/" + data.file);
    }

    function test_existingSources() {
        compare(Wallpaper.Common.unpackWallpaperSource("file:///tmp/clip+video").path,
                "file:///tmp/clip");
        compare(Wallpaper.Common.unpackWallpaperSource("file:///tmp/scene.pkg+scene").type,
                "scene");
        compare(Wallpaper.Common.urlNative("/tmp/literal%23%3F%25"), "/tmp/literal%23%3F%25");
        compare(Wallpaper.Common.urlNative("file:///tmp/literal%2523%253F%2525"),
                "/tmp/literal%23%3F%25");
        compare(Wallpaper.Common.urlNative("file:/tmp/with%20space"), "/tmp/with space");
        compare(Wallpaper.Common.urlNative("file:///tmp/raw%name"), "/tmp/raw%name");
        compare(Wallpaper.Common.packWallpaperSource({path: "", file: "clip", type: "video"}), "");
        compare(Wallpaper.Common.unpackWallpaperSource("invalid").path, "");
    }

    function test_directoryRoundTrip() {
        const list = createTemporaryObject(listComponent, this);
        verify(list);
        list.playlists = {};
        const directory = "/tmp/Обои #%23?100%";
        list.loadFolderLists([{
            folder: directory,
            items: [{ name: "project#?%25", mtime: 1 }]
        }]);
        tryCompare(list.model, "count", 1);
        compare(projectReadPath, directory + "/project#?%25/project.json");
        storedSource = Wallpaper.Common.packWallpaperSource(list.model.get(0));
        const unpacked = Wallpaper.Common.unpackWallpaperSource(String(Qt.resolvedUrl(storedSource)));
        compare(Wallpaper.Common.urlNative(unpacked.path), directory + "/project#?%25/clip#?%23.mp4");
    }

    function test_playlistMembership_data() {
        return [
            { tag: "raw-windows", encoded: false },
            { tag: "encoded-file-url", encoded: true }
        ];
    }

    function test_playlistMembership(data) {
        const list = createTemporaryObject(listComponent, this);
        verify(list);
        const directory = "/tmp/Обои 桜 space%23%20#?/project";
        const entry = data.encoded ? Wallpaper.Common.localFileUrl(directory + "/project.json")
                                   : "C:" + directory + "/project.json";
        list.globalConfigPath = "/tmp/playlist-config.json";
        list.initItemOp = (item) => { item.favor = item.workshopid === "project"; };
        list.readfile = (path) => Promise.resolve(path === list.globalConfigPath
            ? JSON.stringify({ steamuser: { general: { playlists: [{ name: "saved", items: [entry] }] } } })
            : '{"file":"clip.mp4","type":"video"}');
        let loaded = false;
        list.loadPlaylists().then(() => { loaded = true; });
        tryVerify(() => loaded);
        const folders = [{
            folder: directory.substring(0, directory.lastIndexOf('/')),
            items: [{ name: "project", mtime: 1 }, { name: "other", mtime: 1 }]
        }];
        list.loadFolderLists(folders);
        tryCompare(list.model, "count", 2);
        const item = list.model.get(1);
        compare(item.playlists.count, 1);
        compare(item.playlists.get(0).key, "saved");
        verify(item.favor);
        list.filterStr = Wallpaper.Common.filterModel.map((filter) =>
            filter.type === "playlist" || filter.type === "favor" ? 1 : filter.def).join('');
        loaded = false;
        list.loadFolderLists(folders).then(() => { loaded = true; });
        tryVerify(() => loaded);
        compare(list.model.count, 1);
        compare(list.model.get(0).workshopid, "project");
    }

    function test_projectDirectory_data() {
        return [
            {tag: "second-library", source: "/second/workshop/42/index.html", id: "42",
                dirs: ["/first/workshop", "/second/workshop"], expected: "/second/workshop/42"},
            {tag: "nested-html", source: "/second/workshop/42/web/app/index.html", id: "42",
                dirs: ["/first/workshop", "/second/workshop"], expected: "/second/workshop/42"},
            {tag: "repeated-id", source: "/second/workshop/42/web/42/index.html", id: "42",
                dirs: ["/second/workshop"], expected: "/second/workshop/42"},
            {tag: "encoded-names", source: "/lib #%23/projects/Обои #?/web/index.html", id: "Обои #?",
                dirs: ["/lib #%23/projects"], expected: "/lib #%23/projects/Обои #?"},
            {tag: "case-fallbacks", source: "/Steamapps/Workshop/42/sub/index.html", id: "42",
                dirs: [["/steamapps/workshop", "/Steamapps/Workshop"]], expected: "/Steamapps/Workshop/42"},
            {tag: "custom-folder", source: "/custom/local/sub/index.html", id: "local",
                dirs: ["/steam/workshop"], expected: "/custom/local"}
        ];
    }

    function test_projectDirectory(data) {
        const result = Wallpaper.Common.getProjectDirectory(Wallpaper.Common.localFileUrl(data.source),
            data.id, data.dirs.map(dir => Array.isArray(dir)
                ? dir.map(Wallpaper.Common.localFileUrl) : Wallpaper.Common.localFileUrl(dir)));
        compare(Wallpaper.Common.urlNative(result), data.expected);
    }

    function test_malformedProject_data() {
        return [
            {tag: "null-tags", project: '{"type":"video","file":"bad.mp4","tags":null}'},
            {tag: "object-tags", project: '{"type":"video","tags":{}}'},
            {tag: "non-string-tag", project: '{"type":"video","tags":[null]}'},
            {tag: "non-string-type", project: '{"type":5}'},
            {tag: "non-string-title", project: '{"type":"video","title":{}}'},
            {tag: "array-project", project: '[]'},
            {tag: "null-project", project: 'null'},
            {tag: "invalid-json", project: '{'},
            {tag: "unreadable", project: null}
        ];
    }

    function test_malformedProject(data) {
        const list = createTemporaryObject(listComponent, this);
        list.playlists = {};
        list.readfile = path => path.includes("/bad/")
            ? (data.project === null ? Promise.reject(new Error("read failed")) : Promise.resolve(data.project))
            : Promise.resolve('{"type":"video","file":"good.mp4","tags":[]}');
        let finished = false;
        list.loadFolderLists([{folder: "/tmp/catalogue", items: [
            {name: "bad", mtime: 1}, {name: "good", mtime: 2}
        ]}]).then(() => { finished = true; });
        tryVerify(() => finished);
        compare(list.model.count, 1);
        compare(list.model.get(0).workshopid, "good");
    }

    function cleanup() {
        const filters = Wallpaper.Common.filterModel;
        for(let i = filters.count - 1; i >= 0; --i) {
            if(filters.get(i).type === "playlist" && filters.get(i).key === "saved")
                filters.remove(i);
        }
    }
}
