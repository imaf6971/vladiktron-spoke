// MidiStreamParser (app/MidiPort.swift): the byte stream the board sends, split into the whole messages that go out
// on the CoreMIDI sources.
import XCTest
@testable import VladiktronSpoke

final class MidiStreamParserTests: XCTestCase {
    private func messages(_ bytes: [UInt8]) -> [[UInt8]] {
        var p = MidiStreamParser()
        return bytes.compactMap { p.feed($0) }
    }

    func testRunningStatusIsExpanded() {
        XCTAssertEqual(messages([0x90, 60, 100, 62, 90]), [[0x90, 60, 100], [0x90, 62, 90]])
    }

    func testRealTimeBytesPassInsideAMessage() {
        XCTAssertEqual(messages([0x90, 60, 0xF8, 100]), [[0xF8], [0x90, 60, 100]])
        XCTAssertEqual(messages([0xF0, 1, 0xF8, 2, 0xF7]), [[0xF8], [0xF0, 1, 2, 0xF7]])
    }

    func testOneDataByteMessages() {
        XCTAssertEqual(messages([0xC3, 5, 6]), [[0xC3, 5], [0xC3, 6]])           // program change, running status
    }

    func testSystemCommonHasNoRunningStatus() {
        XCTAssertEqual(messages([0xF2, 1, 2, 3, 4]), [[0xF2, 1, 2]])
        XCTAssertEqual(messages([0xF6]), [[0xF6]])
    }

    func testSysExIsCollectedAndAStatusByteCancelsIt() {
        XCTAssertEqual(messages([0xF0, 0x00, 0x20, 0x3C, 0xF7]), [[0xF0, 0x00, 0x20, 0x3C, 0xF7]])
        XCTAssertEqual(messages([0xF0, 1, 2, 0x80, 60, 0]), [[0x80, 60, 0]])
        XCTAssertEqual(messages([0xF7, 0x90, 1, 2]), [[0x90, 1, 2]])               // a stray F7 is dropped
    }

    /// Long SysEx goes on in pieces of at most `sysexChunk` bytes (CoreMIDI packets end at 64 KB, and a dump need not
    /// sit whole in memory); together they are the message.
    func testLongSysExGoesOnInPieces() {
        let sysex: [UInt8] = [0xF0] + (0..<100_000).map { UInt8($0 & 0x7F) } + [0xF7]
        let pieces = messages(sysex)
        XCTAssertGreaterThan(pieces.count, 1)
        XCTAssertTrue(pieces.allSatisfy { $0.count <= MidiStreamParser.sysexChunk }, "a piece is too long")
        XCTAssertEqual(Array(pieces.joined()), sysex)
    }

    func testDataWithoutStatusIsDropped() {
        XCTAssertEqual(messages([1, 2, 3]), [])
    }
}
