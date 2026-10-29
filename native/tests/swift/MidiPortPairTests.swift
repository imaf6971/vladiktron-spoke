// MidiPortPair (app/MidiPort.swift): MIDI into the board waits when the board's queue is full, instead of being lost
// (a SysEx restore arrives far faster than 31250 baud drains), and comes out of it in order.
import XCTest
@testable import VladiktronSpoke

final class MidiPortPairTests: XCTestCase {
    /// A pair whose board takes at most `room` bytes per call, like a ring the OS reads slowly.
    private func pair(room: Int, into received: @escaping ([UInt8]) -> Void) -> MidiPortPair {
        MidiPortPair(name: "test", inject: { p, n in
            let k = min(n, room)
            received(Array(UnsafeBufferPointer(start: p, count: k)))
            return k
        }, drain: { _, _ in 0 })
    }

    func testABigSysExArrivesWholeAndInOrder() {
        var got: [UInt8] = []
        let p = pair(room: 100) { got += $0 }
        let sysex: [UInt8] = [0xF0] + (0..<20_000).map { UInt8($0 & 0x7F) } + [0xF7]
        p.send(sysex)
        for _ in 0..<1000 where got.count < sysex.count { p.flush() }   // the 1 ms MIDI timer
        XCTAssertEqual(got, sysex)
    }

    func testLaterMessagesWaitBehindEarlierOnes() {
        var got: [UInt8] = []
        let p = pair(room: 10) { got += $0 }
        let first = [UInt8](repeating: 0x7F, count: 50), note: [UInt8] = [0x90, 60, 100]
        p.send([0xF0] + first + [0xF7])
        p.send(note)
        for _ in 0..<100 { p.flush() }
        XCTAssertEqual(got, [0xF0] + first + [0xF7] + note)
    }
}
