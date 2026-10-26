// Arturia MiniLab 3 as a hands-on controller for the board, with its factory settings (MIDI Control Center
// "Default" user program): keys on channel 1, pads on channel 10 (bank A notes 36-43, bank B 44-51), knobs
// CC 74 71 76 77 93 18 19 16, faders CC 82 83 85 17, pitch strip = pitch bend, mod strip = CC 1.
//
//   keys                 play the active track (sent to the Model:Cycles' auto channel)
//   pads A1-A6           the six pads, with velocity and pressure
//   pad A7 / A8          PLAY / STOP
//   pads B1-B6           select track 1-6 (TRACK + pad)
//   pad B7 / B8          RECORD / TAP TEMPO
//   knobs 1-8            PITCH DECAY COLOR SHAPE / SWEEP CONTOUR DELAY REVERB
//   faders 1-4           LFO SPEED, VOLUME + DIST, SWING, CHANCE
//   pitch / mod strips   pitch bend and CC 1 on the auto channel
//
// Knobs and faders send absolute values; each change of one becomes one encoder detent on the Model:Cycles.
import CoreMIDI
import Foundation

final class MiniLab {
    /// What the mapping drives: the board's front panel (thread-safe C calls underneath).
    struct Panel {
        let key: (Int, Bool) -> Void
        let turn: (Int, Int) -> Void
        let pad: (Int, Float) -> Void
    }

    static let autoChannel: UInt8 = 9                 // Model:Cycles AUTO channel 10 (0-based 9)
    static let padChannel: UInt8 = 9                  // MiniLab pads, channel 10
    static let knobCC: [UInt8: Int] = [74: 2, 71: 3, 76: 4, 77: 5, 93: 6, 18: 7, 19: 8, 16: 9,   // CC -> encoder
                                       82: 10, 83: 11, 85: 12, 17: 13]
    // Panel key indexes (PanelView's Panel enum): PLAY 7, STOP 5, RECORD 9, TAP 4, TRACK 13
    static let padKeys: [UInt8: Int] = [42: 7, 43: 5, 50: 9, 51: 4]

    private let panel: Panel
    private let notes: MidiPortPair
    private let onChange: (String?) -> Void
    private var client = MIDIClientRef()
    private var port = MIDIPortRef()
    private var own: Set<MIDIEndpointRef> = []
    private var connected: [MIDIEndpointRef: String] = [:]
    private var parsers: [MIDIEndpointRef: MidiStreamParser] = [:]
    private var lastCC: [UInt8: Int] = [:]
    private var heldPads: Set<Int> = []
    private let lock = NSLock()

    init(panel: Panel, notes: MidiPortPair, onChange: @escaping (String?) -> Void) {
        self.panel = panel
        self.notes = notes
        self.onChange = onChange
    }

    func attach(client: MIDIClientRef, ownSources: [MIDIEndpointRef]) {
        self.client = client
        own = Set(ownSources)
        MIDIInputPortCreateWithBlock(client, "MiniLab" as CFString, &port) { [weak self] list, src in
            guard let self else { return }
            let endpoint = MIDIEndpointRef(UInt(bitPattern: src))
            for packet in list.unsafeSequence() {
                let bytes = UnsafeRawPointer(packet) + MemoryLayout<MIDIPacket>.offset(of: \MIDIPacket.data)!
                let p = bytes.assumingMemoryBound(to: UInt8.self)
                self.lock.lock()
                var parser = self.parsers[endpoint] ?? MidiStreamParser()
                var msgs: [[UInt8]] = []
                for i in 0..<Int(packet.pointee.length) { if let m = parser.feed(p[i]) { msgs.append(m) } }
                self.parsers[endpoint] = parser
                for m in msgs { self.handle(m) }
                self.lock.unlock()
            }
        }
        rescan()
    }

    func detach() {
        for e in connected.keys { MIDIPortDisconnectSource(port, e) }
        connected = [:]
        if port != 0 { MIDIPortDispose(port) }
        port = 0
    }

