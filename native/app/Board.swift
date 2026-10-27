// Bridge to the C virtual board (mcboard.h): runs the OS on its own thread, publishes screen + LEDs.
import AppKit
import SwiftUI

final class Board: ObservableObject, @unchecked Sendable {
    @Published var screen: CGImage?
    @Published var leds: UInt64 = 0
    @Published var status = "Starting…"
    @Published var error: String?
    @Published var needsFirmware = false                // no OS installed yet: the panel asks for the .syx
    @Published var usbState: Int32 = 0                  // mcb_usb_state
    @Published var controller: String?                  // connected MIDI controller (MiniLab 3)

    private var board: OpaquePointer?
    private var thread: Thread?
    private var activity: NSObjectProtocol?             // keeps App Nap away while the board runs
    private var timer: Timer?
    private var audio: AudioOut?
    let usbAudio = UsbAudio()                           // USB audio routes (the USB Audio menu)
    private let midi = MidiPort()
    private var lastGen: UInt64 = 0

    // MODELMAC_DATA=/some/folder uses another data folder (for tests).
    static let dataDir = ProcessInfo.processInfo.environment["MODELMAC_DATA"].map { URL(fileURLWithPath: $0, isDirectory: true) }
        ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("ModelMac", isDirectory: true)

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
        let main = AudioOut(name: "main", read: { mcb_audio(b, $0, $1) }, available: { mcb_audio_available(b) })
        main.start()
        audio = main
        usbAudio.start(board: b)
        let panel = MiniLab.Panel(key: { mcb_key(b, Int32($0), $1 ? 1 : 0) },
                                  turn: { mcb_encoder(b, Int32($0), Int32($1)) },
                                  pad: { mcb_pad(b, Int32($0), $1) })
        midi.start(board: b, panel: panel) { [weak self] name in DispatchQueue.main.async { self?.controller = name } }
        let t2 = Timer(timeInterval: 1.0 / 60, repeats: true) { [weak self] _ in self?.refresh() }
        RunLoop.main.add(t2, forMode: .common)              // also while quitting waits (modal run loop mode)
        timer = t2
        runTestScript()
    }

    // Self-check: MODELMAC_TEST="8:play,20:out=BuiltInSpeakerDevice,30:out=,60:quit" runs these steps at the
    // given seconds after launch: press PLAY, route USB audio out/in to a device UID (empty: nowhere), open the
    // About panel ("about"), quit.
    private func runTestScript() {
        guard let script = ProcessInfo.processInfo.environment["MODELMAC_TEST"] else { return }
        for step in script.split(separator: ",") {
            let parts = step.split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2, let at = Double(parts[0]) else { continue }
            let kv = parts[1].split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false).map(String.init)
            let arg = kv.count > 1 && !kv[1].isEmpty ? kv[1] : nil
            DispatchQueue.main.asyncAfter(deadline: .now() + at) { [weak self] in
                guard let self else { return }
                NSLog("Model Mac test: %@", parts[1])
                switch kv[0] {
                case "play": self.key(7, down: true); DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { self.key(7, down: false) }
                case "out": self.usbAudio.outputUID = arg
                case "in": self.usbAudio.inputUID = arg
                case "about": AppDelegate.showAbout()
                case "quit": RunLoop.main.perform { NSApp.terminate(nil) }   // not inside this main-queue
                                                                             // block: the shutdown replies on it
                default: break
                }
            }
        }
    }

    var running: Bool { board != nil }
    private var shuttingDown = false

    /// Clean shutdown, as on the hardware: holds the power key until the OS has saved its working state to the
    /// +Drive and powered off (about 2 s; the screen shows "Shutting down..."), then closes the images.
    /// After `timeout` seconds the CPU is stopped wherever it is. Calls `done` on the main thread.
    func shutDown(timeout: TimeInterval = 5, done: @escaping () -> Void) {
        guard let b = board, let t = thread else { done(); return }
        guard !shuttingDown else { return }                 // one at a time (quit during a reset, say)
        shuttingDown = true
        mcb_power_key(b, 1)
        DispatchQueue.global(qos: .userInitiated).async { [self] in
            let end = Date().addingTimeInterval(timeout)
            while !t.isFinished && Date() < end { usleep(5000) }
            if mcb_powered_off(b) == 0 { NSLog("Model Mac: the OS did not power off within %.0f s; stopping it", timeout) }
            DispatchQueue.main.async { self.stop(); self.shuttingDown = false; done() }
        }
    }

    /// Stops the CPU wherever it is (after shutDown it has already stopped), then flushes and closes the images.
    func stop() {
        guard let b = board else { return }
        timer?.invalidate()
        timer = nil
        audio?.stop()
        audio = nil
        usbAudio.stop()
        midi.stop()
        mcb_stop(b)
        while thread?.isFinished == false { usleep(1000) }
        thread = nil
        mcb_destroy(b)
        board = nil
        if let activity { ProcessInfo.processInfo.endActivity(activity) }
        activity = nil
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
            status = "Starting…"
            start()
            done(failure)
        }
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
                if board == nil {
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
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("ModelMac-" + UUID().uuidString)
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

    func turn(_ encoder: Int, steps: Int) {
        if let b = board, steps != 0 { mcb_encoder(b, Int32(encoder), Int32(steps)) }
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
        var st = [CChar](repeating: 0, count: 512)
        mcb_status(b, &st, st.count)
        status = String(cString: st)
        snapshotIfRequested()
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

    // Self-check: MODELMAC_SNAPSHOT=/path.png saves the rendered window content (10 s after launch, or when
    // snapshotNow() is called by the demo).
    var snapshotView: (() -> NSView?)?
    private var snapshotDone = false
    private let launched = Date()
    private func snapshotIfRequested() {
        guard ProcessInfo.processInfo.environment["MODELMAC_DEMO"] == nil,
              Date().timeIntervalSince(launched) > 10 else { return }
        snapshotNow()
    }
    func snapshotNow() {
        guard !snapshotDone, let path = ProcessInfo.processInfo.environment["MODELMAC_SNAPSHOT"],
              let view = snapshotView?() else { return }
        snapshotDone = true
        guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
    }
}
