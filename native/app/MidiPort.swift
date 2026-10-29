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
    static let waitingLimit = 16 << 20             // past this much waiting, new bytes are dropped (and logged)
    private var parser = MidiStreamParser()
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
        if waitingFrom == waiting.count { taken = inject(bytes, count) }      // nothing waits: straight in
        guard taken < count else { return }
        if waiting.count - waitingFrom + count - taken > MidiPortPair.waitingLimit {
            NSLog("Spoke: %@: %ld MIDI bytes dropped, %ld already waiting for the board", name, count - taken,
                  waiting.count - waitingFrom)
            return
        }
        waiting.append(contentsOf: UnsafeBufferPointer(start: bytes + taken, count: count - taken))
    }

    /// Feeds the board what waits, as far as it has room (the MIDI timer calls this every millisecond).
    func flush() {
        lock.lock(); defer { lock.unlock() }
        guard waitingFrom < waiting.count else { return }
        waitingFrom += waiting.withUnsafeBufferPointer { inject($0.baseAddress! + waitingFrom, $0.count - waitingFrom) }
        if waitingFrom == waiting.count {
            waiting.removeAll(keepingCapacity: true); waitingFrom = 0
        } else if waitingFrom > 65536 && waitingFrom * 2 > waiting.count {
            waiting.removeFirst(waitingFrom); waitingFrom = 0
        }
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

    fileprivate func pump(_ buf: UnsafeMutablePointer<UInt8>, _ cap: Int) {
        flush()
        let n = drain(buf, cap)
        for i in 0..<n { if let msg = parser.feed(buf[i]) { emit(msg) } }
    }

    private func emit(_ msg: [UInt8]) {
        var storage = [UInt8](repeating: 0, count: 64 + msg.count)
        storage.withUnsafeMutableBytes { raw in
            let list = raw.baseAddress!.assumingMemoryBound(to: MIDIPacketList.self)
            let packet = MIDIPacketListInit(list)
            guard MIDIPacketListAdd(list, raw.count, packet, mach_absolute_time(), msg.count, msg) != nil else {
                NSLog("Spoke: %@: a %ld-byte MIDI message did not fit a packet", name, msg.count)
                return
            }
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
    private var timer: DispatchSourceTimer?
    private let queue = DispatchQueue(label: "Vladiktron Spoke MIDI out", qos: .userInteractive)
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

        // Every millisecond: feed the board what waits, and drain its MIDI OUT, so clock bytes keep their timing.
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: .milliseconds(1), leeway: .microseconds(200))
        let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: 1024)
        t.setEventHandler { [pairs] in for p in pairs { p.pump(buf, 1024) } }
        t.resume()
        timer = t
    }

    func stop() {
        timer?.cancel()
        timer = nil
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