    /// Connects to every MiniLab 3 MIDI source (not its DAW / MCU / DIN THRU ports); runs again on setup changes.
    func rescan() {
        guard port != 0 else { return }
        var found: [MIDIEndpointRef: String] = [:]
        for i in 0..<MIDIGetNumberOfSources() {
            let e = MIDIGetSource(i)
            guard !own.contains(e), let name = MiniLab.name(e), MiniLab.isMiniLab(name) else { continue }
            found[e] = name
        }
        for e in connected.keys where found[e] == nil { MIDIPortDisconnectSource(port, e) }
        for e in found.keys where connected[e] == nil {
            MIDIPortConnectSource(port, e, UnsafeMutableRawPointer(bitPattern: UInt(e)))   // refCon = endpoint
        }
        let changed = Set(found.keys) != Set(connected.keys)
        connected = found
        if changed { onChange(found.values.sorted().first) }
    }

    static func isMiniLab(_ name: String) -> Bool {
        let n = name.lowercased()
        guard n.contains("minilab") else { return false }
        return !(n.contains("daw") || n.contains("mcu") || n.contains("thru"))
    }

    private static func name(_ e: MIDIEndpointRef) -> String? {
        var s: Unmanaged<CFString>?
        guard MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s) == noErr, let v = s?.takeRetainedValue() else { return nil }
        return v as String
    }

    // MARK: mapping

    /// One whole message from the MiniLab.
    func handle(_ m: [UInt8]) {
        guard let status = m.first, status < 0xF0 else { return }
        let kind = status & 0xF0, ch = status & 0x0F
        let d1 = m.count > 1 ? m[1] : 0, d2 = m.count > 2 ? m[2] : 0
        if ch == MiniLab.padChannel && (kind == 0x90 || kind == 0x80 || kind == 0xA0) && (36...51).contains(d1) {
            pad(note: d1, velocity: kind == 0x80 ? 0 : d2, pressure: kind == 0xA0)
            return
        }
        switch kind {
        case 0x80, 0x90, 0xE0:                                   // keys and pitch bend: the active track
            notes.send([kind | MiniLab.autoChannel] + m.dropFirst())
        case 0xB0:
            if let enc = MiniLab.knobCC[d1] {
                let v = Int(d2)
                if let last = lastCC[d1], v != last { panel.turn(enc, v - last) }
                lastCC[d1] = v
            } else if d1 == 1 || d1 == 64 {                      // mod strip, sustain
                notes.send([0xB0 | MiniLab.autoChannel, d1, d2])
            }
        case 0xD0 where ch == MiniLab.padChannel:                // channel pressure on the pads
            for p in heldPads { panel.pad(p, max(0.05, Float(d1) / 127)) }
        default:
            break
        }
    }

    private func pad(note: UInt8, velocity: UInt8, pressure: Bool) {
        let down = velocity > 0
        switch note {
        case 36...41:                                            // pads A1-A6: the Model:Cycles pads
            let p = Int(note - 35)
            if pressure && !heldPads.contains(p) { return }
            panel.pad(p, down ? max(0.05, Float(velocity) / 127) : 0)
            if pressure { return }
            if down { heldPads.insert(p) } else { heldPads.remove(p) }
        case 44...49 where !pressure && down:                    // pads B1-B6: a TRACK + pad tap selects a track
            let p = Int(note - 43), q = DispatchQueue.global(qos: .userInteractive)
            panel.key(13, true)
            q.asyncAfter(deadline: .now() + 0.02) { [panel] in panel.pad(p, 0.6) }
            q.asyncAfter(deadline: .now() + 0.08) { [panel] in panel.pad(p, 0) }
            q.asyncAfter(deadline: .now() + 0.11) { [panel] in panel.key(13, false) }
        default:
            if !pressure, let key = MiniLab.padKeys[note] { panel.key(key, down) }
        }
    }
}
