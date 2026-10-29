// Bridge to the C virtual board (mcboard.h): runs the OS on its own thread, publishes screen + LEDs.
import AppKit
import Combine
import CoreAudio
import MCBoard
import SwiftUI

/// The C board for calls that may come after it is gone: MIDI callbacks and MiniLab timers (a pad tap's release fires
/// 110 ms later). `with` runs only while the board exists; `close` (Board.stop, before mcb_destroy) waits for a call
/// in progress and turns later ones into nothing. The audio render blocks keep the plain pointer: their engines stop
/// before the board goes, and a lock has no place on the audio thread.
final class BoardHandle: @unchecked Sendable {
    private var board: OpaquePointer?
    private let lock = NSLock()

    init(_ board: OpaquePointer) { self.board = board }

    @discardableResult
    func with<T>(_ body: (OpaquePointer) -> T) -> T? {
        lock.lock(); defer { lock.unlock() }
        return board.map(body)
    }

    /// The board, once; nil if already closed.
    func close() -> OpaquePointer? {
        lock.lock(); defer { lock.unlock() }
        defer { board = nil }
        return board
    }
}

/// Something the user should know that does not stop the Model:Cycles (an audio output that would not start, the
/// microphone not allowed), shown in a bar under the panel until it is fixed or dismissed.
struct Notice: Equatable {
    let text: String
    var actionTitle: String? = nil                      // a button that opens `actionURL`
    var actionURL: URL? = nil
}

final class Board: ObservableObject, @unchecked Sendable {
    @Published var screen: CGImage?
    @Published var leds: UInt64 = 0
    @Published var error: String?
    @Published var problem: String?                     // the Model:Cycles stopped (Board.problem): the panel says so
    @Published var notices: [String: Notice] = [:]      // by where they come from: "main", "usb out", "usb in"
    @Published var needsFirmware = false                // no OS installed yet: the panel asks for the .syx
    @Published var usbState: Int32 = 0                  // mcb_usb_state
    @Published var controller: String?                  // connected MIDI controller (MiniLab 3)
    /// Where the main output plays (an output device's UID; nil: the system's default output, followed when it
    /// changes). Set in Settings. A chosen device that is not plugged in plays on the default until it comes back.
    @Published var mainOutputUID: String? = UserDefaults.standard.string(forKey: "mainOut") {
        didSet {
            guard mainOutputUID != oldValue else { return }
            UserDefaults.standard.set(mainOutputUID, forKey: "mainOut")
            routeMainOutput()
        }
    }

    private var board: OpaquePointer?
    private var handle: BoardHandle?                    // the board for MIDI and MiniLab calls (closed before destroy)
    private var knobSteps: KnobSteps?                   // knob turns as value steps (scroll, drag, MiniLab)
    private var thread: Thread?
    private var activity: NSObjectProtocol?             // keeps App Nap away while the board runs
    private var timer: Timer?
    private var audio: AudioOut?
    private var audioDevice: AudioDeviceID?             // the device `audio` was started on (nil: the default)
    private var outputsChanged: AnyCancellable?         // the output list changed: the chosen device came or went
    let usbAudio = UsbAudio()                           // USB audio routes (the USB Audio menu)
    private let midi = MidiPort()
    private var lastGen: UInt64 = 0

    // SPOKE_DATA=/some/folder uses another data folder (for tests).
    private static let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
    static let dataDir = ProcessInfo.processInfo.environment["SPOKE_DATA"].map { URL(fileURLWithPath: $0, isDirectory: true) }
        ?? appSupport.appendingPathComponent("Vladiktron Spoke", isDirectory: true)

    /// The app used to be Model Mac, with its data in ModelMac/ and its settings under local.modelmac.app. Moves both
    /// over once, before the board starts, so the +Drive, settings and USB audio routes carry on.
    static func moveModelMacData() {
        let old = appSupport.appendingPathComponent("ModelMac", isDirectory: true), fm = FileManager.default
        if ProcessInfo.processInfo.environment["SPOKE_DATA"] == nil, fm.fileExists(atPath: old.path) {
            if fm.fileExists(atPath: dataDir.path) {
                NSLog("Spoke: both %@ and %@ exist; using the second", old.path, dataDir.path)
            } else {
                do { try fm.moveItem(at: old, to: dataDir) } catch { NSLog("Spoke: could not move %@: %@", old.path, "\(error)") }
            }
        }
        let defaults = UserDefaults.standard, oldDomain = "local.modelmac.app"
        if let settings = defaults.persistentDomain(forName: oldDomain) {
            for (key, value) in settings where defaults.object(forKey: key) == nil { defaults.set(value, forKey: key) }
            defaults.removePersistentDomain(forName: oldDomain)     // once: a route set to nothing must stay nothing
        }
    }

