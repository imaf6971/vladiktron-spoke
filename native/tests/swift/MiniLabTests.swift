// The MiniLab 3 mapping (app/MiniLab.swift) in DAW mode, fed with messages as the hardware sends them, some replayed
// from recordings (tests/fixtures). The board is a recorder: panel calls and the MIDI the mapping sends to it.
//   swift test   (build.sh runs it)
import XCTest
@testable import VladiktronSpoke

final class MiniLabTests: XCTestCase {
    /// What the mapping did to the board.
    final class Recorder {
        private let lock = NSLock()
        private var _events: [String] = []
        private var _midi: [[UInt8]] = []
        var events: [String] { lock.lock(); defer { lock.unlock() }; return _events }
        var midi: [[UInt8]] { lock.lock(); defer { lock.unlock() }; return _midi }
        func add(_ e: String) { lock.lock(); _events.append(e); lock.unlock() }
        func addMIDI(_ m: [UInt8]) { lock.lock(); _midi.append(m); lock.unlock() }
        /// Total OS knob events sent to encoder `enc`, and whether any went the other way than `sign`.
        func nudges(_ enc: Int) -> [Int] {
            events.compactMap { e in
                let p = e.split(separator: " ")
                return p.count == 3 && p[0] == "nudge" && Int(p[1]) == enc ? Int(p[2]) : nil
            }
        }
        func keyDowns(_ key: Int) -> Int { events.filter { $0 == "key \(key) down" }.count }
    }

    private var rec = Recorder()
    private var lab: MiniLab!

    override func setUp() {
        rec = Recorder()
        let r = rec
        let panel = MiniLab.Panel(key: { r.add("key \($0) \($1 ? "down" : "up")") },
                                  turn: { r.add("turn \($0) \($1)") },
                                  nudge: { r.add("nudge \($0) \($1)") },
                                  pad: { r.add("pad \($0) \($1 > 0 ? "down" : "up")") },
                                  leds: { 0 })
        let notes = MidiPortPair(name: "test", inject: { p, n in r.addMIDI(Array(UnsafeBufferPointer(start: p, count: n))); return n },
                                 drain: { _, _ in 0 })
        lab = MiniLab(panel: panel, notes: notes, onChange: { _ in })
        lab.handle(MiniLab.sysex + [0x02, 0x00, 0x40, 0x62, 0x02, 0xF7])        // the MiniLab reports DAW mode
    }

    private func cc(_ n: UInt8, _ v: UInt8) { lab.handle([0xB0, n, v]) }

    /// Replays a recording: "seconds bytes..." lines, in real time if `timed`.
    private func replay(_ fixture: String, timed: Bool) throws {
        let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("fixtures/\(fixture)")
        var last = 0.0
        for line in try String(contentsOf: url, encoding: .utf8).split(separator: "\n") where !line.hasPrefix("#") {
            let f = line.split(separator: " ")
            guard let t = Double(f[0]) else { continue }
            if timed && t > last { Thread.sleep(forTimeInterval: t - last) }
            last = t
            lab.handle(f.dropFirst().compactMap { UInt8($0, radix: 16) })
        }
    }

    func testKnobSpinAcrossResetsNeverTurnsBack() throws {
        try replay("minilab3-knob1-spin.txt", timed: true)
        Thread.sleep(forTimeInterval: 0.1)                                       // the wake-up's deferred steps
        let n = rec.nudges(2)                                                    // knob 1 = PITCH, encoder 2
        XCTAssertFalse(n.contains { $0 < 0 }, "a forward spin went backwards: \(n)")
        XCTAssertGreaterThan(n.reduce(0, +), 60)
    }

