// Project Stark: always read profiles from the real "profiles" folder next to lawinserver.exe,
// never from the copy packed inside the exe. That copy is why the locker reset on every restart.
const fs = require("fs");
const path = require("path");

const cache = {};

module.exports = function loadProfile(requestPath) {
    const file = path.basename(requestPath);

    if (!cache[file]) {
        cache[file] = JSON.parse(fs.readFileSync(path.join(process.cwd(), "profiles", file), "utf8"));
    }

    return cache[file];
};
