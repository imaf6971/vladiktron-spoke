// Arturia MiniLab 3 as a hands-on controller for the board. It works in two ways, following the MiniLab's own mode:
//
// Arturia mode, factory settings (MIDI Control Center "Default" user program): keys on channel 1, pads on channel
// 10 (bank A notes 36-43, bank B 44-51), knobs CC 74 71 76 77 93 18 19 16, faders CC 82 83 85 17, pitch strip =
// pitch bend, mod strip = CC 1.
//
//   keys                 play the active track (sent to the Model:Cycles' auto channel)
//   pads A1-A6           the six pads, with velocity and pressure
//   pad A7 / A8          PLAY / STOP
//   pads B1-B6           select track 1-6 (TRACK + pad)
//   pad B7 / B8          RECORD / TAP TEMPO
//   knobs 1-8            PITCH DECAY COLOR SHAPE / SWEEP CONTOUR DELAY REVERB
//   faders 1-4           LFO SPEED, VOLUME + DIST, SWING, CHANCE (the fader position is the value)
//   pitch / mod strips   pitch bend and CC 1 on the auto channel
//
// DAW mode (Shift + Prog on the MiniLab): the whole front panel, with the pad colours following the Model:Cycles'
// LEDs and names on the MiniLab's display. Everything comes in on the "Minilab3 MIDI" port: knobs CC 86 87 89 90
// 110 111 116 117, faders CC 14 15 30 31, main encoder CC 28 (relative, 64 +/- n) and its press CC 118, Shift CC 27,
// Shift + main encoder CC 29 / CC 119, pads as above, Shift + pads 4-8 CC 105-109 (Shift + pads 1-3 are the
// MiniLab's own bank and program switches). The MiniLab reports mode and bank changes as SysEx.
//
//   Shift                FUNC (held while Shift is held)
//   main encoder         LEVEL/DATA, press = LEVEL/DATA press
//   Shift + encoder press  keys: PANEL (the default) <-> NOTES (play the active track)
//   knobs 1-8            PITCH DECAY COLOR SHAPE / SWEEP CONTOUR DELAY REVERB, endless
//   faders 1-4           LFO SPEED, VOLUME + DIST, SWING, CHANCE: the fader position is the value
//   pads A1-A6, A7, A8   the six pads, PLAY, STOP
//   pads B1-B8           RECORD PATTERN TRACK PAGE / RETRIG LFO-SETUP PAD-MENU PRESET-MENU
//   PANEL keys           (octave at 0) F3-G#4 = trigs 1-16; C3-E3 = QUANTIZE CLICK TEMPO TEMP-SAVE FUNC;
//                        A4-C5 = PLAY STOP RECORD TRACK
//
// Knobs send absolute values; each change of one becomes one encoder detent on the Model:Cycles. In DAW mode each
// value a knob or the main encoder moves is one value step on the Model:Cycles, sent as single OS knob events (see
// step()); turned fast, the MiniLab skips values and the OS speeds up on its own. The app also sets a knob back to
// 64 when it nears either end, so the knobs never run out. Faders, in both modes, go to the active track as MIDI CC,
// so the fader position is the value (needs the Model:Cycles' MIDI settings to receive CC, the factory setting).
import CoreMIDI
import Foundation

private typealias Front = Panel                       // PanelView's LED and key tables (MiniLab.Panel hides the name)

final class MiniLab {
    /// What the mapping drives: the board's front panel (thread-safe C calls underneath).
    struct Panel {
        let key: (Int, Bool) -> Void
        let turn: (Int, Int) -> Void                  // encoder, detents
        let nudge: (Int, Int) -> Void                 // encoder, OS knob events (half detents)
        let pad: (Int, Float) -> Void
        let leds: () -> UInt64
    }

