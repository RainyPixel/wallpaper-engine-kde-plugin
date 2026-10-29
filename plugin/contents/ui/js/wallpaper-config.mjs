export function savePending(pyext, changes, resets) {
    const writes = [];
    const ids = new Set([...resets, ...Object.keys(changes)]);
    ids.forEach(wid => {
        if (Object.prototype.hasOwnProperty.call(changes, wid)) {
            const saved = JSON.stringify(changes[wid]);
            const replaceExisting = resets.has(wid);
            writes.push(pyext.write_wallpaper_config(wid, JSON.parse(saved), replaceExisting).then(() => {
                if (replaceExisting) resets.delete(wid);
                if (JSON.stringify(changes[wid]) === saved) delete changes[wid];
            }));
        } else {
            writes.push(pyext.reset_wallpaper_config(wid).then(() => { resets.delete(wid); }));
        }
    });
    return Promise.all(writes);
}