    func start() {
        let os = Board.dataDir.appendingPathComponent("main_os.bin")
        let flash = Board.dataDir.appendingPathComponent("flash.bin")
        let emmc = Board.dataDir.appendingPathComponent("emmc.img")
        guard let data = try? Data(contentsOf: os), FileManager.default.fileExists(atPath: flash.path) else {
            needsFirmware = true                            // the panel shows the first-run screen
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
                if self?.needsFirmware == true { self?.snapshotNow() }
            }
            return
        }
        var err = [CChar](repeating: 0, count: 256)
        let b = data.withUnsafeBytes { raw in
            mcb_create(raw.bindMemory(to: UInt8.self).baseAddress, data.count, flash.path, emmc.path, &err, err.count)
        }
        guard let b else {
            error = "Could not start the board: " + String(cString: err)
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.snapshotNow() }   // SPOKE_SNAPSHOT
            runTestScript()
            return
        }
        board = b
        // App Nap would throttle the CPU thread's timers while the window is hidden, and the audio and MIDI with
        // them. The Mac may still sleep when idle, as it may with a synth plugged in.
        activity = ProcessInfo.processInfo.beginActivity(options: [.userInitiatedAllowingIdleSystemSleep, .latencyCritical],
                                                         reason: "Model:Cycles is running")
        let t = Thread { [b] in _ = mcb_run(b, 0) }
        t.name = "Model:Cycles CPU"
        t.qualityOfService = .userInteractive
        t.start()
        thread = t
        usbAudio.onNotice = { [weak self] key, n in DispatchQueue.main.async { self?.setNotice(key, n) } }
        usbAudio.start(board: b)                            // (lists the outputs the main output is chosen from)
        routeMainOutput()
        outputsChanged = usbAudio.$outputs.dropFirst().receive(on: DispatchQueue.main).sink { [weak self] _ in
            self?.routeMainOutput()
        }
        let h = BoardHandle(b)                              // MiniLab timers may fire after the board is gone
        handle = h
        let steps = KnobSteps(nudge: { e, n in h.with { mcb_encoder_events($0, Int32(e), Int32(n)) } })
        knobSteps = steps
        let panel = MiniLab.Panel(key: { k, down in h.with { mcb_key($0, Int32(k), down ? 1 : 0) } },
                                  turn: { e, n in steps.step(e, n) },
                                  nudge: { e, n in h.with { mcb_encoder_events($0, Int32(e), Int32(n)) } },
                                  pad: { p, v in h.with { mcb_pad($0, Int32(p), v) } },
                                  leds: { h.with { mcb_leds($0) } ?? 0 })
        midi.start(board: h, panel: panel) { [weak self] name in DispatchQueue.main.async { self?.controller = name } }
        let t2 = Timer(timeInterval: 1.0 / 60, repeats: true) { [weak self] _ in self?.refresh() }
        RunLoop.main.add(t2, forMode: .common)              // also while quitting waits (modal run loop mode)
        timer = t2
        runTestScript()
    }

    // Self-check: SPOKE_TEST="8:play,20:out=BuiltInSpeakerDevice,30:out=,60:quit" runs these steps at the
    // given seconds after launch: press PLAY, route USB audio out/in to a device UID (empty: nowhere), open the
    // About panel ("about"), Reset +Drive without asking ("reset": use it with SPOKE_DATA), stop the CPU where it
    // is, as a fault would ("halt"), press the problem overlay's Restart ("restart"), quit.
    private var testScriptRan = false
    private func runTestScript() {
        guard !testScriptRan, let script = ProcessInfo.processInfo.environment["SPOKE_TEST"] else { return }
        testScriptRan = true                                // once per launch, not again after a restart
        usbAudio.remember = false                           // routes it sets are not the user's settings
        for step in script.split(separator: ",") {
            let parts = step.split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2, let at = Double(parts[0]) else { continue }
            let kv = parts[1].split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false).map(String.init)
            let arg = kv.count > 1 && !kv[1].isEmpty ? kv[1] : nil
            DispatchQueue.main.asyncAfter(deadline: .now() + at) { [weak self] in
                guard let self else { return }
                NSLog("Spoke test: %@", parts[1])
                switch kv[0] {
                case "play": self.key(7, down: true); DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { self.key(7, down: false) }
                case "out": self.usbAudio.outputUID = arg
                case "in": self.usbAudio.inputUID = arg
                case "about": AppDelegate.showAbout()
                case "halt": if let b = self.board { mcb_stop(b) }
                case "restart": self.restartAfterProblem { NSLog("Spoke test: restarted, problem: %@", self.problem ?? "none") }
                case "reset": self.resetDrive { NSLog("Spoke test: reset: %@", $0.map { "\($0)" } ?? "ok") }
                case "quit": RunLoop.main.perform { NSApp.terminate(nil) }   // not inside this main-queue
                                                                             // block: the shutdown replies on it
                default: break
                }
            }
        }
    }

    /// (Re)starts the main output on the device chosen in Settings, if it is plugged in, else on the default. Does
    /// nothing if that is where it plays already.
    private func routeMainOutput() {
        guard let b = board else { return }
        let device = mainOutputUID.flatMap { uid in AudioDevices.list(input: false).first { $0.uid == uid }?.id }
        if audio != nil && device == audioDevice { return }
        audio?.stop()
        setNotice("main", nil)
        let main = AudioOut(name: "main", read: { mcb_audio(b, $0, $1) }, available: { mcb_audio_available(b) })
        main.onProblem = { [weak self] p in DispatchQueue.main.async { self?.setNotice("main", p.map { Notice(text: $0) }) } }
        main.start(device: device)
        audio = main
        audioDevice = device
    }

    var running: Bool { board != nil }
    private var shuttingDown = false
    private var afterShutdown: [() -> Void] = []        // `done` of every shutDown asked for while one runs
    /// The app is quitting: a restart under way leaves the board off (its change is still made).
    var quitting = false

    /// Clean shutdown, as on the hardware: holds the power key until the OS has saved its working state to the
    /// +Drive and powered off (about 2 s; the screen shows "Shutting down..."), then closes the images.
    /// After `timeout` seconds the CPU is stopped wherever it is. Calls `done` on the main thread.
    func shutDown(timeout: TimeInterval = 5, done: @escaping () -> Void) {
        guard let b = board, let t = thread else { done(); return }
        let timeout = problem != nil ? 0 : timeout          // a stopped OS saves nothing: don't wait for it
        afterShutdown.append(done)
        guard !shuttingDown else { return }                 // one at a time (quit during a reset, say): this
        shuttingDown = true                                 // `done` runs when the one under way ends
        mcb_power_key(b, 1)
        DispatchQueue.global(qos: .userInitiated).async { [self] in
            let end = Date().addingTimeInterval(timeout)
            while !t.isFinished && Date() < end { usleep(5000) }
            if mcb_powered_off(b) == 0 { NSLog("Spoke: the OS did not power off within %.0f s; stopping it", timeout) }
            DispatchQueue.main.async {
                self.stop()
                self.shuttingDown = false
                let waiting = self.afterShutdown
                self.afterShutdown = []
                for done in waiting { done() }
            }
        }
    }

    /// Stops the CPU wherever it is (after shutDown it has already stopped), then flushes and closes the images.
    func stop() {
        guard let b = board else { return }
        timer?.invalidate()
        timer = nil
        outputsChanged = nil
        audio?.stop()
        audio = nil
        audioDevice = nil
        usbAudio.stop()
        midi.stop()
        _ = handle?.close()                                 // MIDI and MiniLab calls from now on do nothing
        handle = nil
        mcb_stop(b)
        while thread?.isFinished == false { usleep(1000) }
        thread = nil
        mcb_destroy(b)
        board = nil
        if let activity { ProcessInfo.processInfo.endActivity(activity) }
        activity = nil
        notices = [:]                                       // about outputs and inputs that are closed now
    }

    func setNotice(_ key: String, _ notice: Notice?) {
        if notices[key] != notice { notices[key] = notice }
    }

    /// Shuts down cleanly, runs `change` on the data folder (with the board closed), then boots again.
    /// `done` gets the error `change` threw, if any (on the main thread).
    func restart(changing change: @escaping (URL) throws -> Void, done: @escaping (Error?) -> Void) {
        shutDown { [self] in
            var failure: Error?
            do { try change(Board.dataDir) } catch { failure = error }
            screen = nil
            leds = 0
            lastGen = 0
            error = nil
            problem = nil
            if !quitting { start() }
            done(failure)
        }
    }

    /// After a problem (or a board that would not start): boot again, changing nothing.
    func restartAfterProblem(done: @escaping () -> Void = {}) {
        restart(changing: { _ in }, done: { _ in done() })
    }

    /// Reset +Drive: a fresh, empty eMMC image; the OS formats it at boot. Settings (flash.bin) stay.
    func resetDrive(done: @escaping (Error?) -> Void) {
        restart(changing: { dir in try Board.remove(["emmc.img", "emmc.img.extcsd"], in: dir) }, done: done)
    }

    static var factoryFlash: URL { dataDir.appendingPathComponent("flash.factory.bin") }

    /// Installs the OS from the user's own .syx (mcfirmware.h): main_os.bin, flash.factory.bin, and flash.bin if
    /// there is none. Only Elektron's OS 1.13 passes; `done` gets the reason a file didn't (on the main thread).
    /// A running board needs no restart: it has its OS in memory, and flash.bin is never replaced.
    /// Elektron's download is a .zip with the .syx inside; either one will do.
    func installFirmware(from file: URL, done: @escaping (String?) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async { [self] in
            var err = [CChar](repeating: 0, count: 256)
            var rc: Int32 = -1
            var unzipped: URL?
            if file.pathExtension.lowercased() == "zip" {
                unzipped = Board.unzip(file)
                if let syx = unzipped.flatMap(Board.findSyx) {
                    rc = mcf_install(syx.path, Board.dataDir.path, &err, err.count)
                } else {
                    strlcpy(&err, "There is no .syx file in this .zip.", err.count)
                }
            } else {
                rc = mcf_install(file.path, Board.dataDir.path, &err, err.count)
            }
            if let unzipped { try? FileManager.default.removeItem(at: unzipped) }
            DispatchQueue.main.async { [self] in
                guard rc == 0 else { done(String(cString: err)); return }
                if board == nil && !quitting {
                    needsFirmware = false
                    error = nil
                    start()
                }
                done(nil)
            }
        }
    }

    /// Unpacks a .zip into a new temporary folder (nil if ditto fails).
    private static func unzip(_ zip: URL) -> URL? {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("Spoke-" + UUID().uuidString)
        let ditto = Process()
        ditto.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        ditto.arguments = ["-x", "-k", zip.path, dir.path]
        guard (try? ditto.run()) != nil else { return nil }
        ditto.waitUntilExit()
        return ditto.terminationStatus == 0 ? dir : nil
    }

    private static func findSyx(in dir: URL) -> URL? {
        let all = FileManager.default.enumerator(at: dir, includingPropertiesForKeys: nil)?.compactMap { $0 as? URL } ?? []
        return all.first { $0.pathExtension.lowercased() == "syx" && !$0.path.contains("__MACOSX") }
    }

    /// Factory reset: a fresh +Drive, and flash.bin (settings) back to the image installed from the .syx
    /// (check `factoryFlash` exists first).
    func factoryReset(done: @escaping (Error?) -> Void) {
        restart(changing: { dir in
            let flash = dir.appendingPathComponent("flash.bin"), fresh = dir.appendingPathComponent("flash.bin.new")
            try Board.remove(["flash.bin.new"], in: dir)
            try FileManager.default.copyItem(at: Board.factoryFlash, to: fresh)   // copy first: never no flash.bin
            try Board.remove(["emmc.img", "emmc.img.extcsd"], in: dir)
            if FileManager.default.fileExists(atPath: flash.path) {
                _ = try FileManager.default.replaceItemAt(flash, withItemAt: fresh)
            } else {
                try FileManager.default.moveItem(at: fresh, to: flash)
            }
        }, done: done)
    }

    private static func remove(_ names: [String], in dir: URL) throws {
        for name in names {
            let url = dir.appendingPathComponent(name)
            if FileManager.default.fileExists(atPath: url.path) { try FileManager.default.removeItem(at: url) }
        }
    }

    // MARK: front panel input

    func key(_ index: Int, down: Bool) {
        if let b = board { mcb_key(b, Int32(index), down ? 1 : 0) }
    }

    /// A short press: the key is down for 80 ms, long enough for the OS's key scan to see it.
    func tap(_ index: Int) {
        key(index, down: true)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.08) { [weak self] in self?.key(index, down: false) }
    }

    /// `steps` value steps (one per scroll click or drag step; see KnobSteps).
    func turn(_ encoder: Int, steps: Int) {
        knobSteps?.step(encoder, steps)
    }

    func pad(_ pad: Int, pressure: Float) {
        if let b = board { mcb_pad(b, Int32(pad), pressure) }
    }

    // MARK: output

    private func refresh() {
        guard let b = board else { return }
        var fb = [UInt8](repeating: 0, count: 1024)
        let gen = mcb_screen(b, &fb)
        if gen != lastGen {
            lastGen = gen
            screen = Board.render(fb)
        }
        let l = mcb_leds(b)
        if l != leds { leds = l }
        let u = mcb_usb_state(b)
        if u != usbState { usbState = u }
        if (problem == nil || problemIsHang) && !shuttingDown {   // (a hang that passes clears its message)
            let stopped = thread?.isFinished == true            // mcb_run returned by itself: a fault
            var why = [CChar](repeating: 0, count: 256)
            if stopped { _ = mcb_fault(b, &why, why.count) }
            let busy = stopped ? 0 : mcb_busy_seconds(b)
            let p = Board.problem(cpuStopped: stopped, fault: String(cString: why), osExceptions: mcb_os_exceptions(b),
                                  busySeconds: busy)
            if p != problem {
                NSLog("Spoke: %@", p ?? "the OS responds again")
                problem = p
                problemIsHang = p != nil && !stopped && mcb_os_exceptions(b) == 0
            }
        }
        snapshotIfRequested()
    }

    private var problemIsHang = false
    /// The OS has run this long without a pause in its idle loop: it has stopped responding. (OS 1.13's longest busy
    /// stretch, measured: 0.14 s, formatting a new +Drive at boot; playing, pads, knobs and saving at power-off stay under
    /// 0.01 s.)
    static let hangSeconds = 10.0

    /// What stopped the Model:Cycles, in words for the panel; nil while it runs. `fault` is mcb_fault's reason,
    /// `busySeconds` mcb_busy_seconds.
    static func problem(cpuStopped: Bool, fault: String, osExceptions: UInt64, busySeconds: Double) -> String? {
        if osExceptions > 0 {
            return "The Model:Cycles OS crashed: it showed EXCEPTION on its screen and stopped."
        }
        if cpuStopped {
            return fault.isEmpty ? "The emulator stopped." : "The emulator stopped: \(fault)."
        }
        if busySeconds >= hangSeconds {
            return String(format: "The Model:Cycles OS stopped responding: it has been busy for over %.0f s without a pause.",
                          hangSeconds)
        }
        return nil
    }

    /// The board's one-line diagnostics (mcb_status), made when asked. Not published: its uptime changes every
    /// frame, and publishing it redrew the whole panel 60 times a second.
    var status: String {
        guard let b = board else { return "Starting…" }
        var st = [CChar](repeating: 0, count: 512)
        mcb_status(b, &st, st.count)
        return String(cString: st)
    }

    // 128x64, column-major, pixel (x, y) = bit 7-(y&7) of byte x*8 + y/8, y = 0 at the bottom.
    static func render(_ fb: [UInt8]) -> CGImage? {
        let w = 128, h = 64
        var px = [UInt8](repeating: 0, count: w * h * 4)
        for row in 0..<h {
            let y = 63 - row
            for x in 0..<w {
                let on = (fb[x * 8 + y / 8] >> (7 - (y & 7))) & 1 == 1
                let i = (row * w + x) * 4
                let (r, g, b): (UInt8, UInt8, UInt8) = on ? (38, 40, 44) : (214, 218, 224)
                px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255
            }
        }
        let cs = CGColorSpaceCreateDeviceRGB()
        return px.withUnsafeMutableBytes { buf in
            CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                      space: cs, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)?.makeImage()
        }
    }

    // Self-check: SPOKE_SNAPSHOT=/path.png saves the rendered window content (10 s after launch, or when
    // snapshotNow() is called by the demo).
    var snapshotView: (() -> NSView?)?
    private var snapshotDone = false
    private let launched = Date()
    private func snapshotIfRequested() {
        guard ProcessInfo.processInfo.environment["SPOKE_DEMO"] == nil,
              Date().timeIntervalSince(launched) > 10 else { return }
        snapshotNow()
    }
    func snapshotNow() {
        guard !snapshotDone, let path = ProcessInfo.processInfo.environment["SPOKE_SNAPSHOT"],
              let view = snapshotView?() else { return }
        snapshotDone = true
        guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
    }
}