    static let autoChannel: UInt8 = 9                 // Model:Cycles AUTO channel 10 (0-based 9)
    static let padChannel: UInt8 = 9                  // MiniLab pads, channel 10
    static let knobCC: [UInt8: Int] = [74: 2, 71: 3, 76: 4, 77: 5, 93: 6, 18: 7, 19: 8, 16: 9,   // CC -> encoder
                                       82: 10, 83: 11, 85: 12, 17: 13]
    // Panel key indexes (mcboard.h): PLAY 7, STOP 5, RECORD 9, TAP 4, TRACK 13
    static let padKeys: [UInt8: Int] = [42: 7, 43: 5, 50: 9, 51: 4]

    // DAW mode
    static let dawKnobCC: [UInt8] = [86, 87, 89, 90, 110, 111, 116, 117]         // knobs 1-8 -> encoders 2-9
    static let dawFaderCC: [UInt8: Int] = [14: 10, 15: 11, 30: 12, 31: 13]
    // Faders set their parameter outright, as MIDI CC on the auto channel (the OS's parameter descriptors at
    // 0x4010e1b0..: LFO SPEED 102, VOLUME + DIST 7, SWING 15 (0-30), CHANCE 14 (0-100)): encoder -> CC
    static let faderParamCC: [Int: UInt8] = [10: 102, 11: 7, 12: 15, 13: 14]
    static let encoderNames = ["", "LEVEL", "PITCH", "DECAY", "COLOR", "SHAPE", "SWEEP", "CONTOUR", "DELAY",
                               "REVERB", "LFO SPEED", "VOL+DIST", "SWING", "CHANCE"]
    static let bankAKeys = [6: 7, 7: 5]                                           // pads A7 / A8 (0-based): PLAY, STOP
    static let bankBKeys = [9, 11, 13, 30, 12, 3, 8, 0]                           // pads B1-B8
    // PANEL keys, octave 0 (lowest key C3 = 48): note -> panel key
    static let panelMap: [UInt8: Int] = {
        var m: [UInt8: Int] = [:]
        for (i, k) in [1, 2, 4, 6, 10].enumerated() { m[48 + UInt8(i)] = k }   // C3-E3
        for i in 0..<16 { m[53 + UInt8(i)] = 14 + i }                          // F3-G#4: trigs 1-16
        for (i, k) in [7, 5, 9, 13].enumerated() { m[69 + UInt8(i)] = k }      // A4-C5
        return m
    }()
    static let sysex: [UInt8] = [0xF0, 0x00, 0x20, 0x6B, 0x7F, 0x42]
    static let trackColors: [(UInt8, UInt8, UInt8)] = [(127, 20, 20), (127, 70, 0), (110, 110, 0),
                                                       (0, 110, 30), (0, 70, 127), (90, 0, 127)]

    private let panel: Panel
    private let notes: MidiPortPair
    private let onChange: (String?) -> Void
    private var client = MIDIClientRef()
    private var port = MIDIPortRef()
    private var outPort = MIDIPortRef()
    private var own: Set<MIDIEndpointRef> = []
    private var connected: [MIDIEndpointRef: String] = [:]
    private var destination = MIDIEndpointRef()
    private var parsers: [MIDIEndpointRef: MidiStreamParser] = [:]
    private var lastCC: [UInt8: Int] = [:]
    private var heldPads: Set<Int> = []
    private let lock = NSLock()

    // DAW mode state (under lock)
    private var daw = false
    private var panelKeysOn = true                    // keys: PANEL instead of NOTES
    private var heldKeys: [UInt8: Int] = [:]          // PANEL key note -> panel key, so a release always matches
    private var heldPadKeys: [UInt8: Int] = [:]       // pad note or CC -> panel key
    private var sentColors: [UInt8: (UInt8, UInt8, UInt8)] = [:]
    private var ledTimer: DispatchSourceTimer?
    private let ledQueue = DispatchQueue(label: "Vladiktron Spoke MiniLab LEDs", qos: .userInteractive)