    /// A fast turn: the app resets the knob to 64 at 112, but 114 and 116 were already on their way. They must not
    /// count as jumps of 50 from the reset value.
    func testKnobValuesInFlightDuringAResetAreNoJumps() {
        for v: UInt8 in [100, 104, 108, 112, 114, 116, 66, 70, 74] { cc(86, v); Thread.sleep(forTimeInterval: 0.05) }
        Thread.sleep(forTimeInterval: 0.1)
        let n = rec.nudges(2)
        XCTAssertTrue(n.allSatisfy { (1...8).contains($0) }, "steps should be small and forward: \(n)")
    }

    func testMainEncoderIsOneEventPerClick() {
        cc(28, 65); cc(28, 61); cc(28, 67)                                       // +1, -3 (fast), +3
        XCTAssertEqual(rec.nudges(1), [1, -1, 1])
        cc(118, 127); cc(118, 0)
        XCTAssertEqual(rec.keyDowns(31), 1)                                      // LEVEL/DATA press
    }

    func testPitchStripQuickTapsAllCount() throws {
        cc(27, 127)                                                              // Shift = FUNC: tap tempo
        try replay("minilab3-pitchstrip-taps.txt", timed: false)
        XCTAssertEqual(rec.keyDowns(10), 1)
        XCTAssertEqual(rec.keyDowns(4), 16, "every tap in the recording is one TEMPO press")
        XCTAssertEqual(rec.keyDowns(6), 0, "no tap on the lower half")
        XCTAssertTrue(rec.midi.isEmpty, "PANEL mode sends no pitch bend")
    }

    func testModStripHalves() {
        lab.handle([0xB0, 1, 100])
        Thread.sleep(forTimeInterval: 0.2)
        lab.handle([0xB0, 1, 20])
        XCTAssertEqual(rec.keyDowns(0), 1)                                       // MACHINES
        XCTAssertEqual(rec.keyDowns(3), 1)                                       // LFO
    }

    func testPanelKeys() {
        let want: [UInt8: Int] = [48: 10, 49: 9, 50: 11, 51: 13, 52: 30, 69: 12, 70: 1, 71: 2, 72: 8]
            .merging((0..<16).map { (53 + UInt8($0), 14 + $0) }) { a, _ in a }
        for note in UInt8(48)...72 {
            lab.handle([0x90, note, 100]); lab.handle([0x80, note, 0])
        }
        for (note, key) in want {
            XCTAssertEqual(rec.keyDowns(key), 1, "note \(note) should press key \(key)")
        }
        XCTAssertEqual(rec.events.filter { $0.hasSuffix(" down") }.count, 25)
        XCTAssertEqual(rec.events.filter { $0.hasSuffix(" up") }.count, 25)
    }

    func testFadersSendTheirValue() {
        cc(14, 0); cc(15, 127); cc(30, 64); cc(31, 99)                          // faders 1-4
        let sent = rec.midi
        for want: [UInt8] in [[0xB9, 102, 0], [0xB9, 7, 127], [0xB9, 15, 64], [0xB9, 14, 99]] {
            XCTAssertTrue(sent.contains(want), "missing \(want) in \(sent)")
        }
    }

    func testShiftPadsAreFuncPads() {
        cc(27, 127)
        cc(105, 127); cc(105, 0)                                                 // Shift + pad 4: FUNC + T4 (mute)
        cc(108, 127); cc(108, 0)                                                 // Shift + pad 7: FUNC + PLAY
        XCTAssertTrue(rec.events.contains("pad 4 down") && rec.events.contains("pad 4 up"))
        XCTAssertEqual(rec.keyDowns(7), 1)
    }

    func testPadsBankA() {
        lab.handle([0x99, 36, 90]); lab.handle([0x89, 36, 0])                   // A1 = T1
        lab.handle([0x99, 42, 90]); lab.handle([0x89, 42, 0])                   // A7 = PLAY
        lab.handle([0x99, 43, 90]); lab.handle([0x89, 43, 0])                   // A8 = STOP
        XCTAssertTrue(rec.events.contains("pad 1 down"))
        XCTAssertEqual(rec.keyDowns(7), 1)
        XCTAssertEqual(rec.keyDowns(5), 1)
    }
}
