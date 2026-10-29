// AudioOut (app/AudioOut.swift): it keeps playing when the output hardware changes. The system then stops the
// engine and posts AVAudioEngineConfigurationChange (headphones in, another default output, a new sample rate);
// these tests do the same to a silent output on this Mac's default device.
import AVFoundation
import XCTest
@testable import VladiktronSpoke

final class AudioOutTests: XCTestCase {
    private func started() throws -> AudioOut {
        let out = AudioOut(name: "test", read: { _, _ in 0 }, available: { 0 })   // plays silence
        guard out.start() else { throw XCTSkip("no audio output on this machine") }
        return out
    }

    private func wait(until done: () -> Bool, seconds: TimeInterval = 3) {
        let end = Date().addingTimeInterval(seconds)
        while !done() && Date() < end { RunLoop.current.run(until: Date().addingTimeInterval(0.05)) }
    }

    func testRestartsAfterAConfigurationChange() throws {
        let out = try started()
        defer { out.stop() }
        XCTAssertTrue(out.engine.isRunning)
        out.engine.stop()                                                   // as the system does
        NotificationCenter.default.post(name: .AVAudioEngineConfigurationChange, object: out.engine)
        wait { out.engine.isRunning }
        XCTAssertTrue(out.engine.isRunning, "the output stays stopped after a configuration change")
    }

    /// Stopping and starting an engine waits for the hardware (seconds, when a device is switching): never on the main
    /// thread, which would freeze the window.
    func testRestartIsOffTheMainThread() throws {
        let out = try started()
        defer { out.stop() }
        out.engine.stop()
        NotificationCenter.default.post(name: .AVAudioEngineConfigurationChange, object: out.engine)
        wait { out.restartedOnMain != nil }
        XCTAssertEqual(out.restartedOnMain, false, "the restart ran on the main thread")
    }

    func testStoppedOutputStaysStopped() throws {
        let out = try started()
        out.stop()
        NotificationCenter.default.post(name: .AVAudioEngineConfigurationChange, object: out.engine)
        RunLoop.current.run(until: Date().addingTimeInterval(0.5))
        XCTAssertFalse(out.engine.isRunning, "stop() is for good")
    }
}
