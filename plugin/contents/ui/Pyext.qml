import QtQuick 2.0
import com.github.catsout.wallpaperEngineKde 1.2

// FileHelper wrapper retaining the asynchronous caller API
Item {
    id: root

    // Always ready (no Python startup delay)
    readonly property bool ok: true
    readonly property string log: ""
    readonly property string version: "native"

    // Internal C++ FileHelper instance
    FileHelper {
        id: fileHelper
    }

    function readfile(path) {
        return new Promise(resolve => resolve(fileHelper.readFile(path)));
    }

    function get_dir_size(path, depth) {
        return new Promise(resolve => resolve(fileHelper.getDirSize(path, depth === undefined ? 3 : depth)));
    }

    function get_folder_list(path, opt) {
        return new Promise(resolve => resolve(fileHelper.getFolderList(path, opt || {})));
    }

    function read_wallpaper_config(id) {
        return new Promise(resolve => resolve(fileHelper.readWallpaperConfig(id)));
    }

    function write_wallpaper_config(id, changed, replaceExisting) {
        return new Promise(resolve => {
            if (!fileHelper.writeWallpaperConfig(id, changed, Boolean(replaceExisting))) {
                notifySaveError("Failed to save wallpaper settings. Previous settings were kept.");
                throw new Error("Failed to save wallpaper configuration");
            }
            resolve();
        });
    }

    function reset_wallpaper_config(id) {
        return new Promise(resolve => {
            if (!fileHelper.resetWallpaperConfig(id)) {
                notifySaveError("Failed to reset wallpaper settings. Previous settings were kept.");
                throw new Error("Failed to reset wallpaper configuration");
            }
            resolve();
        });
    }

    function notifySaveError(message) {
        fileHelper.notifySaveError(message);
    }

    // No promise here, the callers bind the map straight into properties
    function detect_steam(configuredLibrary) {
        return fileHelper.detectSteam(configuredLibrary || '');
    }
}