    init(panel: Panel, notes: MidiPortPair, onChange: @escaping (String?) -> Void) {
        self.panel = panel
        self.notes = notes
        self.onChange = onChange
    }

    func attach(client: MIDIClientRef, ownSources: [MIDIEndpointRef]) {
        self.client = client
        own = Set(ownSources)
        MIDIOutputPortCreate(client, "MiniLab out" as CFString, &outPort)
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
        let t = DispatchSource.makeTimerSource(queue: ledQueue)
        t.schedule(deadline: .now(), repeating: .milliseconds(33), leeway: .milliseconds(5))
        t.setEventHandler { [weak self] in self?.refreshLEDs() }
        t.resume()
        ledTimer = t
        rescan()
    }

    func detach() {
        ledTimer?.cancel()
        ledTimer = nil
        ledQueue.sync {}
        lock.lock()
        if daw { send(MiniLab.sysex + [0x02, 0x00, 0x40, 0x6A, 0x20, 0xF7]) }      // hand the MiniLab back
        daw = false
        lock.unlock()
        for e in connected.keys { MIDIPortDisconnectSource(port, e) }
        connected = [:]
        if port != 0 { MIDIPortDispose(port) }
        if outPort != 0 { MIDIPortDispose(outPort) }
        port = 0; outPort = 0
    }

