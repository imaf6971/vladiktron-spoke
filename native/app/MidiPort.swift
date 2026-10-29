// The board's MIDI connections as CoreMIDI virtual ports, as the hardware shows up on a Mac: "Model:Cycles" is
// the USB-MIDI port, "Model:Cycles TRS" the MIDI IN / OUT jacks (as if on a MIDI interface). Other apps send to
// the destinations and listen to the sources. A MiniLab 3, if one is connected, plays the board directly.
import CoreMIDI
import Foundation
import MCBoard

/// One connection: a CoreMIDI destination feeding the board, and a source carrying what the board sends.
final class MidiPortPair {
    let name: String
    private let inject: (UnsafePointer<UInt8>, Int) -> Int    // bytes the board took (its queue may be full)
    private let drain: (UnsafeMutablePointer<UInt8>, Int) -> Int
    private let lock = NSLock()                    // the board's input side takes one writer at a time
    // Bytes the board had no room for yet, oldest first, from `waitingFrom` on (under `lock`). A SysEx restore comes
    // in far faster than the jacks' 31250 baud (or USB) take it; it waits here instead of being lost.
    private var waiting: [UInt8] = []
    private var waitingFrom = 0
    // Real-time bytes (F8-FF: clock, start, stop) do not wait behind them: MIDI lets them go between any two bytes, so
    // a DAW's clock keeps time during a SysEx transfer. The board queues them apart and sends them first; these are
    // the few its real-time queue had no room for yet.
    private var waitingRealTime: [UInt8] = []
    static let waitingLimit = 16 << 20             // past this much waiting, new bytes are dropped (and logged)
    private var parser = MidiStreamParser()
    /// Told when bytes are left waiting, so the owner flushes them soon (MidiPort: within a millisecond).
    var onWaiting: (() -> Void)?
    fileprivate var source = MIDIEndpointRef()
    fileprivate var destination = MIDIEndpointRef()

    init(name: String, inject: @escaping (UnsafePointer<UInt8>, Int) -> Int,
         drain: @escaping (UnsafeMutablePointer<UInt8>, Int) -> Int) {
        self.name = name
        self.inject = inject
        self.drain = drain
    }

    /// Raw MIDI bytes into the board (from CoreMIDI or the controller mapping), after any that still wait.
    func send(_ bytes: UnsafePointer<UInt8>, _ count: Int) {
        lock.lock(); defer { lock.unlock() }
        var taken = 0
        if waitingFrom == waiting.count && waitingRealTime.isEmpty { taken = inject(bytes, count) }  // straight in
        guard taken < count else { return }
        let rest = UnsafeBufferPointer(start: bytes + taken, count: count - taken)
        if waiting.count - waitingFrom + waitingRealTime.count + rest.count > MidiPortPair.waitingLimit {
            NSLog("Spoke: %@: %ld MIDI bytes dropped, %ld already waiting for the board", name, rest.count,
                  waiting.count - waitingFrom + waitingRealTime.count)
            return
        }
        for byte in rest {
            if byte >= 0xF8 { waitingRealTime.append(byte) } else { waiting.append(byte) }
        }
        flushRealTime()
        onWaiting?()
    }

    private func flushRealTime() {                 // (under `lock`)
        guard !waitingRealTime.isEmpty else { return }
        let n = waitingRealTime.withUnsafeBufferPointer { inject($0.baseAddress!, $0.count) }
        waitingRealTime.removeFirst(n)
    }

    /// Feeds the board what waits, as far as it has room (MidiPort calls this every millisecond while anything
    /// waits). Returns whether something still waits.
    @discardableResult
    func flush() -> Bool {
        lock.lock(); defer { lock.unlock() }
        flushRealTime()
        guard waitingFrom < waiting.count else { return !waitingRealTime.isEmpty }
        waitingFrom += waiting.withUnsafeBufferPointer { inject($0.baseAddress! + waitingFrom, $0.count - waitingFrom) }
        if waitingFrom == waiting.count {
            waiting.removeAll(keepingCapacity: true); waitingFrom = 0
        } else if waitingFrom > 65536 && waitingFrom * 2 > waiting.count {
            waiting.removeFirst(waitingFrom); waitingFrom = 0
        }
        return waitingFrom < waiting.count || !waitingRealTime.isEmpty
    }
    func send(_ bytes: [UInt8]) { bytes.withUnsafeBufferPointer { send($0.baseAddress!, $0.count) } }

    fileprivate func create(client: MIDIClientRef) {
        MIDISourceCreate(client, name as CFString, &source)
        MIDIDestinationCreateWithBlock(client, name as CFString, &destination) { [weak self] list, _ in
            for packet in list.unsafeSequence() {                    // data may run past the 256-byte field
                let bytes = UnsafeRawPointer(packet) + MemoryLayout<MIDIPacket>.offset(of: \MIDIPacket.data)!
                self?.send(bytes.assumingMemoryBound(to: UInt8.self), Int(packet.pointee.length))
            }
        }
    }

    /// Feeds the board what waits and sends on everything it has sent. Returns whether input still waits.
    fileprivate func pump(_ buf: UnsafeMutablePointer<UInt8>, _ cap: Int) -> Bool {
        let more = flush()
        while true {
            let n = drain(buf, cap)
            for i in 0..<n { if let msg = parser.feed(buf[i]) { emit(msg) } }
            if n < cap { return more }
        }
    }

    private func emit(_ msg: [UInt8]) {
        // Always fits: the list has room for the message, and a message is at most MidiStreamParser.sysexChunk bytes
        // (far below a packet's 64 KB). (Swift sees MIDIPacketListAdd's result as never nil, so it cannot be checked.)
        var storage = [UInt8](repeating: 0, count: 64 + msg.count)
        storage.withUnsafeMutableBytes { raw in
            let list = raw.baseAddress!.assumingMemoryBound(to: MIDIPacketList.self)
            let packet = MIDIPacketListInit(list)
            MIDIPacketListAdd(list, raw.count, packet, mach_absolute_time(), msg.count, msg)
            MIDIReceived(source, list)
        }
    }

