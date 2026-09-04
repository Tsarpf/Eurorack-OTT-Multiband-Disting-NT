/* Live Probe file bridge. Runs in Max's low-priority JS scheduler. */
autowatch = 1;
inlets = 1;
outlets = 0;

var owner = this;
var patcherObject = this.patcher;
var role = jsarguments[1];
var endpoint = jsarguments[2];
var bufferName = jsarguments[3] || "live_probe_input";
var lastId = null;
var polling = null;
var pending = null;
var playing = false;
var playbackId = null;
var loadedPath = null;
var durationMs = 0;
var recordingPath = null;
var sampleRate = 0;
var dspRunning = false;
var tasks = [];
var instanceId = String(new Date().getTime()) + "-" + String(Math.random());

function readJSON(path) {
    var f = new File(path, "read");
    if (!f.isopen) return null;
    var contents = "";
    try {
        while (f.position < f.eof) contents += f.readstring(8192);
    } finally { f.close(); }
    return contents ? JSON.parse(contents) : null;
}
function writeJSON(path, value) {
    var f = new File(path, "write", "TEXT");
    if (!f.isopen) throw new Error("Cannot write " + path);
    try { f.eof = 0; f.writestring(JSON.stringify(value)); }
    finally { f.close(); }
}
function label(text) {
    var box = patcherObject.getnamed("status_label");
    if (box) box.message("set", text);
}
function reply(command, result, errorText) {
    var response = {id: command.id, ok: !errorText};
    if (errorText) response.error = String(errorText);
    else response.result = result || {};
    writeJSON(endpoint + "/response.json", response);
    label(role + ": " + (errorText || command.action + " OK"));
}
function later(fn, milliseconds) {
    var task = new Task(function () {
        try { fn.call(owner); } catch (e) { error("Live Probe: " + e + "\n"); }
        var i = tasks.indexOf(task);
        if (i >= 0) tasks.splice(i, 1);
        task.freepeer();
    }, owner);
    tasks.push(task);
    task.schedule(milliseconds);
}
function init() {
    if (polling) return;
    var ownDevice = api("this_device");
    if (!Number(ownDevice.id)) return;
    var startup = new File(endpoint + "/startup.log", "write", "TEXT");
    if (startup.isopen) {
        startup.position = startup.eof;
        startup.writeline(instanceId + " " + ownDevice.unquotedpath);
        startup.close();
    }
    // Never replay an old command when a saved set is reopened.
    try {
        var old = readJSON(endpoint + "/command.json");
        lastId = old ? old.id : null;
        var previous = readJSON(endpoint + "/response.json");
        if (old && (!previous || previous.id !== old.id))
            reply(old, null, "Device restarted before command completed; retry the command.");
    }
    catch (e) { lastId = null; }
    polling = new Task(poll, owner);
    polling.interval = 50;
    polling.repeat();
    var state = patcherObject.getnamed("dsp_state");
    if (!state) {
        var object = patcherObject.firstobject;
        while (object) {
            if (object.maxclass === "dspstate~") { state = object; break; }
            object = object.nextobject;
        }
    }
    if (state) state.message("bang");
    label(role + ": ready");
}
function notifydeleted() {
    if (polling) { polling.cancel(); polling.freepeer(); }
    for (var i = 0; i < tasks.length; ++i) { tasks[i].cancel(); tasks[i].freepeer(); }
}
function samplerate(value) { sampleRate = Number(value); }
function dsp(value) { dspRunning = Boolean(value); }
function api(path) { return new LiveAPI(function () {}, path); }
function scalar(value) { return value instanceof Array ? value[0] : value; }
function text(value) { return value instanceof Array ? value.join(" ") : String(value); }
function prop(object, key, fallback) {
    try { return scalar(object.get(key)); } catch (e) { return fallback; }
}
function display(object, value) { return text(object.call("str_for_value", value)); }

