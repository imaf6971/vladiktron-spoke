// The board's MIDI connections as CoreMIDI virtual ports, as the hardware shows up on a Mac: "Model:Cycles" is
// the USB-MIDI port, "Model:Cycles TRS" the MIDI IN / OUT jacks (as if on a MIDI interface). Other apps send to
// the destinations and listen to the sources. A MiniLab 3, if one is connected, plays the board directly.
import CoreMIDI
import Foundation

/// One connection: a CoreMIDI destination feeding the board, and a source carrying what the board sends.
final class MidiPortPair {
    let name: String
    private let inject: (UnsafePointer<UInt8>, Int) -> Void
    private let drain: (UnsafeMutablePointer<UInt8>, Int) -> Int
    private let lock = NSLock()                    // the board's input side takes one writer at a time
    private var parser = MidiStreamParser()
    fileprivate var source = MIDIEndpointRef()
    fileprivate var destination = MIDIEndpointRef()

    init(name: String, inject: @escaping (UnsafePointer<UInt8>, Int) -> Void,
         drain: @escaping (UnsafeMutablePointer<UInt8>, Int) -> Int) {
        self.name = name
        self.inject = inject
        self.drain = drain
    }

    /// Raw MIDI bytes into the board (from CoreMIDI or the controller mapping).
    func send(_ bytes: UnsafePointer<UInt8>, _ count: Int) {
        lock.lock(); inject(bytes, count); lock.unlock()
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
        let n = drain(buf, cap)
        for i in 0..<n { if let msg = parser.feed(buf[i]) { emit(msg) } }
    }

    private func emit(_ msg: [UInt8]) {
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
    private var timer: DispatchSourceTimer?
    private let queue = DispatchQueue(label: "Model Mac MIDI out", qos: .userInteractive)
    private(set) var controller: MiniLab?

    func start(board b: OpaquePointer, panel: MiniLab.Panel, onControllerChange: @escaping (String?) -> Void) {
        let usb = MidiPortPair(name: "Model:Cycles", inject: { mcb_usb_midi_in(b, $0, $1) },
                               drain: { mcb_usb_midi_out(b, $0, $1) })
        let trs = MidiPortPair(name: "Model:Cycles TRS", inject: { mcb_midi_in(b, $0, $1) },
                               drain: { mcb_midi_out(b, $0, $1) })
        pairs = [usb, trs]
        let lab = MiniLab(panel: panel, notes: trs, onChange: onControllerChange)
        controller = lab
        guard MIDIClientCreateWithBlock("Model Mac" as CFString, &client, { [weak lab] note in
            if note.pointee.messageID == .msgSetupChanged { lab?.rescan() }
        }) == noErr else {
            NSLog("Model Mac: CoreMIDI unavailable")
            return
        }
        for p in pairs { p.create(client: client) }
        lab.attach(client: client, ownSources: pairs.map(\.source))

        // MIDI OUT: drain the board every millisecond, so clock bytes keep their timing.
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
/// their own (even inside another message), and collects SysEx.
struct MidiStreamParser {
    private var status: UInt8 = 0
    private var data: [UInt8] = []
    private var sysex: [UInt8]?

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
        if sysex != nil { sysex!.append(byte); return nil }
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