    fileprivate func dispose() {
        if destination != 0 { MIDIEndpointDispose(destination) }
        if source != 0 { MIDIEndpointDispose(source) }
        source = 0; destination = 0
    }
}

final class MidiPort {
    private var client = MIDIClientRef()
    private var pairs: [MidiPortPair] = []
    private var board: BoardHandle?
    private var wake: MidiWake?
    private let queue = DispatchQueue(label: "Vladiktron Spoke MIDI out", qos: .userInteractive)

    /// What the board pokes when its MIDI OUT has bytes (mcb_on_midi_out, from the CPU thread: one atomic merge).
    private final class MidiWake {
        let source: DispatchSourceUserDataOr
        init(queue: DispatchQueue) { source = DispatchSource.makeUserDataOrSource(queue: queue) }
    }
    private(set) var controller: MiniLab?

    func start(board h: BoardHandle, panel: MiniLab.Panel, onControllerChange: @escaping (String?) -> Void) {
        // (a board that is gone takes everything: nothing waits for it)
        let usb = MidiPortPair(name: "Model:Cycles", inject: { p, n in h.with { mcb_usb_midi_in($0, p, n) } ?? n },
                               drain: { p, n in h.with { mcb_usb_midi_out($0, p, n) } ?? 0 })
        let trs = MidiPortPair(name: "Model:Cycles TRS", inject: { p, n in h.with { mcb_midi_in($0, p, n) } ?? n },
                               drain: { p, n in h.with { mcb_midi_out($0, p, n) } ?? 0 })
        pairs = [usb, trs]
        let lab = MiniLab(panel: panel, notes: trs, onChange: onControllerChange)
        controller = lab
        guard MIDIClientCreateWithBlock("Vladiktron Spoke" as CFString, &client, { [weak lab] note in
            if note.pointee.messageID == .msgSetupChanged { lab?.rescan() }
        }) == noErr else {
            NSLog("Spoke: CoreMIDI unavailable")
            return
        }
        for p in pairs { p.create(client: client) }
        lab.attach(client: client, ownSources: pairs.map(\.source))

        // Nothing polls while MIDI is quiet (a 1 ms timer used to wake the Mac 1,000 times a second, which costs
        // battery). The board pokes `wake` as the OS sends MIDI OUT bytes, which go on at once; input the board had
        // no room for is retried every millisecond until it is all in.
        let wake = MidiWake(queue: queue)
        let source = wake.source, queue = self.queue
        let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: 1024)
        source.setEventHandler { [pairs] in
            var waiting = false
            for p in pairs where p.pump(buf, 1024) { waiting = true }
            if waiting { queue.asyncAfter(deadline: .now() + .milliseconds(1)) { source.or(data: 1) } }
        }
        source.resume()
        for p in pairs { p.onWaiting = { source.or(data: 1) } }
        // (retained for good: the CPU thread may still be inside a call with it just after mcb_on_midi_out(nil))
        let ctx = Unmanaged.passRetained(wake).toOpaque()
        h.with { mcb_on_midi_out($0, { ctx in Unmanaged<MidiWake>.fromOpaque(ctx!).takeUnretainedValue().source.or(data: 1) }, ctx) }
        board = h
        self.wake = wake
        source.or(data: 1)                                                  // anything sent before we listened
    }

    func stop() {
        board?.with { mcb_on_midi_out($0, nil, nil) }
        board = nil
        for p in pairs { p.onWaiting = nil }
        wake?.source.cancel()
        wake = nil
        queue.sync {}                                                   // a running drain finishes first
        controller?.detach()
        for p in pairs { p.dispose() }
        if client != 0 { MIDIClientDispose(client) }
        client = 0
    }
}

/// Splits a MIDI byte stream into whole messages: expands running status, passes real-time bytes through on
/// their own (even inside another message), and collects SysEx: up to `sysexChunk` bytes at a time, so a long one
/// goes on in pieces (as SysEx from hardware arrives) and never has to sit whole in memory.
struct MidiStreamParser {
    static let sysexChunk = 1024
    private var status: UInt8 = 0
    private var data: [UInt8] = []
    private var sysex: [UInt8]?                                // in a SysEx: what has not gone on yet

    mutating func feed(_ byte: UInt8) -> [UInt8]? {
        if byte >= 0xF8 { return [byte] }
        if byte == 0xF0 { sysex = [byte]; status = 0; return nil }
        if byte == 0xF7 {
            guard var s = sysex else { return nil }
            sysex = nil
            s.append(byte)
            return s
        }
        if byte >= 0x80 {
            sysex = nil
            status = byte
            data = []
            if MidiStreamParser.length(byte) == 0 { status = 0; return [byte] }
            return nil
        }
        if sysex != nil {
            sysex!.append(byte)
            guard sysex!.count == MidiStreamParser.sysexChunk else { return nil }
            defer { sysex = [] }
            return sysex
        }
        guard status != 0 else { return nil }
        data.append(byte)
        guard data.count == MidiStreamParser.length(status) else { return nil }
        let msg = [status] + data
        data = []
        if status >= 0xF0 { status = 0 }                      // system common: no running status
        return msg
    }

    static func length(_ status: UInt8) -> Int {
        switch status & 0xF0 {
        case 0xC0, 0xD0: return 1
        case 0xF0:
            switch status { case 0xF1, 0xF3: return 1; case 0xF2: return 2; default: return 0 }
        default: return 2
        }
    }
}