function siblings() {
    var self = api("this_device");
    var parent = api("this_device canonical_parent");
    var parentPath = parent.unquotedpath;
    var foundSelf = false;
    var foundCapture = false;
    var devices = [];
    function walk(path) {
        var object = api(path);
        devices.push({name: text(object.get("name")), path: path,
                      id: Number(object.id), class_name: text(object.get("class_name"))});
        if (Number(prop(object, "can_have_chains", 0))) {
            for (var c = 0; c < object.getcount("chains"); ++c) {
                var chainPath = path + " chains " + c;
                var chain = api(chainPath);
                for (var d = 0; d < chain.getcount("devices"); ++d)
                    walk(chainPath + " devices " + d);
            }
        }
    }
    for (var i = 0; i < parent.getcount("devices"); ++i) {
        var path = parentPath + " devices " + i;
        var object = api(path);
        if (Number(object.id) === Number(self.id)) { foundSelf = true; continue; }
        if (!foundSelf) continue;
        if (text(object.get("name")) === "Live Probe Capture") { foundCapture = true; break; }
        walk(path);
    }
    if (!foundCapture) throw new Error("Place Live Probe Capture after the effects, on the same track or rack chain; keep its name unchanged.");
    return devices;
}
function resolveDevice(selector) {
    var all = siblings(), matches = [];
    for (var i = 0; i < all.length; ++i) {
        var d = all[i];
        if ((typeof selector === "string" && d.name === selector) ||
            (selector && selector.path && selector.path === d.path) ||
            (selector && selector.name && selector.name === d.name)) matches.push(d);
    }
    if (matches.length !== 1)
        throw new Error("Device selector must identify exactly one device between Source and Capture; found " + matches.length);
    return matches[0];
}
function parameterInfo(device, index) {
    var path = device.path + " parameters " + index;
    var object = api(path);
    var value = Number(scalar(object.get("value")));
    var quantized = Boolean(Number(scalar(object.get("is_quantized"))));
    var items = [];
    if (quantized) {
        try { items = object.get("value_items"); if (!(items instanceof Array)) items = [items]; }
        catch (e) { items = []; }
    }
    return {id: Number(object.id), index: index, path: path,
            name: text(object.get("name")), original_name: text(object.get("original_name")),
            min: Number(scalar(object.get("min"))), max: Number(scalar(object.get("max"))),
            value: value, display_value: display(object, value),
            is_quantized: quantized, value_items: items,
            is_enabled: Boolean(Number(prop(object, "is_enabled", 1)))};
}
function parameterList(selector) {
    var device = resolveDevice(selector), object = api(device.path), parameters = [];
    for (var i = 0; i < object.getcount("parameters"); ++i) parameters.push(parameterInfo(device, i));
    return {device: device, parameters: parameters};
}
function resolveParameter(list, name) {
    var found = [];
    for (var i = 0; i < list.length; ++i)
        if (list[i].name === name || list[i].original_name === name || list[i].path === name)
            found.push(list[i]);
    if (found.length !== 1) throw new Error("Parameter '" + name + "' is missing or ambiguous. Run parameters first.");
    return found[0];
}
function displayNumber(str) {
    var match = String(str).replace(/\u2212/g, "-").match(/^\s*([+-]?(?:\d+(?:\.\d*)?|\.\d+))\s*(.*?)\s*$/);
    if (!match) return null;
    var result = {number: Number(match[1]), unit: match[2].toLowerCase()};
    if (result.unit === "khz") { result.number *= 1000; result.unit = "hz"; }
    if (result.unit === "s") { result.number *= 1000; result.unit = "ms"; }
    return result;
}
function valueFor(info, specification) {
    var object = api(info.path), raw;
    if (typeof specification === "number") raw = specification;
    else if (specification && typeof specification.normalized === "number") {
        if (specification.normalized < 0 || specification.normalized > 1) throw new Error("Normalized values must be within 0..1");
        raw = info.min + specification.normalized * (info.max - info.min);
        if (info.is_quantized) raw = Math.round(raw);
    } else {
        var requested = typeof specification === "string" ? specification : specification && specification.display;
        if (typeof requested !== "string") throw new Error("Use a raw number, enum label, {normalized:...}, or {display:...}");
        // Search discrete parameters using their actual display strings.
        if (info.is_quantized && info.max - info.min <= 4096) {
            for (var v = info.min; v <= info.max; ++v) {
                var item = info.value_items[Math.round(v - info.min)];
                if (String(item).toLowerCase() === requested.toLowerCase() || display(object, v).toLowerCase() === requested.toLowerCase()) return v;
            }
        }
        var target = displayNumber(requested);
        if (!target) throw new Error("Unknown value '" + requested + "' for " + info.name);
        // Many Live knobs expose normalized raw values. Invert their formatted
        // numeric display instead of assuming that e.g. raw 100 means 100%.
        var lo = info.min, hi = info.max, best = lo, bestError = Infinity;
        var low = displayNumber(display(object, lo)), high = displayNumber(display(object, hi));
        if (low && low.unit === target.unit && low.number === target.number) return lo;
        if (high && high.unit === target.unit && high.number === target.number) return hi;
        // Skip endpoint-only labels such as -inf, while retaining finite range.
        if (!low) { lo += (hi - lo) * 1e-7; low = displayNumber(display(object, lo)); }
        if (!high) { hi -= (hi - lo) * 1e-7; high = displayNumber(display(object, hi)); }
        if (!low || !high || low.unit !== target.unit || high.unit !== target.unit)
            throw new Error("Cannot invert this display; inspect parameters and use raw/normalized values for " + info.name);
        var increasing = high.number >= low.number;
        if (target.number < Math.min(low.number, high.number) || target.number > Math.max(low.number, high.number))
            throw new Error("Display value outside parameter range: " + requested);
        for (var k = 0; k < 44; ++k) {
            var mid = (lo + hi) * 0.5;
            var measured = displayNumber(display(object, mid));
            if (!measured || measured.unit !== target.unit) throw new Error("Non-numeric display during inversion");
            var distance = Math.abs(measured.number - target.number);
            if (distance < bestError) { best = mid; bestError = distance; }
            if (distance < 1e-9) break;
            if ((measured.number < target.number) === increasing) lo = mid; else hi = mid;
        }
        if (bestError > Math.max(0.01, Math.abs(target.number) * 0.001))
            throw new Error("Display value cannot be represented accurately: " + requested);
        raw = best;
    }
    // Live exposes float32 bounds, while JSON numbers are doubles. Values
    // such as 0.1 can sit a few ulps below the reported minimum.
    var endpointTolerance = Math.max(1, Math.abs(info.min), Math.abs(info.max)) * 1e-7;
    if (!isFinite(raw) || raw < info.min - endpointTolerance || raw > info.max + endpointTolerance)
        throw new Error("Value outside range for " + info.name);
    raw = Math.max(info.min, Math.min(info.max, raw));
    if (info.is_quantized && raw !== Math.round(raw)) throw new Error("Discrete parameter requires an integer: " + info.name);
    return raw;
}
function setParameters(args) {
    var listing = parameterList(args.device), changes = [], keys = Object.keys(args.parameters || {});
    for (var i = 0; i < keys.length; ++i) {
        var info = resolveParameter(listing.parameters, keys[i]);
        if (!info.is_enabled) throw new Error("Parameter is disabled: " + info.name);
        changes.push({info: info, value: valueFor(info, args.parameters[keys[i]])});
    }
    var applied = [];
    try {
        for (var j = 0; j < changes.length; ++j) {
            var change = changes[j];
            api(change.info.path).set("value", change.value);
            applied.push(change);
            var actual = Number(prop(api(change.info.path), "value", NaN));
            if (Math.abs(actual - change.value) > Math.max(1e-6, (change.info.max - change.info.min) * 1e-5))
                throw new Error("Parameter readback differs: " + change.info.name);
        }
    } catch (e) {
        for (var r = applied.length - 1; r >= 0; --r) {
            try { api(applied[r].info.path).set("value", applied[r].info.value); } catch (ignored) {}
        }
        throw e;
    }
    var result = {device: listing.device, parameters: [], previous: []};
    for (var p = 0; p < changes.length; ++p) {
        result.previous.push(changes[p].info);
        result.parameters.push(parameterInfo(listing.device, changes[p].info.index));
    }
    return result;
}
function loaded() {
    if (!pending || pending.action !== "load") return;
    var command = pending;
    pending = null;
    try {
        var b = new Buffer(bufferName);
        durationMs = b.length();
        if (!(durationMs > 0) || !(b.framecount() > 0)) throw new Error("Loaded buffer is empty");
        reply(command, {path: loadedPath, duration_ms: durationMs, frames: b.framecount(), channels: b.channelcount()});
    } catch (e) { loadedPath = null; reply(command, null, e); }
}
function finished() { playing = false; }
function poll() {
    if (pending) return;
    var command;
    try { command = readJSON(endpoint + "/command.json"); } catch (e) { return; }
    if (!command || !command.id || command.id === lastId) return;
    lastId = command.id;
    try {
        var args = command.args || {}, result = {};
        if (command.action === "ping" || command.action === "status") {
            result = {protocol: 1, role: role, platform: max.os === "windows" ? "windows" : "macintosh",
                      instance_id: instanceId, device_path: api("this_device").unquotedpath,
                      sample_rate: sampleRate, dsp_running: dspRunning, playing: playing,
                      playback_id: playbackId, recording: Boolean(recordingPath), path: recordingPath};
        } else if (role === "source") {
            if (command.action === "devices") result = {devices: siblings()};
            else if (command.action === "parameters") result = parameterList(args.device);
            else if (command.action === "set") result = setParameters(args);
            else if (command.action === "load") {
                patcherObject.getnamed("player").message("stop");
                playing = false;
                loadedPath = String(args.path);
                pending = command;
                patcherObject.getnamed("input_buffer").message("replace", loadedPath);
                later(function () {
                    if (pending && pending.id === command.id) {
                        pending = null; loadedPath = null;
                        reply(command, null, "Buffer load timed out; check the host path and WAV format.");
                    }
                }, 10000);
                return;
            } else if (command.action === "play") {
                if (!loadedPath || durationMs <= 0) throw new Error("Load a WAV before playing");
                if (!dspRunning) throw new Error("Live audio engine is not running");
                playing = true; playbackId = command.id;
                patcherObject.getnamed("player").message("start", 0, durationMs, durationMs);
                result = {playing: playing, playback_id: playbackId};
            } else if (command.action === "stop") {
                patcherObject.getnamed("player").message("stop"); playing = false;
                result = {playing: false, playback_id: playbackId};
            } else throw new Error("Unknown source action: " + command.action);
        } else if (role === "capture") {
            var recorder = patcherObject.getnamed("recorder");
            if (command.action === "record") {
                if (recordingPath) throw new Error("Already recording; stop first");
                if (!dspRunning) throw new Error("Live audio engine is not running");
                if (args.channels && args.channels !== 2) throw new Error("Capture supports two channels");
                recorder.message("samptype", "float32");
                recorder.message("open", String(args.path), "wave");
                pending = command;
                later(function () {
                    pending = null;
                    recordingPath = String(args.path);
                    recorder.message("int", 1);
                    reply(command, {recording: true, path: recordingPath});
                }, 100);
                return;
            } else if (command.action === "stop") {
                var path = recordingPath;
                recorder.message("int", 0); recordingPath = null;
                pending = command;
                later(function () {
                    pending = null;
                    if (path) {
                        var f = new File(path, "read");
                        var valid = f.isopen && f.eof > 44;
                        if (f.isopen) f.close();
                        if (!valid) { reply(command, null, "Recorder did not produce a nonempty WAV; check Max console and output path."); return; }
                    }
                    reply(command, {recording: false, path: path});
                }, 150);
                return;
            } else throw new Error("Unknown capture action: " + command.action);
        } else throw new Error("Invalid role");
        reply(command, result);
    } catch (e) { reply(command, null, e); }
}

// autowatch recompiles this script without reloading the surrounding device.
// Restart polling after such a reload; all LiveAPI calls remain command-time.
later(init, 1000);