    /// Connects to every MiniLab 3 MIDI source (not its DAW / MCU / DIN THRU / ALV ports); runs again on setup changes.
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
        let dest = (0..<MIDIGetNumberOfDestinations()).map(MIDIGetDestination)
            .first { MiniLab.name($0).map(MiniLab.isMiniLab) == true } ?? 0
        lock.lock()
        let newDest = dest != destination
        destination = dest
        if dest == 0 { daw = false }
        if newDest && dest != 0 { hello() }
        lock.unlock()
        if changed { onChange(found.values.sorted().first) }
    }

    static func isMiniLab(_ name: String) -> Bool {
        let n = name.lowercased()
        guard n.contains("minilab") else { return false }
        return !(n.contains("daw") || n.contains("mcu") || n.contains("thru") || n.contains("alv"))
    }

    private static func name(_ e: MIDIEndpointRef) -> String? {
        var s: Unmanaged<CFString>?
        guard MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s) == noErr, let v = s?.takeRetainedValue() else { return nil }
        return v as String
    }

    // MARK: mapping

    /// One whole message from the MiniLab (called with the lock held).
    func handle(_ m: [UInt8]) {
        guard let status = m.first else { return }
        if status == 0xF0 { sysexIn(m); return }
        guard status < 0xF0 else { return }
        let kind = status & 0xF0, ch = status & 0x0F
        let d1 = m.count > 1 ? m[1] : 0, d2 = m.count > 2 ? m[2] : 0
        if ch == MiniLab.padChannel && (kind == 0x90 || kind == 0x80 || kind == 0xA0) && (36...51).contains(d1) {
            if daw { dawPad(note: d1, velocity: kind == 0x80 ? 0 : d2, pressure: kind == 0xA0) }
            else { pad(note: d1, velocity: kind == 0x80 ? 0 : d2, pressure: kind == 0xA0) }
            return
        }
        if daw && kind == 0xB0 && dawControl(d1, Int(d2)) { return }
        switch kind {
        case 0x80, 0x90:                                         // keys: the active track, or the PANEL layer
            if daw && (panelKeysOn || heldKeys[d1] != nil) { panelKey(note: d1, down: kind == 0x90 && d2 > 0) }
            else { notes.send([kind | MiniLab.autoChannel] + m.dropFirst()) }
        case 0xE0:                                               // pitch bend: the active track
            notes.send([kind | MiniLab.autoChannel] + m.dropFirst())
        case 0xB0:
            if !daw, let enc = MiniLab.knobCC[d1], MiniLab.faderParamCC[enc] != nil {
                fader(enc, d2)
            } else if !daw, let enc = MiniLab.knobCC[d1] {
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
            playPad(Int(note - 35), velocity: velocity, pressure: pressure)
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

    /// Model:Cycles pad p (1-6) with velocity, or pressure while held.
    private func playPad(_ p: Int, velocity: UInt8, pressure: Bool) {
        let down = velocity > 0
        if pressure && !heldPads.contains(p) { return }
        panel.pad(p, down ? max(0.05, Float(velocity) / 127) : 0)
        if pressure { return }
        if down { heldPads.insert(p) } else { heldPads.remove(p) }
    }

    // MARK: DAW mode

    private func sysexIn(_ m: [UInt8]) {
        // F0 00 20 6B 7F 42 02 00 40 <what> <value> F7: 62 = program (02 DAW), 01 = reply to our request (01 DAW)
        guard m.count == 12, Array(m[0..<6]) == MiniLab.sysex, m[6] == 0x02, m[7] == 0x00, m[8] == 0x40 else { return }
        switch (m[9], m[10]) {
        case (0x62, 0x02), (0x01, 0x01): if !daw { enterDAW() }
        case (0x62, _), (0x01, _): leaveDAW()
        case (0x63, let bank): currentBankB = bank == 1             // pad bank, reported on every switch
        default: break
        }
    }

    /// Asks the MiniLab which program it runs; a DAW reply turns DAW mode on.
    private func hello() {
        send(MiniLab.sysex + [0x02, 0x00, 0x40, 0x6A, 0x21, 0xF7])
        send(MiniLab.sysex + [0x01, 0x00, 0x40, 0x01, 0xF7])
    }

    private func enterDAW() {
        releaseAll()
        daw = true
        panelKeysOn = true
        sentColors = [:]
        send(MiniLab.sysex + [0x02, 0x00, 0x40, 0x6A, 0x21, 0xF7])                // colours need a connected DAW
        for (i, _) in MiniLab.dawKnobCC.enumerated() { recentre(i) }
        showLayer()
    }

    private func leaveDAW() {
        guard daw else { return }
        releaseAll()
        daw = false
        lastCC = [:]
    }

    /// Lets go of every key and pad the MiniLab holds down on the board (on a mode change).
    private func releaseAll() {
        for k in Set(heldKeys.values).union(heldPadKeys.values) { panel.key(k, false) }
        for p in heldPads { panel.pad(p, 0) }
        heldKeys = [:]; heldPadKeys = [:]; heldPads = []
        if daw { panel.key(10, false) }                                          // Shift = FUNC
    }

    /// A DAW-mode CC; false if it is not one of the MiniLab's controls (mod strip, sustain).
    private func dawControl(_ cc: UInt8, _ v: Int) -> Bool {
        if let i = MiniLab.dawKnobCC.firstIndex(of: cc) {
            let enc = i + 2
            if let last = lastCC[cc], v != last {
                // a jump of more than 32 is a value sent before our recentre arrived, or the first after it:
                // its side of 64 is the direction the knob turns
                let d = abs(v - last) > 32 ? (v - 64).signum() : v - last
                if d != 0 { step(enc, d); showParameter(cc, enc) }
            }
            lastCC[cc] = v
            if v < 16 || v > 111 { recentre(i) }
            return true
        }
        if let enc = MiniLab.dawFaderCC[cc] {
            fader(enc, UInt8(v))
            showParameter(cc, enc)
            return true
        }
        switch cc {
        case 27: panel.key(10, v > 0)                                            // Shift = FUNC
        case 28, 29: if v != 64 { step(1, v - 64) }                             // main encoder (with Shift = FUNC)
        case 118: panel.key(31, v > 0)                                           // its press
        case 119:                                                                // Shift + press: keys layer
            if v > 0 { panelKeysOn.toggle(); showLayer() }
        case 105...109:                                                          // Shift + pads 4-8: FUNC + pad
            padButton(id: cc, index: Int(cc) - 105 + 3, bankB: currentBankB, down: v > 0)
        default: return false
        }
        return true
    }

    private var currentBankB = false
    private var faderShown: [Int: TimeInterval] = [:]        // encoder -> when a knob event last showed its value
    private var faderMoved: [Int: TimeInterval] = [:]        // encoder -> time of its fader's last move
    private var lastStep: [Int: TimeInterval] = [:]          // encoder -> time of its last step
    private var waking: [Int: TimeInterval] = [:]            // encoder -> end of its wake-up wait
    private var deferred: [Int: Int] = [:]                   // steps held for the end of that wait

    private func dawPad(note: UInt8, velocity: UInt8, pressure: Bool) {
        let bankB = note >= 44
        currentBankB = bankB
        let index = Int(note) - (bankB ? 44 : 36)
        if !bankB && index < 6 { playPad(index + 1, velocity: velocity, pressure: pressure); return }
        if pressure { return }
        padButton(id: note, index: index, bankB: bankB, down: velocity > 0)
    }

    /// Pad `index` (0-7) of a bank as a button; `id` (note or CC) pairs the release with its press.
    private func padButton(id: UInt8, index: Int, bankB: Bool, down: Bool) {
        if !down {
            if let k = heldPadKeys.removeValue(forKey: id) { panel.key(k, false) }
            else if !bankB && index < 6 { playPad(index + 1, velocity: 0, pressure: false) }
            return
        }
        if !bankB && index < 6 { playPad(index + 1, velocity: 100, pressure: false); return }
        guard let k = bankB ? MiniLab.bankBKeys[index] : MiniLab.bankAKeys[index] else { return }
        heldPadKeys[id] = k
        panel.key(k, true)
    }

    private func panelKey(note: UInt8, down: Bool) {
        if !down {
            if let k = heldKeys.removeValue(forKey: note) { panel.key(k, false) }
            return
        }
        guard let k = MiniLab.panelMap[note] else { return }
        heldKeys[note] = k
        panel.key(k, true)
    }

    /// `n` value steps of encoder `enc` on the Model:Cycles (one per MiniLab value: it skips values when turned fast).
    /// The OS ignores the first knob event after a pause, so after one the app sends a wake-up event first and the
    /// steps 40 ms later (two events closer together can count twice); steps that come in meanwhile go with them.
    private func step(_ enc: Int, _ n: Int) {
        let now = ProcessInfo.processInfo.systemUptime
        defer { lastStep[enc] = now }
        if let until = waking[enc], now < until { deferred[enc, default: 0] += n; return }
        if now - (lastStep[enc] ?? 0) < 0.15 { panel.nudge(enc, n); return }
        panel.nudge(enc, n.signum())
        waking[enc] = now + 0.04
        deferred[enc] = n
        DispatchQueue.global(qos: .userInteractive).asyncAfter(deadline: .now() + 0.04) { [weak self] in
            guard let self else { return }
            self.lock.lock()
            if let n = self.deferred.removeValue(forKey: enc), n != 0 { self.panel.nudge(enc, n) }
            self.waking[enc] = nil
            self.lock.unlock()
        }
    }

    /// Fader for encoder `enc` at `v`: its parameter on the active track as MIDI CC. A value set by CC shows nothing
    /// on the screen, so the app also sends a lone knob event, at most every 0.6 s: after a pause the OS only shows
    /// the value for it, without changing it. In case one does count, the value goes again 0.1 s after the last move.
    private func fader(_ enc: Int, _ v: UInt8) {
        guard let param = MiniLab.faderParamCC[enc] else { return }
        let now = ProcessInfo.processInfo.systemUptime
        if now - (faderShown[enc] ?? 0) >= 0.6 { panel.nudge(enc, 1); faderShown[enc] = now }
        notes.send([0xB0 | MiniLab.autoChannel, param, v])
        faderMoved[enc] = now
        DispatchQueue.global(qos: .userInteractive).asyncAfter(deadline: .now() + 0.1) { [weak self] in
            guard let self else { return }
            self.lock.lock()
            if self.faderMoved[enc] == now { self.notes.send([0xB0 | MiniLab.autoChannel, param, v]) }
            self.lock.unlock()
        }
    }

    /// Sets knob i (0-7) back to 64 on the MiniLab, so it can keep turning either way.
    private func recentre(_ i: Int) {
        send(MiniLab.sysex + [0x21, 0x10, 0x00, UInt8(7 + i), 0x00, 64, 0xF7])
        lastCC[MiniLab.dawKnobCC[i]] = 64
    }

    // MARK: display and LEDs

    private func showLayer() {
        display(line1: "Spoke", line2: panelKeysOn ? "Keys: PANEL" : "Keys: NOTES")
    }

    /// A parameter pop-up for the control `cc`, which the MiniLab hides after a moment.
    private func showParameter(_ cc: UInt8, _ enc: Int) {
        send(MiniLab.sysex + [0x04, 0x02, 0x60, 0x1F, 0x08, 0x02, cc, 0x00, 0x01]
             + MiniLab.ascii(MiniLab.encoderNames[enc], 10) + [0x00, 0x02, 0x00, 0xF7])
    }

    private func display(line1: String, line2: String) {
        send(MiniLab.sysex + [0x04, 0x02, 0x60, 0x1F, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01]
             + MiniLab.ascii(line1, 10) + [0x00, 0x02] + MiniLab.ascii(line2, 18) + [0x00, 0xF7])
    }

    private static func ascii(_ s: String, _ width: Int) -> [UInt8] {
        Array(s.unicodeScalars.filter { $0.isASCII && $0.value >= 0x20 }.prefix(width).map { UInt8($0.value) })
    }

    /// Pad colours from the board's LEDs; sends only what changed.
    private func refreshLEDs() {
        let leds = panel.leds()
        func lit(_ bit: Int) -> Bool { leds & (1 << UInt64(bit)) != 0 }
        lock.lock()
        defer { lock.unlock() }
        guard daw else { return }
        for i in 0..<6 {                                                         // A1-A6: dim track colour, bright when lit
            let c = MiniLab.trackColors[i]
            setColor(UInt8(0x34 + i), lit(Front.padLED[i]) ? (127, 127, 127) : (c.0 / 6, c.1 / 6, c.2 / 6))
        }
        setColor(0x3A, lit(Front.keyLED[Front.play]!) ? (0, 127, 0) : (0, 12, 0))
        setColor(0x3B, lit(Front.keyLED[Front.stop]!) ? (127, 0, 0) : (12, 0, 0))
        for (i, k) in MiniLab.bankBKeys.enumerated() {
            let bit = Front.keyLED[k] ?? -1
            setColor(UInt8(0x44 + i), bit >= 0 && lit(bit) ? (127, 90, 0) : (14, 10, 4))
        }
    }

    private func setColor(_ id: UInt8, _ c: (UInt8, UInt8, UInt8)) {
        if let old = sentColors[id], old == c { return }
        sentColors[id] = c
        send(MiniLab.sysex + [0x02, 0x02, 0x16, id, c.0, c.1, c.2, 0xF7])
    }

    private func send(_ msg: [UInt8]) {
        guard outPort != 0, destination != 0 else { return }
        var storage = [UInt8](repeating: 0, count: 64 + msg.count)
        storage.withUnsafeMutableBytes { raw in
            let list = raw.baseAddress!.assumingMemoryBound(to: MIDIPacketList.self)
            let packet = MIDIPacketListInit(list)
            MIDIPacketListAdd(list, raw.count, packet, 0, msg.count, msg)
            MIDISend(outPort, destination, list)
        }
    }
}
