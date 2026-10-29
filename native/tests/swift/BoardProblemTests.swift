// Board.problem (app/Board.swift): what the panel tells the user when the Model:Cycles stopped.
import XCTest
@testable import VladiktronSpoke

final class BoardProblemTests: XCTestCase {
    func testHealthyBoardHasNoProblem() {
        XCTAssertNil(Board.problem(cpuStopped: false, fault: "", osExceptions: 0, busySeconds: 0))
    }

    func testCpuFaultSaysWhy() {
        let p = Board.problem(cpuStopped: true, fault: "unmapped access 0x20000000 at pc 0x400004e8", osExceptions: 0,
                                  busySeconds: 0)
        XCTAssertNotNil(p)
        XCTAssertTrue(p!.contains("unmapped access 0x20000000"), p!)
    }

    func testCpuStoppedWithoutAReason() {
        XCTAssertNotNil(Board.problem(cpuStopped: true, fault: "", osExceptions: 0, busySeconds: 0))
    }

    /// The OS ran without a pause in its idle loop for longer than Board.hangSeconds (OS 1.13's longest stretch measured:
    /// 0.14 s, formatting a new +Drive at boot).
    func testAnOsBusyForLongHasStoppedResponding() {
        XCTAssertNil(Board.problem(cpuStopped: false, fault: "", osExceptions: 0, busySeconds: 2))
        let p = Board.problem(cpuStopped: false, fault: "", osExceptions: 0, busySeconds: Board.hangSeconds + 1)
        XCTAssertTrue(p?.contains("stopped responding") == true, p ?? "nil")
    }

    func testOsExceptionIsAProblemWhileTheCpuRuns() {
        let p = Board.problem(cpuStopped: false, fault: "", osExceptions: 1, busySeconds: 30)
        XCTAssertNotNil(p)
        XCTAssertTrue(p!.contains("EXCEPTION"), p!)
    }
}
