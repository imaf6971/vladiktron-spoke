// Bridge to the C virtual board (mcboard.h): runs the OS on its own thread, publishes screen + LEDs.
import AppKit
import SwiftUI

final class Board: ObservableObject, @unchecked Sendable {
    @Published var screen: CGImage?
    @Published var leds: UInt64 = 0
    @Published var status = "Starting…"
    @Published var error: String?
    @Published var usbState: Int32 = 0                  // mcb_usb_state
    @Published var controller: String?                  // connected MIDI controller (MiniLab 3)

    private var board: OpaquePointer?
    private var thread: Thread?
    private var timer: Timer?
    private var audio: AudioOut?
    let usbAudio = UsbAudio()                           // USB audio routes (the USB Audio menu)
    private let midi = MidiPort()
    private var lastGen: UInt64 = 0

    static let dataDir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("ModelMac", isDirectory: true)

    func start() {
        let os = Board.dataDir.appendingPathComponent("main_os.bin")
        let flash = Board.dataDir.appendingPathComponent("flash.bin")
        let emmc = Board.dataDir.appendingPathComponent("emmc.img")
        guard let data = try? Data(contentsOf: os), FileManager.default.fileExists(atPath: flash.path) else {
            error = "Firmware not found in \(Board.dataDir.path).\nRun native/build.sh with your model-cycles_OS1.13.syx first."
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
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { [weak self] _ in self?.refresh() }
    }

    func stop() {
        guard let b = board else { return }
        audio?.stop()
        audio = nil
        usbAudio.stop()
        midi.stop()
        mcb_stop(b)
        while thread?.isFinished == false { usleep(1000) }
        mcb_destroy(b)
        board = nil
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
