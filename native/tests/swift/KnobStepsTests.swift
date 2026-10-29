// KnobSteps (app/KnobSteps.swift): one OS knob event per value step, with a wake-up event only after a pause.
import XCTest
@testable import VladiktronSpoke

final class KnobStepsTests: XCTestCase {
    private final class Recorder: @unchecked Sendable {
        private let lock = NSLock()
        private var _nudges: [String] = []
        var nudges: [String] { lock.lock(); defer { lock.unlock() }; return _nudges }
        func add(_ e: Int, _ n: Int) { lock.lock(); _nudges.append("\(e) \(n)"); lock.unlock() }
    }

    func testAfterAPauseAWakeUpGoesFirstThenTheSteps() {
        let r = Recorder()
        let k = KnobSteps(nudge: r.add)
        k.step(2, 1)
        XCTAssertEqual(r.nudges, ["2 1"], "the wake-up event at once")
        k.step(2, 1)                                    // within the 40 ms wait: held
        Thread.sleep(forTimeInterval: 0.1)
        XCTAssertEqual(r.nudges, ["2 1", "2 2"], "the steps 40 ms later, together")
        k.step(2, -1)
        XCTAssertEqual(r.nudges, ["2 1", "2 2", "2 -1"], "while the knob moves, one event per step at once")
    }

    func testLevelDataGetsNoWakeUp() {
        let r = Recorder()
        let k = KnobSteps(nudge: r.add)
        k.step(1, 3)
        XCTAssertEqual(r.nudges, ["1 3"])
    }
}
