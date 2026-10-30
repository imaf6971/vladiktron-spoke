// BoardHandle (app/Board.swift): calls from MIDI and MiniLab timers reach the C board only while it exists. Board.stop
// closes the handle before mcb_destroy, so a timer that fires later (a MiniLab pad tap's release, 110 ms on) does
// nothing instead of writing into freed memory. A dummy pointer stands in for the board: it is never dereferenced.
import XCTest
@testable import VladiktronSpoke

final class BoardHandleTests: XCTestCase {
    private let dummy = OpaquePointer(bitPattern: 0x1000)!

    func testCallsReachTheBoardUntilClosed() {
        let h = BoardHandle(dummy)
        var calls = 0
        h.with { _ in calls += 1 }
        XCTAssertEqual(h.close(), dummy)
        h.with { _ in calls += 1 }                                   // a late timer
        XCTAssertEqual(calls, 1)
        XCTAssertNil(h.close(), "closing twice gives the board back once")
    }

    func testCloseWaitsForACallInProgress() {
        let h = BoardHandle(dummy)
        let inside = expectation(description: "call started")
        var finished = false
        DispatchQueue.global().async {
            h.with { _ in inside.fulfill(); Thread.sleep(forTimeInterval: 0.2); finished = true }
        }
        wait(for: [inside], timeout: 2)
        _ = h.close()                                               // Board.stop, then mcb_destroy
        XCTAssertTrue(finished, "close returned while a call was still using the board")
    }

    func testMiniLabTimersAfterCloseDoNothing() {
        let h = BoardHandle(dummy)
        var keys: [String] = []
        let lock = NSLock()
        let panel = MiniLab.Panel(key: { k, d in h.with { _ in lock.lock(); keys.append("\(k) \(d)"); lock.unlock() } },
                                  turn: { _, _ in }, nudge: { _, _ in }, pad: { _, _ in }, leds: { 0 })
        let notes = MidiPortPair(name: "test", inject: { _, n in n }, drain: { _, _ in 0 })
        let lab = MiniLab(panel: panel, notes: notes, onChange: { _ in })
        lab.handle([0x99, 44, 100])                                  // Arturia mode, pad B1: TRACK + pad, released later
        _ = h.close()                                               // the board goes away right after the tap
        Thread.sleep(forTimeInterval: 0.3)                          // the release timer fires (110 ms)
        lock.lock(); defer { lock.unlock() }
        XCTAssertEqual(keys, ["13 true"], "the TRACK release reached a closed board")
    }
}
