import QtQuick

QtObject {
    objectName: "fixtureFileHelper"
    property var notifications: []
    function notifySaveError(message) { notifications.push(message); }
    function readFile(path) {
        if (path === "error") throw new Error("read failed");
        return "fixture:" + path;
    }
    function getDirSize(path, depth) { return depth; }
    function getFolderList(path, options) { return {folder: path, items: []}; }
    function readWallpaperConfig(id) { return {id: id}; }
    function writeWallpaperConfig(id, changed, replaceExisting) {
        return id !== "error" && (id !== "replace" || replaceExisting === true);
    }
    function resetWallpaperConfig(id) { return id !== "error"; }
    function detectSteam(path) { return {library: path}; }
}
